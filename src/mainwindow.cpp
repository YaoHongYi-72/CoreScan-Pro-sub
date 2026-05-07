#include "mainwindow.h"
#include "spectrallibrary.h"
#include "spectrumdialog.h"
#include "envidataset.h"

#include <QApplication>
#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QSplitter>
#include <QTabWidget>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QProgressDialog>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QtConcurrent>

// ---------- helpers ----------

static bool parseSpectrumFile(const QString& path,
                               QVector<double>& outWl,
                               QVector<double>& outVals,
                               QString& outErr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        outErr = QObject::tr("Cannot open file: %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    // Skip header lines (non-numeric first token)
    // Support comma or whitespace delimited
    bool isCsv = path.endsWith(".csv", Qt::CaseInsensitive);
    outWl.clear(); outVals.clear();
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        QStringList parts = isCsv ? line.split(',') : line.split(QRegularExpression("\\s+"));
        if (parts.size() < 2) continue;
        bool okW, okV;
        double w = parts[0].trimmed().toDouble(&okW);
        double v = parts[1].trimmed().toDouble(&okV);
        if (!okW || !okV) continue;
        outWl.append(w);
        outVals.append(v);
    }
    if (outWl.isEmpty()) {
        outErr = QObject::tr("No valid wavelength/reflectance pairs found in file.");
        return false;
    }
    return true;
}

