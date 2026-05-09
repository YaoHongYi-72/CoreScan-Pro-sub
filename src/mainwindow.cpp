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
#include <QProgressDialog>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QValueAxis>

// ─────────────────────────────────────────────────────────────────────────────
// PixelPopup 实现
// ─────────────────────────────────────────────────────────────────────────────
PixelPopup::PixelPopup(QWidget* parent) : QWidget(parent)
{
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    setStyleSheet(R"(
        QWidget#popup {
            background: #252636;
            border: 1px solid #4a4d6a;
            border-radius: 6px;
        }
        QLabel  { color: #b9c2e0; font-size: 11px; }
        QPushButton {
            background: #2a3a6a; color: #b8d0ff;
            border: 1px solid #4a6aaa; border-radius: 4px;
            padding: 3px 10px; font-size: 11px;
        }
        QPushButton:hover { background: #3a4a8a; }
    )");

    auto* inner = new QWidget(this);
    inner->setObjectName("popup");
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(inner);

    auto* layout = new QVBoxLayout(inner);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    m_coordLabel = new QLabel("坐标: --, --");
    layout->addWidget(m_coordLabel);

    auto* btn = new QPushButton("查看光谱");
    layout->addWidget(btn);

    connect(btn, &QPushButton::clicked, this, [this]() {
        hide();
        emit spectrumRequested(m_x, m_y);
    });
}

void PixelPopup::showAt(const QPoint& globalPos, int x, int y)
{
    m_x = x; m_y = y;
    m_coordLabel->setText(QString("坐标: (%1, %2)").arg(x).arg(y));
    adjustSize();
    move(globalPos + QPoint(12, 12));
    show();
    raise();
}

// ─────────────────────────────────────────────────────────────────────────────
// MainWindow
// ─────────────────────────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("CoreScan Pro — 光谱匹配");
    resize(1200, 760);
    setupUi();
    setupMenus();
    applyDarkTheme();
    updateStatusBar(tr("就绪。请先加载光谱库。"));
}

MainWindow::~MainWindow() = default;

