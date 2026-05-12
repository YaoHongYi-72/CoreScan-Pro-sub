#pragma once
#include <algorithm>
#include <QMainWindow>
#include <QFutureWatcher>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QLabel>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QValueAxis>
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

// ─────────────────────────────────────────────────────────────────────────────
// EnviGraphicsView — 支持滚轮缩放 + 选点标注 + 拖拽平移的 QGraphicsView
// ─────────────────────────────────────────────────────────────────────────────
class EnviGraphicsView : public QGraphicsView {
    Q_OBJECT
public:
    explicit EnviGraphicsView(QGraphicsScene* scene, QWidget* parent = nullptr);

    double currentZoom() const { return m_zoom; }
    void resetZoom();
    void setImageRect(const QRectF& rect);
    void clearSelectionMarkers();

signals:
    void pixelClicked(int sample, int line);
    void hoverPixelChanged(int sample, int line, bool inside);
    void zoomChanged(double zoom);

protected:
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;

private:
    bool isInsideImage(const QPointF& scenePos) const;
    void updateHoverState(const QPoint& viewportPos);
    void clearHoverState();
    void addSelectionMarker(const QPoint& pixel);

    double m_zoom = 1.0;
    QRectF m_imageRect;
    QVector<QPoint> m_selectedPixels;
    QPoint m_hoverPixel;
    QPoint m_hoverViewportPos;
    bool m_hoverValid = false;
    bool m_isPanning = false;
    QPoint m_panStartPos;
    int m_panStartHValue = 0;
    int m_panStartVValue = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// PixelPopup — 点击图像后弹出的小悬浮窗
// ─────────────────────────────────────────────────────────────────────────────
class PixelPopup : public QWidget {
    Q_OBJECT
public:
    explicit PixelPopup(QWidget* parent = nullptr);
    void showAt(const QPoint& globalPos, int x, int y);

signals:
    void spectrumRequested(int x, int y);

private:
    QLabel* m_coordLabel = nullptr;
    int m_x = 0, m_y = 0;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // File menu
    void onOpenEnvi();
    void onImportSpectrum();
    void onLoadLibrary();
    void onAbout();

    // ENVI mode
    void onEnviBandChanged(int idx);
    void onEnviPixelClicked(int sample, int line);
    void onEnviAnalysisFinished();
    void onEnviHoverChanged(int sample, int line, bool inside);

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
    void applyDarkTheme();
    void updateStatusBar(const QString& msg);
    void showMatchResults(const QVector<AnalysisDisplayEntry>& matches,
                          const QVector<double>& wl,
                          const QVector<double>& spectrum);
    void updateInlineChart(const QVector<double>& wl, const QVector<double>& vals,
                           const QString& label);

    // Data
    std::unique_ptr<EnviDataset> m_dataset;
    QVector<double>              m_importedWl;
    QVector<double>              m_importedVals;
    QString                      m_importedLabel;

    // Async watchers
    QFutureWatcher<PixelMatchResult>* m_enviWatcher  = nullptr;
    QFutureWatcher<PixelMatchResult>* m_importWatcher = nullptr;

    // UI widgets (ENVI tab)
    EnviGraphicsView*    m_enviView   = nullptr;
    QGraphicsScene*      m_enviScene  = nullptr;
    QGraphicsPixmapItem* m_enviPixItem = nullptr;
    QComboBox*           m_bandCombo  = nullptr;
    QLabel*              m_coordLabel = nullptr;
    QLabel*              m_zoomLabel  = nullptr;
    PixelPopup*          m_pixelPopup = nullptr;

    // UI widgets (Spectrum tab)
    QLabel*      m_specFileLabel  = nullptr;
    QPushButton* m_analyzeBtn     = nullptr;
    QChartView*  m_inlineChart    = nullptr;   // 内嵌光谱图
    QLabel*      m_chartPlaceholder = nullptr; // 未加载时的占位文字

    // Shared results
    QListWidget* m_matchList  = nullptr;
    QTabWidget*  m_tabWidget  = nullptr;

    // Library panel
    QListWidget* m_libraryList   = nullptr;
    QLabel*      m_libraryStatus = nullptr;

    // Last match results
    QVector<AnalysisDisplayEntry> m_lastMatches;
    QVector<double> m_lastWl;
    QVector<double> m_lastSpectrum;
    int m_lastSelectedSample = -1;
    int m_lastSelectedLine   = -1;
    bool m_lastResultHasPixel = false;
    int m_lastResultSample = -1;
    int m_lastResultLine   = -1;
};