// ---------- MainWindow ----------

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("CoreScan Pro — Spectral Matching");
    resize(1100, 720);
    setupUi();
    setupMenus();
    updateStatusBar(tr("Ready. Load a spectral library to begin."));
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi()
{
    auto* central  = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(6, 6, 6, 6);

    // ---- left: tabs (ENVI | Spectrum file) ----
    m_tabWidget = new QTabWidget;
    setupEnviTab();
    setupSpectrumTab();

    // ---- right: library panel + match results ----
    auto* rightWidget  = new QWidget;
    auto* rightLayout  = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    setupLibraryPanel();

    auto* matchGroup = new QGroupBox(tr("Match Results"));
    matchGroup->setMinimumHeight(200);
    auto* matchLayout = new QVBoxLayout(matchGroup);
    m_matchList = new QListWidget;
    m_matchList->setAlternatingRowColors(true);
    matchLayout->addWidget(m_matchList);
    connect(m_matchList, &QListWidget::currentRowChanged,
            this, &MainWindow::onMatchResultClicked);

    rightLayout->addWidget(m_libraryList->parentWidget()); // group box added in setupLibraryPanel
    rightLayout->addWidget(matchGroup);
    rightWidget->setMinimumWidth(300);
    rightWidget->setMaximumWidth(380);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(m_tabWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    rootLayout->addWidget(splitter);
    setCentralWidget(central);
    setStatusBar(new QStatusBar(this));
}

void MainWindow::setupMenus()
{
    auto* fileMenu = menuBar()->addMenu(tr("File"));
    auto* actOpenEnvi   = fileMenu->addAction(tr("Open ENVI File (.hdr)..."));
    auto* actImportSpec = fileMenu->addAction(tr("Import Spectrum File (txt/csv)..."));
    auto* actLoadLib    = fileMenu->addAction(tr("Load Spectral Library..."));
    fileMenu->addSeparator();
    auto* actQuit = fileMenu->addAction(tr("Quit"));

    auto* helpMenu = menuBar()->addMenu(tr("Help"));
    auto* actAbout = helpMenu->addAction(tr("About"));

    connect(actOpenEnvi,   &QAction::triggered, this, &MainWindow::onOpenEnvi);
    connect(actImportSpec, &QAction::triggered, this, &MainWindow::onImportSpectrum);
    connect(actLoadLib,    &QAction::triggered, this, &MainWindow::onLoadLibrary);
    connect(actQuit,       &QAction::triggered, qApp, &QApplication::quit);
    connect(actAbout,      &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::setupEnviTab()
{
    auto* tab    = new QWidget;
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Display Band:")));
    m_bandCombo = new QComboBox;
    m_bandCombo->setMinimumWidth(160);
    topRow->addWidget(m_bandCombo);
    topRow->addStretch();
    m_coordLabel = new QLabel(tr("Click image to analyze pixel"));
    topRow->addWidget(m_coordLabel);
    layout->addLayout(topRow);

    m_enviImage = new QLabel(tr("No ENVI file loaded.\n\nUse File → Open ENVI File (.hdr)..."));
    m_enviImage->setAlignment(Qt::AlignCenter);
    m_enviImage->setMinimumSize(400, 400);
    m_enviImage->setStyleSheet("background:#1e2030; color:#8892b0; border:1px solid #3a3f5c;");
    m_enviImage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_enviImage, 1);

    connect(m_bandCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onEnviBandChanged);

    m_tabWidget->addTab(tab, tr("ENVI Image"));

    // Watcher for async pixel analysis
    m_enviWatcher = new QFutureWatcher<PixelMatchResult>(this);
    connect(m_enviWatcher, &QFutureWatcher<PixelMatchResult>::finished,
            this, &MainWindow::onEnviAnalysisFinished);
}

void MainWindow::setupSpectrumTab()
{
    auto* tab    = new QWidget;
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* topRow = new QHBoxLayout;
    m_specFileLabel = new QLabel(tr("No spectrum file loaded."));
    m_specFileLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_analyzeBtn = new QPushButton(tr("Analyze"));
    m_analyzeBtn->setEnabled(false);
    topRow->addWidget(m_specFileLabel);
    topRow->addWidget(m_analyzeBtn);
    layout->addLayout(topRow);

    // Chart placeholder — SpectrumDialog shows the real chart
    m_chartPlaceholder = new QLabel(
        tr("Imported spectrum will be shown here.\n"
           "Use File → Import Spectrum File to load a txt or csv spectrum."));
    m_chartPlaceholder->setAlignment(Qt::AlignCenter);
    m_chartPlaceholder->setMinimumSize(400, 400);
    m_chartPlaceholder->setStyleSheet("background:#1e2030; color:#8892b0; border:1px solid #3a3f5c;");
    m_chartPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_chartPlaceholder, 1);

    connect(m_analyzeBtn, &QPushButton::clicked, this, &MainWindow::onAnalyzeImported);

    m_tabWidget->addTab(tab, tr("Spectrum File"));

    m_importWatcher = new QFutureWatcher<PixelMatchResult>(this);
    connect(m_importWatcher, &QFutureWatcher<PixelMatchResult>::finished,
            this, &MainWindow::onImportAnalysisFinished);
}

void MainWindow::setupLibraryPanel()
{
    // We create the group box here; setupUi() retrieves it via m_libraryList->parentWidget()
    auto* group  = new QGroupBox(tr("Spectral Library"));
    auto* layout = new QVBoxLayout(group);

    m_libraryStatus = new QLabel(tr("No library loaded."));
    m_libraryStatus->setWordWrap(true);
    layout->addWidget(m_libraryStatus);

    auto* loadBtn = new QPushButton(tr("Load Library..."));
    layout->addWidget(loadBtn);

    m_libraryList = new QListWidget;
    m_libraryList->setAlternatingRowColors(true);
    layout->addWidget(m_libraryList);
    connect(loadBtn, &QPushButton::clicked, this, &MainWindow::onLoadLibrary);
    connect(m_libraryList, &QListWidget::currentRowChanged,
            this, &MainWindow::onLibraryItemDoubleClicked);
}

void MainWindow::updateStatusBar(const QString& msg)
{
    statusBar()->showMessage(msg);
}

// ---------- File menu handlers ----------

void MainWindow::onOpenEnvi()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("Open ENVI Header"), QString(),
        tr("ENVI Header (*.hdr);;All Files (*)"));
    if (path.isEmpty()) return;

    try {
        auto ds = std::make_unique<EnviDataset>();
        if (!ds->load(path)) {
            QMessageBox::critical(this, tr("Open ENVI Error"), tr("Failed to load: %1").arg(path));
            return;
        }
        m_dataset = std::move(ds);

        m_bandCombo->blockSignals(true);
        m_bandCombo->clear();
        const auto meta = m_dataset->meta();
        const auto& wls = meta.wavelengths;
        for (int b = 0; b < meta.bands; ++b) {
            QString label = (b < wls.size())
                ? QString("%1 nm").arg(wls[b], 0, 'f', 1)
                : QString("Band %1").arg(b + 1);
            m_bandCombo->addItem(label);
        }
        m_bandCombo->blockSignals(false);

        // Trigger display of band 0
        onEnviBandChanged(0);
        m_tabWidget->setCurrentIndex(0);
        updateStatusBar(tr("Loaded: %1  [%2 x %3, %4 bands]")
            .arg(QFileInfo(path).fileName())
            .arg(meta.samples).arg(meta.lines).arg(meta.bands));
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, tr("Open ENVI Error"), QString::fromStdString(ex.what()));
    }
}