void MainWindow::applyDarkTheme()
{
    qApp->setStyleSheet(R"(
        QMainWindow, QWidget          { background:#12131f; color:#c8d0e8; }
        QMenuBar                      { background:#1a1b2a; color:#c8d0e8; border-bottom:1px solid #2e3050; }
        QMenuBar::item:selected       { background:#2c2d3c; }
        QMenu                         { background:#1e1f30; color:#c8d0e8; border:1px solid #3a3f5c; }
        QMenu::item:selected          { background:#2c3a6a; }
        QTabWidget::pane              { border:1px solid #2e3050; background:#12131f; }
        QTabBar::tab                  { background:#1a1b2a; color:#8892b0; padding:6px 18px;
                                        border:1px solid #2e3050; border-bottom:none; border-radius:3px 3px 0 0; }
        QTabBar::tab:selected         { background:#12131f; color:#c8d0e8; border-bottom:1px solid #12131f; }
        QTabBar::tab:hover            { background:#252636; color:#c8d0e8; }
        QGroupBox                     { border:1px solid #2e3050; border-radius:4px; margin-top:8px;
                                        color:#8892b0; font-size:11px; }
        QGroupBox::title              { subcontrol-origin:margin; left:8px; padding:0 4px; }
        QListWidget                   { background:#1a1b2a; color:#c8d0e8; border:1px solid #2e3050;
                                        alternate-background-color:#1e1f30; }
        QListWidget::item:selected    { background:#2c3a6a; color:#c8d0e8; }
        QListWidget::item:hover       { background:#252636; }
        QComboBox                     { background:#1e1f30; color:#c8d0e8; border:1px solid #3a3f5c;
                                        border-radius:3px; padding:3px 8px; }
        QComboBox::drop-down          { border:none; }
        QComboBox QAbstractItemView   { background:#1e1f30; color:#c8d0e8; selection-background-color:#2c3a6a; }
        QPushButton                   { background:#2c2d3c; color:#c8d0e8; border:1px solid #3e4057;
                                        border-radius:4px; padding:5px 14px; }
        QPushButton:hover             { background:#363847; border-color:#7c9cff; }
        QPushButton:disabled          { color:#555870; border-color:#2e3050; }
        QLabel                        { color:#b9c2e0; }
        QStatusBar                    { background:#1a1b2a; color:#8892b0; border-top:1px solid #2e3050; }
        QScrollBar:vertical           { background:#1a1b2a; width:8px; }
        QScrollBar::handle:vertical   { background:#3a3f5c; border-radius:4px; min-height:20px; }
        QScrollBar:horizontal         { background:#1a1b2a; height:8px; }
        QScrollBar::handle:horizontal { background:#3a3f5c; border-radius:4px; min-width:20px; }
        QSplitter::handle             { background:#2e3050; }
    )");
}

void MainWindow::setupUi()
{
    auto* central    = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    // ── 左侧：Tab（ENVI | 光谱文件）──────────────────────────────────
    m_tabWidget = new QTabWidget;
    setupEnviTab();
    setupSpectrumTab();

    // ── 右侧：库面板 + 匹配结果 ──────────────────────────────────────
    auto* rightWidget = new QWidget;
    auto* rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);
    setupLibraryPanel();

    auto* matchGroup  = new QGroupBox(tr("匹配结果"));
    matchGroup->setMinimumHeight(200);
    auto* matchLayout = new QVBoxLayout(matchGroup);
    matchLayout->setContentsMargins(6, 14, 6, 6);
    m_matchList = new QListWidget;
    m_matchList->setAlternatingRowColors(true);
    matchLayout->addWidget(m_matchList);
    connect(m_matchList, &QListWidget::currentRowChanged,
            this, &MainWindow::onMatchResultClicked);

    rightLayout->addWidget(m_libraryList->parentWidget());
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
    auto* fileMenu      = menuBar()->addMenu(tr("文件"));
    auto* actOpenEnvi   = fileMenu->addAction(tr("打开 ENVI 文件 (.hdr)..."));
    auto* actImportSpec = fileMenu->addAction(tr("导入光谱文件 (txt/csv)..."));
    auto* actLoadLib    = fileMenu->addAction(tr("加载光谱库..."));
    fileMenu->addSeparator();
    auto* actQuit = fileMenu->addAction(tr("退出"));

    auto* helpMenu = menuBar()->addMenu(tr("帮助"));
    auto* actAbout = helpMenu->addAction(tr("关于"));

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
    layout->setSpacing(4);

    // ── 顶部工具栏 ────────────────────────────────────────────────────
    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("显示波段:")));
    m_bandCombo = new QComboBox;
    m_bandCombo->setMinimumWidth(160);
    topRow->addWidget(m_bandCombo);
    topRow->addStretch();
    m_coordLabel = new QLabel(tr("坐标: -- , --"));
    m_coordLabel->setStyleSheet("color:#7c9cff; font-size:11px;");
    topRow->addWidget(m_coordLabel);
    topRow->addSpacing(20);
    m_zoomLabel = new QLabel(tr("缩放: 100%"));
    m_zoomLabel->setStyleSheet("color:#7c9cff; font-size:11px;");
    topRow->addWidget(m_zoomLabel);
    layout->addLayout(topRow);

    // ── QGraphicsView 图像区域 ────────────────────────────────────────
    m_enviScene   = new QGraphicsScene(this);
    m_enviView    = new EnviGraphicsView(m_enviScene, tab);
    m_enviPixItem = nullptr;

    // 占位文字（未加载时显示）
    auto* placeholder = m_enviScene->addText(
        tr("未加载 ENVI 文件\n\n请使用 文件 → 打开 ENVI 文件 (.hdr)..."));
    placeholder->setDefaultTextColor(QColor("#8892b0"));
    placeholder->setPos(60, 160);
    m_enviScene->setSceneRect(0, 0, 600, 450);

    layout->addWidget(m_enviView, 1);

    // ── 信号连接 ──────────────────────────────────────────────────────
    connect(m_bandCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onEnviBandChanged);
    connect(m_enviView, &EnviGraphicsView::pixelClicked,
            this, &MainWindow::onEnviPixelClicked);
    connect(m_enviView, &EnviGraphicsView::zoomChanged, this, [this](double z) {
        m_zoomLabel->setText(tr("缩放: %1%").arg(qRound(z * 100)));
    });

    m_tabWidget->addTab(tab, tr("ENVI 图像"));

    // ── PixelPopup ────────────────────────────────────────────────────
    m_pixelPopup = new PixelPopup(this);
    connect(m_pixelPopup, &PixelPopup::spectrumRequested,
            this, &MainWindow::onEnviPixelClicked);

    // ── 异步 watcher ──────────────────────────────────────────────────
    m_enviWatcher = new QFutureWatcher<PixelMatchResult>(this);
    connect(m_enviWatcher, &QFutureWatcher<PixelMatchResult>::finished,
            this, &MainWindow::onEnviAnalysisFinished);
}

void MainWindow::setupSpectrumTab()
{
    auto* tab    = new QWidget;
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    // ── 顶部工具栏 ────────────────────────────────────────────────────
    auto* topRow = new QHBoxLayout;
    m_specFileLabel = new QLabel(tr("未加载光谱文件。"));
    m_specFileLabel->setStyleSheet("color:#7c9cff; font-size:11px;");
    m_specFileLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_analyzeBtn = new QPushButton(tr("开始匹配"));
    m_analyzeBtn->setEnabled(false);
    topRow->addWidget(m_specFileLabel);
    topRow->addWidget(m_analyzeBtn);
    layout->addLayout(topRow);

    // ── 中央区域：占位文字 / 内嵌光谱图（用 QStackedWidget 切换）────
    auto* stack = new QStackedWidget;

    // 页 0：占位文字
    m_chartPlaceholder = new QLabel(
        tr("导入光谱文件后，数据将在此处显示。\n\n"
           "请使用 文件 → 导入光谱文件 (txt/csv)..."));
    m_chartPlaceholder->setAlignment(Qt::AlignCenter);
    m_chartPlaceholder->setStyleSheet(
        "background:#1e2030; color:#8892b0; border:1px solid #3a3f5c;");
    stack->addWidget(m_chartPlaceholder);   // index 0

    // 页 1：内嵌 QChartView
    auto* chart = new QChart();
    chart->setTheme(QChart::ChartThemeDark);
    chart->setBackgroundVisible(false);
    chart->setMargins(QMargins(4, 4, 4, 4));
    chart->legend()->setVisible(true);
    chart->legend()->setAlignment(Qt::AlignBottom);

    m_inlineChart = new QChartView(chart);
    m_inlineChart->setRenderHint(QPainter::Antialiasing);
    m_inlineChart->setStyleSheet("background:#1e2030; border:1px solid #3a3f5c;");
    stack->addWidget(m_inlineChart);        // index 1

    layout->addWidget(stack, 1);

    // 保存 stack 指针供 updateInlineChart 使用
    // 通过 m_inlineChart->parentWidget() 可以拿到 stack
    connect(m_analyzeBtn, &QPushButton::clicked, this, &MainWindow::onAnalyzeImported);

    m_tabWidget->addTab(tab, tr("光谱文件"));

    m_importWatcher = new QFutureWatcher<PixelMatchResult>(this);
    connect(m_importWatcher, &QFutureWatcher<PixelMatchResult>::finished,
            this, &MainWindow::onImportAnalysisFinished);
}

void MainWindow::setupLibraryPanel()
{
    auto* group  = new QGroupBox(tr("光谱库"));
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(6, 14, 6, 6);

    m_libraryStatus = new QLabel(tr("未加载光谱库。"));
    m_libraryStatus->setWordWrap(true);
    layout->addWidget(m_libraryStatus);

    auto* loadBtn = new QPushButton(tr("加载光谱库..."));
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

// ─────────────────────────────────────────────────────────────────────────────
// helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool parseSpectrumFile(const QString& path,
                               QVector<double>& outWl,
                               QVector<double>& outVals,
                               QString& outErr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        outErr = QObject::tr("无法打开文件: %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    bool isCsv = path.endsWith(".csv", Qt::CaseInsensitive);
    outWl.clear(); outVals.clear();
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        QStringList parts = isCsv ? line.split(',')
                                  : line.split(QRegularExpression("\\s+"));
        if (parts.size() < 2) continue;
        bool okW, okV;
        double w = parts[0].trimmed().toDouble(&okW);
        double v = parts[1].trimmed().toDouble(&okV);
        if (!okW || !okV) continue;
        outWl.append(w);
        outVals.append(v);
    }
    if (outWl.isEmpty()) {
        outErr = QObject::tr("文件中未找到有效的波长/反射率数据对。");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// File menu handlers
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onOpenEnvi()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("打开 ENVI 头文件"), QString(),
        tr("ENVI 头文件 (*.hdr);;所有文件 (*)"));
    if (path.isEmpty()) return;

    try {
        auto ds = std::make_unique<EnviDataset>();
        if (!ds->load(path)) {
            QMessageBox::critical(this, tr("打开 ENVI 失败"),
                                  tr("无法加载: %1").arg(path));
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
                : QString("波段 %1").arg(b + 1);
            m_bandCombo->addItem(label);
        }
        m_bandCombo->blockSignals(false);

        onEnviBandChanged(0);
        m_tabWidget->setCurrentIndex(0);
        updateStatusBar(tr("已加载: %1  [%2 × %3，%4 波段]")
            .arg(QFileInfo(path).fileName())
            .arg(meta.samples).arg(meta.lines).arg(meta.bands));
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, tr("打开 ENVI 失败"),
                              QString::fromStdString(ex.what()));
    }
}

void MainWindow::onImportSpectrum()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("导入光谱文件"), QString(),
        tr("光谱文件 (*.txt *.csv);;所有文件 (*)"));
    if (path.isEmpty()) return;

    QString err;
    if (!parseSpectrumFile(path, m_importedWl, m_importedVals, err)) {
        QMessageBox::warning(this, tr("导入失败"), err);
        return;
    }
    m_importedLabel = QFileInfo(path).baseName();
    m_specFileLabel->setText(
        tr("%1  [%2 个数据点，%.1f – %.1f nm]")
        .arg(m_importedLabel)
        .arg(m_importedWl.size())
        .arg(m_importedWl.front())
        .arg(m_importedWl.back()));
    m_analyzeBtn->setEnabled(true);
    m_tabWidget->setCurrentIndex(1);

    // 立即在内嵌图表中显示光谱
    updateInlineChart(m_importedWl, m_importedVals, m_importedLabel);

    updateStatusBar(tr("已导入光谱: %1（%2 个数据点）")
                    .arg(m_importedLabel).arg(m_importedWl.size()));
}

void MainWindow::onLoadLibrary()
{
    QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("加载光谱库"), QString(),
        tr("光谱文件 (*.txt *.csv);;所有文件 (*)"));
    if (paths.isEmpty()) return;

    auto* prog = new QProgressDialog(tr("正在加载光谱库..."), tr("取消"),
                                     0, paths.size(), this);
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

    m_libraryList->clear();
    const auto& spectra = lib.spectra();
    for (const auto& e : spectra)
        m_libraryList->addItem(e.name);
    m_libraryStatus->setText(tr("已加载 %1 条参考光谱").arg(spectra.size()));
    updateStatusBar(tr("光谱库: %1 条参考光谱").arg(spectra.size()));
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("关于 CoreScan Pro — 光谱匹配"),
        tr("<b>CoreScan Pro 光谱匹配工具</b><br><br>"
           "从 CoreScan Pro 主程序提取的独立光谱分析模块。<br><br>"
           "支持功能：<br>"
           "&bull; ENVI 高光谱图像文件 (.hdr)<br>"
           "&bull; 直接导入光谱文件 (txt / csv)<br>"
           "&bull; SAM 粗筛 + FCLS 精解矿物匹配<br>"
           "&bull; 连续统去除预处理<br><br>"
           "基于 Qt6 &amp; Eigen3 构建。"));
}

// ─────────────────────────────────────────────────────────────────────────────
// ENVI tab slots
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onEnviBandChanged(int idx)
{
    if (!m_dataset || idx < 0) return;

    QImage img = m_dataset->getBandImage(idx);
    QPixmap pm = QPixmap::fromImage(img);

    if (!m_enviPixItem) {
        // 首次加载：清空场景，添加图像 item
        m_enviScene->clear();
        m_enviPixItem = m_enviScene->addPixmap(pm);
        m_enviScene->setSceneRect(pm.rect());
        // 适应视图
        m_enviView->fitInView(m_enviScene->sceneRect(), Qt::KeepAspectRatio);
        m_enviView->resetZoom();
    } else {
        m_enviPixItem->setPixmap(pm);
        m_enviScene->setSceneRect(pm.rect());
    }
}

void MainWindow::onEnviPixelClicked(int sample, int line)
{
    if (!m_dataset) return;
    if (m_enviWatcher->isRunning()) return;

    // 边界检查
    const auto meta = m_dataset->meta();
    if (sample < 0 || line < 0 || sample >= meta.samples || line >= meta.lines)
        return;

    m_coordLabel->setText(tr("S:%1  L:%2  — 分析中...").arg(sample).arg(line));

    // 先弹出 PixelPopup，让用户确认后再触发分析
    // （PixelPopup 的 spectrumRequested 信号已连接到本槽，
    //   所以这里直接执行分析，不再二次弹窗）
    QVector<double> spectrum = m_dataset->getPixelSpectrum(sample, line);
    QVector<double> wls      = m_dataset->meta().wavelengths;

    auto future = QtConcurrent::run([wls, spectrum]() -> PixelMatchResult {
        PixelMatchResult res;
        res.wavelengths = wls;
        res.spectrum    = spectrum;
        try {
            res.matches = SpectralLibrary::instance().analyze(
                res.wavelengths, res.spectrum);
        } catch (const std::exception& ex) {
            res.error = QString::fromStdString(ex.what());
        }
        return res;
    });
    m_enviWatcher->setFuture(future);

    // 同时弹出 PixelPopup（显示坐标 + "查看光谱"按钮）
    // 注意：PixelPopup 的 spectrumRequested 已连接到本槽，
    // 但此处我们直接触发分析，popup 仅作坐标提示用
    // 若需要"先弹窗再分析"的交互，可在 eventFilter 中改为先 showAt
}

void MainWindow::onEnviAnalysisFinished()
{
    PixelMatchResult res = m_enviWatcher->result();
    if (!res.error.isEmpty()) {
        updateStatusBar(tr("分析出错: %1").arg(res.error));
        m_coordLabel->setText(tr("出错"));
        return;
    }
    m_coordLabel->setText(tr("分析完成 — %1 个匹配").arg(res.matches.size()));
    showMatchResults(res.matches, res.wavelengths, res.spectrum);

    // 弹出 SpectrumDialog 显示完整光谱图 + 矿物匹配表
    // 需要知道点击的坐标，从 coordLabel 文字中解析（简单方案）
    auto* dlg = new SpectrumDialog(0, 0, res.spectrum, res.wavelengths,
                                   res.matches, false, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

// ─────────────────────────────────────────────────────────────────────────────
// Spectrum file tab slots
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onAnalyzeImported()
{
    if (m_importedWl.isEmpty()) return;
    if (m_importWatcher->isRunning()) return;

    updateStatusBar(tr("正在分析..."));
    m_analyzeBtn->setEnabled(false);

    QVector<double> wls  = m_importedWl;
    QVector<double> vals = m_importedVals;

    auto future = QtConcurrent::run([wls, vals]() -> PixelMatchResult {
        PixelMatchResult res;
        res.wavelengths = wls;
        res.spectrum    = vals;
        try {
            res.matches = SpectralLibrary::instance().analyze(
                res.wavelengths, res.spectrum);
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
        updateStatusBar(tr("分析出错: %1").arg(res.error));
        return;
    }
    updateStatusBar(tr("分析完成 — 找到 %1 个匹配").arg(res.matches.size()));
    showMatchResults(res.matches, res.wavelengths, res.spectrum);

    // 弹出 SpectrumDialog 显示完整光谱图 + 矿物匹配表
    auto* dlg = new SpectrumDialog(0, 0, res.spectrum, res.wavelengths,
                                   res.matches, false, this);
    dlg->setWindowTitle(tr("光谱匹配 — %1").arg(m_importedLabel));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared helpers
// ─────────────────────────────────────────────────────────────────────────────

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
        QString text = QString("%1  |  置信度: %2%  |  丰度: %3%")
            .arg(m.name)
            .arg(m.confidenceScore, 0, 'f', 1)
            .arg(m.abundance * 100.0, 0, 'f', 1);
        auto* item = new QListWidgetItem(text);
        if      (m.confidenceScore >= 80) item->setForeground(QColor("#50fa7b"));
        else if (m.confidenceScore >= 50) item->setForeground(QColor("#f1fa8c"));
        else                              item->setForeground(QColor("#ff5555"));
        m_matchList->addItem(item);
    }
}

void MainWindow::updateInlineChart(const QVector<double>& wl,
                                   const QVector<double>& vals,
                                   const QString& label)
{
    // 切换到图表页（index 1）
    auto* stack = qobject_cast<QStackedWidget*>(m_inlineChart->parentWidget());
    if (stack) stack->setCurrentIndex(1);

    auto* chart = m_inlineChart->chart();
    chart->removeAllSeries();

    // 清除旧坐标轴
    const auto oldAxes = chart->axes();
    for (auto* ax : oldAxes) {
        chart->removeAxis(ax);
        delete ax;
    }

    auto* series = new QLineSeries();
    series->setName(label);
    QPen pen(QColor("#7c9cff"));
    pen.setWidthF(1.5);
    series->setPen(pen);

    double xMin = 0, xMax = 1, yMin = 0, yMax = 1;
    if (!vals.isEmpty()) {
        xMin = wl.isEmpty() ? 0 : wl.front();
        xMax = wl.isEmpty() ? vals.size() - 1 : wl.back();
        yMin = *std::min_element(vals.begin(), vals.end());
        yMax = *std::max_element(vals.begin(), vals.end());
    }

    QVector<QPointF> pts;
    pts.reserve(vals.size());
    for (int i = 0; i < vals.size(); ++i)
        pts.append({ (i < wl.size()) ? wl[i] : (double)i, vals[i] });
    series->replace(pts);
    chart->addSeries(series);

    auto* axisX = new QValueAxis();
    axisX->setTitleText(wl.isEmpty() ? tr("波段序号") : tr("波长 (nm)"));
    axisX->setLabelFormat("%.0f");
    axisX->setTickCount(9);
    axisX->setGridLineColor(QColor("#2a2b3a"));

    auto* axisY = new QValueAxis();
    axisY->setTitleText(tr("反射率"));
    axisY->setLabelFormat("%.4f");
    axisY->setTickCount(7);
    axisY->setGridLineColor(QColor("#2a2b3a"));

    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisX);
    series->attachAxis(axisY);

    double yPad = (yMax - yMin) * 0.08;
    if (yPad < 1e-6) yPad = 0.01;
    axisX->setRange(xMin, xMax);
    axisY->setRange(yMin - yPad, yMax + yPad);
}

void MainWindow::onMatchResultClicked(int row)
{
    if (row < 0 || m_lastWl.isEmpty()) return;
    auto* dlg = new SpectrumDialog(0, 0, m_lastSpectrum, m_lastWl,
                                   m_lastMatches, false, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::onLibraryItemDoubleClicked(int /*row*/)
{
    // 预留：双击库条目显示参考光谱
}

