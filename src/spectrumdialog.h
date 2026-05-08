#pragma once

#include <QDialog>
#include <QVector>
#include <QMap>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QValueAxis>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QColor>
#include <QFile>
#include <QTextStream>
#include "spectrallibrary.h"    // 包含 SpectralLibrary + AnalysisDisplayEntry

// ─────────────────────────────────────────────────────────────────────────────
// SpectrumDialog — 独立光谱图对话框
//   • 多开、可移动、可保存 PNG
//   • 显示 SAM / FCLS 矿物匹配结果表（带复选框）
//   • 勾选矿物后，在光谱图上叠加该矿物的参考光谱（虚线，多选多色）
// ─────────────────────────────────────────────────────────────────────────────
class SpectrumDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SpectrumDialog(int x, int y,
                            const QVector<double>& vals,
                            const QVector<double>& wl,
                            const QVector<AnalysisDisplayEntry>& entries,
                            bool isSAM = false,
                            QWidget* parent = nullptr)
        : QDialog(parent), m_entries(entries), m_isSAM(isSAM)
    {
        setWindowTitle(QString("光谱图  (%1, %2)").arg(x).arg(y));
        setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint
                     | Qt::WindowMinimizeButtonHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(760, 680);

        setStyleSheet(R"(
            QDialog   { background:#1a1b2a; color:#eef2ff; }
            QLabel    { color:#b9c2e0; }
            QPushButton { background:#2c2d3c; color:#eef2ff; border:1px solid #3e4057;
                          border-radius:4px; padding:5px 14px; }
            QPushButton:hover   { background:#363847; border-color:#7c9cff; }
            QPushButton:checked { background:#1e3060; border-color:#7c9cff; color:#7c9cff; }
            QTableWidget { background:#1a1b26; color:#eef2ff; border:1px solid #3e4057;
                           gridline-color:#2a2b3a; font-size:11px; }
            QHeaderView::section { background:#252633; color:#7c9cff; border:none;
                                   padding:4px; font-size:11px; }
            QTableWidget::item:selected { background:#363f7a; }
            QTableWidget::indicator { width:13px; height:13px; border-radius:2px; }
            QTableWidget::indicator:unchecked {
                border:1px solid #5a5d7a; background:#1a1b26; }
            QTableWidget::indicator:checked {
                border:1px solid #7c9cff; background:#4a5aff; }
        )");

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 10);
        layout->setSpacing(8);

        // ── 信息栏 + Y轴模式切换按钮 ─────────────────────────────────────
        m_pixelYMin = vals.isEmpty() ? 0 : *std::min_element(vals.begin(), vals.end());
        m_pixelYMax = vals.isEmpty() ? 1 : *std::max_element(vals.begin(), vals.end());
        auto* infoLabel = new QLabel(
            QString("坐标: (%1, %2)　|　波段数: %3　|　反射率范围: %4 ~ %5")
            .arg(x).arg(y).arg(vals.size())
            .arg(m_pixelYMin, 0, 'f', 4).arg(m_pixelYMax, 0, 'f', 4));
        infoLabel->setStyleSheet("color:#7c9cff; font-size:11px; padding:4px 0;");

        m_axisAutoBtn = new QPushButton("自动 Y 轴");
        m_axisAutoBtn->setCheckable(true);
        m_axisAutoBtn->setChecked(false);
        m_axisAutoBtn->setToolTip(
            "关闭：Y 轴锁定在像元范围，像元曲线不被压扁\n"
            "开启：Y 轴自动扩展以包含所有参考曲线完整形状");

        auto* infoRow = new QHBoxLayout();
        infoRow->addWidget(infoLabel);
        infoRow->addStretch();
        infoRow->addWidget(m_axisAutoBtn);
        layout->addLayout(infoRow);

        // ── 光谱折线图 ────────────────────────────────────────────────
        auto* series = new QLineSeries();
        series->setName(QString("像元 (%1,%2)").arg(x).arg(y));
        QPen pen(QColor("#7c9cff")); pen.setWidthF(1.5);
        series->setPen(pen);

        QVector<QPointF> pts;
        pts.reserve(vals.size());
        for (int i = 0; i < vals.size(); ++i)
            pts.append({ (i < wl.size()) ? wl[i] : (double)i, vals[i] });
        series->replace(pts);

        auto* chart = new QChart();
        chart->addSeries(series);
        chart->setTheme(QChart::ChartThemeDark);
        chart->setBackgroundVisible(false);
        chart->setMargins(QMargins(4, 4, 4, 4));
        chart->legend()->setVisible(true);
        chart->legend()->setAlignment(Qt::AlignBottom);

        m_axisX = new QValueAxis();
        m_axisX->setTitleText(wl.isEmpty() ? "波段序号" : "波长 (nm)");
        m_axisX->setLabelFormat("%.0f");
        m_axisX->setTickCount(11);
        m_axisX->setMinorTickCount(4);
        m_axisX->setGridLineColor(QColor("#2a2b3a"));
        m_axisX->setMinorGridLineColor(QColor("#222230"));

        m_axisY = new QValueAxis();
        m_axisY->setTitleText("反射率");
        m_axisY->setLabelFormat("%.4f");
        m_axisY->setTickCount(9);
        m_axisY->setMinorTickCount(4);
        m_axisY->setGridLineColor(QColor("#2a2b3a"));
        m_axisY->setMinorGridLineColor(QColor("#222230"));

        chart->addAxis(m_axisX, Qt::AlignBottom);
        chart->addAxis(m_axisY, Qt::AlignLeft);
        series->attachAxis(m_axisX);
        series->attachAxis(m_axisY);

        if (!pts.isEmpty()) {
            m_pixelXMin = pts.front().x();
            m_pixelXMax = pts.back().x();
            double pad = (m_pixelYMax - m_pixelYMin) * 0.06;
            if (pad < 1e-6) pad = 0.01;
            m_axisX->setRange(m_pixelXMin, m_pixelXMax);
            m_axisY->setRange(m_pixelYMin - pad, m_pixelYMax + pad);
        }

        m_chartView = new QChartView(chart);
        m_chartView->setRenderHint(QPainter::Antialiasing);
        m_chartView->setMinimumHeight(340);
        layout->addWidget(m_chartView, /*stretch=*/1);

        connect(m_axisAutoBtn, &QPushButton::toggled,
                this, [this](bool){ updateAxisRanges(); });

        // ── 矿物匹配结果表（带复选框）────────────────────────────────
        if (!entries.isEmpty()) {
            QString tableLabel = isSAM
                ? "矿物匹配结果（SAM，按角度排序）— 勾选矿物可在图上叠加参考光谱"
                : "矿物解混结果（FCLS，按丰度排序）— 勾选矿物可在图上叠加参考光谱";
            auto* matchLabel = new QLabel(tableLabel);
            matchLabel->setStyleSheet("color:#7c9cff; font-size:11px; padding:4px 0 2px 0;");
            layout->addWidget(matchLabel);

            m_table = new QTableWidget(entries.size(), 4);
            QString col3 = isSAM ? "SAM角(°)" : "丰度";
            m_table->setHorizontalHeaderLabels({"矿物名称", "来源", col3, "置信度"});
            m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
            m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
            m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
            m_table->verticalHeader()->setVisible(false);
            m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
            m_table->setMaximumHeight(160);

            // 填表时屏蔽信号，避免 setCheckState 触发 itemChanged
            m_table->blockSignals(true);

            for (int i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];

                auto* nameItem  = new QTableWidgetItem(e.name);
                auto* srcItem   = new QTableWidgetItem(e.source);
                auto* abundItem = new QTableWidgetItem(
                    isSAM
                        ? QString::number(e.abundance, 'f', 2) + "°"
                        : QString::number(e.abundance * 100.0, 'f', 1) + "%");
                abundItem->setTextAlignment(Qt::AlignCenter);

                QString scoreStr = e.isUnknown
                    ? "—"
                    : QString::number(e.confidenceScore, 'f', 1) + "%";
                auto* scoreItem = new QTableWidgetItem(scoreStr);
                scoreItem->setTextAlignment(Qt::AlignCenter);

                if (e.isUnknown) {
                    QColor gray(120, 120, 140);
                    nameItem ->setForeground(gray);
                    srcItem  ->setForeground(gray);
                    abundItem->setForeground(gray);
                    scoreItem->setForeground(gray);
                    // 未知矿物：不添加复选框（无 ItemIsUserCheckable flag）
                } else {
                    if (i == 0) {
                        QColor gold(255, 200, 80);
                        nameItem ->setForeground(gold);
                        srcItem  ->setForeground(gold);
                        abundItem->setForeground(gold);
                        scoreItem->setForeground(gold);
                    }
                    // 非未知矿物：添加复选框
                    nameItem->setFlags(nameItem->flags() | Qt::ItemIsUserCheckable);
                    nameItem->setCheckState(Qt::Unchecked);
                }

                m_table->setItem(i, 0, nameItem);
                m_table->setItem(i, 1, srcItem);
                m_table->setItem(i, 2, abundItem);
                m_table->setItem(i, 3, scoreItem);
            }

            m_table->blockSignals(false);

            // 填表完成后再连接信号
            connect(m_table, &QTableWidget::itemChanged,
                    this, &SpectrumDialog::onMineralCheckChanged);

            layout->addWidget(m_table);

        } else if (SpectralLibrary_cacheReady()) {
            auto* noMatch = new QLabel("（光谱重叠不足或丰度均低于 1%，无匹配结果）");
            noMatch->setStyleSheet("color:#555870; font-size:11px; padding:2px 0;");
            layout->addWidget(noMatch);
        } else {
            auto* noLib = new QLabel("（未加载波谱库，无法匹配矿物）");
            noLib->setStyleSheet("color:#555870; font-size:11px; padding:2px 0;");
            layout->addWidget(noLib);
        }

        // ── 底部按钮 ──────────────────────────────────────────────────
        auto* btnRow    = new QHBoxLayout();
        auto* saveBtn   = new QPushButton("保存为 PNG");
        auto* exportBtn = new QPushButton("导出光谱数据");
        auto* closeBtn  = new QPushButton("关闭");
        btnRow->addWidget(saveBtn);
        btnRow->addWidget(exportBtn);
        btnRow->addStretch();
        btnRow->addWidget(closeBtn);
        layout->addLayout(btnRow);

        connect(saveBtn,   &QPushButton::clicked, this, &SpectrumDialog::onSave);
        connect(exportBtn, &QPushButton::clicked, this, &SpectrumDialog::onExport);
        connect(closeBtn,  &QPushButton::clicked, this, &QDialog::close);

        m_x = x; m_y = y;
        m_vals = vals; m_wl = wl;
    }

private slots:
    void onSave() {
        QString path = QFileDialog::getSaveFileName(
            this, "保存光谱图",
            QString("spectrum_%1_%2.png").arg(m_x).arg(m_y),
            "PNG 图片 (*.png)");
        if (path.isEmpty()) return;
        QPixmap pix = m_chartView->grab();
        pix.save(path, "PNG");
    }

    void onExport() {
        QString defaultName = (m_x < 0)
            ? "roi_spectrum.txt"
            : QString("spectrum_%1_%2.txt").arg(m_x).arg(m_y);
        QString path = QFileDialog::getSaveFileName(
            this, "导出光谱数据", defaultName,
            "ENVI ASCII Plot (*.txt);;所有文件 (*)");
        if (path.isEmpty()) return;

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream ts(&f);

        ts << "ENVI ASCII Plot File\n";
        ts << "Column 1: Wavelength\n";
        QString label = (m_x < 0)
            ? "ROI Mean Spectrum"
            : QString("Pixel (%1, %2)").arg(m_x).arg(m_y);
        ts << "Column 2: " << label << "\n";

        for (int i = 0; i < m_vals.size(); ++i) {
            double wlVal = (i < m_wl.size()) ? m_wl[i] : (double)i;
            ts << QString("  %1  %2\n")
                  .arg(wlVal,      12, 'f', 6)
                  .arg(m_vals[i],  10, 'f', 6);
        }
    }

    void onMineralCheckChanged(QTableWidgetItem* item) {
        if (!item || item->column() != 0) return;
        if (!(item->flags() & Qt::ItemIsUserCheckable)) return;

        int row = item->row();
        if (row < 0 || row >= m_entries.size()) return;

        const AnalysisDisplayEntry& e = m_entries[row];

        if (item->checkState() == Qt::Checked) {
            // 查找参考光谱
            SpectralLibrary::MineralLookupResult lr =
                SpectralLibrary::instance().findMineralSpectrum(e.name, e.source);

            if (!lr.found) {
                // 找不到参考光谱，恢复未勾选
                m_table->blockSignals(true);
                item->setCheckState(Qt::Unchecked);
                m_table->blockSignals(false);
                return;
            }

            // 移除该行之前可能残留的 series
            if (m_refSeriesMap.contains(row)) {
                m_chartView->chart()->removeSeries(m_refSeriesMap[row]);
                delete m_refSeriesMap[row];
                m_refSeriesMap.remove(row);
            }

            // 构建参考光谱 series
            auto* refSeries = new QLineSeries();
            refSeries->setName(e.name + "（参考）");
            QPen refPen(refColor(row));
            refPen.setWidthF(1.5);
            refPen.setStyle(Qt::DashLine);
            refSeries->setPen(refPen);

            QVector<QPointF> refPts;
            refPts.reserve(lr.wavelengths.size());
            for (int i = 0; i < lr.wavelengths.size(); ++i)
                refPts.append({ lr.wavelengths[i], lr.reflectance[i] });
            refSeries->replace(refPts);

            m_chartView->chart()->addSeries(refSeries);
            refSeries->attachAxis(m_axisX);
            refSeries->attachAxis(m_axisY);

            m_refSeriesMap[row] = refSeries;
            updateAxisRanges();

        } else {
            // 取消勾选：移除对应 series
            if (m_refSeriesMap.contains(row)) {
                m_chartView->chart()->removeSeries(m_refSeriesMap[row]);
                delete m_refSeriesMap[row];
                m_refSeriesMap.remove(row);
                updateAxisRanges();
            }
        }
    }

private:
    // 根据行号返回固定调色板颜色（与像元蓝色 #7c9cff 区分）
    static QColor refColor(int row) {
        static const QColor kPalette[] = {
            QColor("#FF6B6B"),   // 珊瑚红
            QColor("#51CF66"),   // 翠绿
            QColor("#FFD43B"),   // 金黄
            QColor("#CC5DE8"),   // 紫罗兰
            QColor("#FF922B"),   // 橙色
        };
        return kPalette[row % 5];
    }

    // 重新计算并应用坐标轴范围
    // X 轴：始终扩展到所有曲线的最宽波长覆盖
    // Y 轴：按"自动 Y 轴"按钮状态决定
    //   关闭（默认）→ 锁定在像元光谱范围，像元曲线不被压扁
    //   开启        → 扩展到包含所有参考曲线，可看完整形状对比
    void updateAxisRanges() {
        const bool autoY = m_axisAutoBtn && m_axisAutoBtn->isChecked();
        double xMin = m_pixelXMin, xMax = m_pixelXMax;
        double yMin = m_pixelYMin, yMax = m_pixelYMax;

        for (QLineSeries* s : m_refSeriesMap) {
            for (const QPointF& p : s->points()) {
                xMin = qMin(xMin, p.x());
                xMax = qMax(xMax, p.x());
                if (autoY) {
                    yMin = qMin(yMin, p.y());
                    yMax = qMax(yMax, p.y());
                }
            }
        }

        double yPad = (yMax - yMin) * 0.06;
        if (yPad < 1e-6) yPad = 0.01;
        m_axisX->setRange(xMin, xMax);
        m_axisY->setRange(yMin - yPad, yMax + yPad);
    }

    static bool SpectralLibrary_cacheReady() {
        return SpectralLibrary::instance().cacheReady();
    }

    // ── 图表相关 ──────────────────────────────────────────────────────
    QChartView*               m_chartView   = nullptr;
    QValueAxis*               m_axisX       = nullptr;
    QValueAxis*               m_axisY       = nullptr;
    QPushButton*              m_axisAutoBtn = nullptr;   // "自动 Y 轴"切换按钮

    // ── 矿物列表相关 ──────────────────────────────────────────────────
    QTableWidget*             m_table     = nullptr;
    QVector<AnalysisDisplayEntry> m_entries;
    QMap<int, QLineSeries*>   m_refSeriesMap;   // key = 行号
    bool                      m_isSAM     = false;

    // ── 像元光谱原始范围（供 updateAxisRanges 基准使用）──────────────
    double m_pixelXMin = 0, m_pixelXMax = 1;
    double m_pixelYMin = 0, m_pixelYMax = 1;

    // ── 数据 ─────────────────────────────────────────────────────────
    int             m_x, m_y;
    QVector<double> m_vals, m_wl;
};