void MainWindow::onImportSpectrum()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("Import Spectrum"), QString(),
        tr("Spectrum Files (*.txt *.csv);;All Files (*)"));
    if (path.isEmpty()) return;

    QString err;
    if (!parseSpectrumFile(path, m_importedWl, m_importedVals, err)) {
        QMessageBox::warning(this, tr("Import Error"), err);
        return;
    }
    m_importedLabel = QFileInfo(path).baseName();
    m_specFileLabel->setText(tr("%1  [%2 points]").arg(m_importedLabel).arg(m_importedWl.size()));
    m_analyzeBtn->setEnabled(true);
    m_tabWidget->setCurrentIndex(1);

    m_chartPlaceholder->setText(
        tr("Loaded: %1\n%2 points, %.1f – %.1f nm\n\nClick \"Analyze\" to match against library.")
        .arg(m_importedLabel)
        .arg(m_importedWl.size())
        .arg(m_importedWl.front())
        .arg(m_importedWl.back()));
    updateStatusBar(tr("Imported spectrum: %1 (%2 points)").arg(m_importedLabel).arg(m_importedWl.size()));
}

void MainWindow::onLoadLibrary()
{
    QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Load Spectral Library"), QString(),
        tr("Spectrum Files (*.txt *.csv);;All Files (*)"));
    if (paths.isEmpty()) return;

    auto* prog = new QProgressDialog(tr("Loading library..."), tr("Cancel"), 0, paths.size(), this);
    prog->setWindowModality(Qt::WindowModal);
    prog->show();
    QApplication::processEvents();

    int loaded = 0;
    auto& lib = SpectralLibrary::instance();
    for (int i = 0; i < paths.size(); ++i) {
        prog->setValue(i);
        QApplication::processEvents();
        if (prog->wasCanceled()) break;

        if (paths[i].endsWith(".csv", Qt::CaseInsensitive))
            loaded += lib.loadCSV(paths[i]) ? 1 : 0;
        else
            loaded += lib.loadUSGSTxt(paths[i]);
    }
    prog->setValue(paths.size());
    prog->deleteLater();

    // Refresh library list
    m_libraryList->clear();
    const auto& spectra = lib.spectra();
    for (const auto& e : spectra) {
        m_libraryList->addItem(e.name);
    }
    m_libraryStatus->setText(tr("%1 spectra loaded").arg(spectra.size()));
    updateStatusBar(tr("Library: %1 reference spectra").arg(spectra.size()));
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About CoreScan Pro — Spectral Matching"),
        tr("<b>CoreScan Pro Spectral Matching Tool</b><br><br>"
           "Standalone spectral analysis module extracted from CoreScan Pro.<br><br>"
           "Supports:<br>"
           "&bull; ENVI hyperspectral image files (.hdr)<br>"
           "&bull; Direct spectrum import (txt / csv)<br>"
           "&bull; SAM + FCLS mineral matching<br>"
           "&bull; Continuum-removal preprocessing<br><br>"
           "Powered by Qt6 &amp; Eigen3."));
}

// ---------- ENVI tab ----------

void MainWindow::onEnviBandChanged(int idx)
{
    if (!m_dataset || idx < 0) return;
    QImage img = m_dataset->getBandImage(idx);
    QSize maxSz = m_enviImage->size().isEmpty()
        ? QSize(800, 600) : m_enviImage->size();
    QPixmap pm = QPixmap::fromImage(img).scaled(maxSz, Qt::KeepAspectRatio, Qt::FastTransformation);
    m_enviImage->setPixmap(pm);
}

