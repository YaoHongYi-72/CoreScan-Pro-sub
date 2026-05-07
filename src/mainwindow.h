#pragma once
#include <QMainWindow>
#include <QFutureWatcher>
#include "spectrallibrary.h"
#include "envidataset.h"

QT_BEGIN_NAMESPACE
class QAction;
class QLabel;
class QComboBox;
class QListWidget;
class QSplitter;
class QTabWidget;
class QPushButton;
class QLineEdit;
QT_END_NAMESPACE

// Result from async ENVI pixel analysis
struct PixelMatchResult {
    QVector<AnalysisDisplayEntry> matches;
    QVector<double> spectrum;
    QVector<double> wavelengths;
    QString error;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // File menu
    void onOpenEnvi();          // Load ENVI .hdr file
    void onImportSpectrum();    // Import txt/csv spectrum file
    void onLoadLibrary();       // Load spectral reference library
    void onAbout();

    // ENVI mode
    void onEnviBandChanged(int idx);
    void onEnviPixelClicked(int sample, int line);
    void onEnviAnalysisFinished();

    // Spectrum file mode
    void onAnalyzeImported();
    void onImportAnalysisFinished();

    // Library list interaction
    void onLibraryItemDoubleClicked(int row);
    void onMatchResultClicked(int row);

private:
    void setupUi();
    void setupMenus();
    void setupEnviTab();
    void setupSpectrumTab();
    void setupLibraryPanel();
    void updateStatusBar(const QString& msg);
    void showMatchResults(const QVector<AnalysisDisplayEntry>& matches,
                          const QVector<double>& wl,
                          const QVector<double>& spectrum);
    void plotSpectrum(const QVector<double>& wl,
                      const QVector<double>& vals,
                      const QString& label);
    void clearPlot();

    // Data
    std::unique_ptr<EnviDataset> m_dataset;
    QVector<double>              m_importedWl;
    QVector<double>              m_importedVals;
    QString                       m_importedLabel;

    // Async watchers
    QFutureWatcher<PixelMatchResult>* m_enviWatcher = nullptr;
    QFutureWatcher<PixelMatchResult>* m_importWatcher = nullptr;

    // UI widgets (ENVI tab)
    QLabel*      m_enviImage  = nullptr;
    QComboBox*   m_bandCombo  = nullptr;
    QLabel*      m_coordLabel = nullptr;

    // UI widgets (Spectrum tab)
    QLabel*      m_specFileLabel = nullptr;
    QPushButton* m_analyzeBtn    = nullptr;

    // Shared chart area (simple QLabel-based placeholder; real chart in SpectrumDialog)
    QListWidget* m_matchList     = nullptr;
    QLabel*      m_chartPlaceholder = nullptr;
    QTabWidget*  m_tabWidget     = nullptr;

    // Library panel
    QListWidget* m_libraryList   = nullptr;
    QLabel*      m_libraryStatus = nullptr;

    // Last match results (for overlay on demand)
    QVector<AnalysisDisplayEntry> m_lastMatches;
    QVector<double> m_lastWl;
    QVector<double> m_lastSpectrum;
};