void MainWindow::onEnviPixelClicked(int sample, int line)
{
    if (!m_dataset) return;
    if (m_enviWatcher->isRunning()) return;

    m_coordLabel->setText(tr("S:%1 L:%2 — analyzing...").arg(sample).arg(line));

    QVector<double> spectrum = m_dataset->getPixelSpectrum(sample, line);
    QVector<double> wls      = m_dataset->meta().wavelengths;

    auto future = QtConcurrent::run([wls, spectrum, this]() -> PixelMatchResult {
        PixelMatchResult res;
        res.wavelengths = wls;
        res.spectrum    = spectrum;
        try {
            res.matches = SpectralLibrary::instance().analyze(res.wavelengths, res.spectrum);
        } catch (const std::exception& ex) {
            res.error = QString::fromStdString(ex.what());
        }
        return res;
    });
    m_enviWatcher->setFuture(future);
}

void MainWindow::onEnviAnalysisFinished()
{
    PixelMatchResult res = m_enviWatcher->result();
    if (!res.error.isEmpty()) {
        updateStatusBar(tr("Analysis error: %1").arg(res.error));
        m_coordLabel->setText(tr("Error"));
        return;
    }
    m_coordLabel->setText(tr("Analysis done — %1 matches").arg(res.matches.size()));
    showMatchResults(res.matches, res.wavelengths, res.spectrum);
}

// ---------- Spectrum file tab ----------

void MainWindow::onAnalyzeImported()
{
    if (m_importedWl.isEmpty()) return;
    if (m_importWatcher->isRunning()) return;

    updateStatusBar(tr("Analyzing..."));
    m_analyzeBtn->setEnabled(false);

    QVector<double> wls  = QVector<double>(m_importedWl.begin(),  m_importedWl.end());
    QVector<double> vals = QVector<double>(m_importedVals.begin(), m_importedVals.end());

    auto future = QtConcurrent::run([wls, vals, this]() -> PixelMatchResult {
        PixelMatchResult res;
        res.wavelengths = wls;
        res.spectrum    = vals;
        try {
            res.matches = SpectralLibrary::instance().analyze(res.wavelengths, res.spectrum);
        } catch (const std::exception& ex) {
            res.error = QString::fromStdString(ex.what());
        }
        return res;
    });
    m_importWatcher->setFuture(future);
}

void MainWindow::onImportAnalysisFinished()
{
    m_analyzeBtn->setEnabled(true);
    PixelMatchResult res = m_importWatcher->result();
    if (!res.error.isEmpty()) {
        updateStatusBar(tr("Analysis error: %1").arg(res.error));
        return;
    }
    updateStatusBar(tr("Analysis done — %1 matches found").arg(res.matches.size()));
    showMatchResults(res.matches, res.wavelengths, res.spectrum);

    // Open the SpectrumDialog for a rich visual
    auto* dlg = new SpectrumDialog(0, 0, res.spectrum, res.wavelengths, res.matches, false, this);
    dlg->setWindowTitle(tr("Spectral Match — %1").arg(m_importedLabel));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

// ---------- Shared result display ----------

void MainWindow::showMatchResults(
    const QVector<AnalysisDisplayEntry>& matches,
    const QVector<double>& wl,
    const QVector<double>& spectrum)
{
    m_lastMatches  = matches;
    m_lastWl       = wl;
    m_lastSpectrum = spectrum;

    m_matchList->clear();
    for (const auto& m : matches) {
        auto* item = new QListWidgetItem(
            QString("%1  |  Score: %2  |  Abundance: %3")
            .arg(m.name)
            .arg(m.confidenceScore, 0, 'f', 1)
            .arg(m.abundance, 0, 'f', 4));
        // Color-code by confidence score
        if      (m.confidenceScore >= 80) item->setForeground(QColor("#50fa7b"));
        else if (m.confidenceScore >= 50) item->setForeground(QColor("#f1fa8c"));
        else                              item->setForeground(QColor("#ff5555"));
        m_matchList->addItem(item);
    }
}

void MainWindow::onMatchResultClicked(int row)
{
    // Double-clicking a match row opens a SpectrumDialog focused on that mineral
    if (row < 0 || m_lastWl.isEmpty()) return;
    auto* dlg = new SpectrumDialog(0, 0, m_lastSpectrum, m_lastWl, m_lastMatches, false, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::onLibraryItemDoubleClicked(int /*row*/)
{
    // reserved for future: show individual reference spectrum
}

void MainWindow::plotSpectrum(const QVector<double>& /*wl*/,
                               const QVector<double>& /*vals*/,
                               const QString& /*label*/)
{
    // The rich chart is delegated to SpectrumDialog; nothing to do here.
}

void MainWindow::clearPlot() {}
