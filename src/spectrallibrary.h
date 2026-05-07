#pragma once

#include <QString>
#include <QVector>
#include <QColor>
#include <QList>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QMap>
#include <QtMath>
#include <QtConcurrent>
#include <Eigen/Dense>
#include <shared_mutex>
#include <algorithm>

#include "spectralanalyzer.h"

// ─────────────────────────────────────────────────────────────────────────────
// MineralSpectrum — 单种矿物的参考光谱
// ─────────────────────────────────────────────────────────────────────────────
struct MineralSpectrum {
    QString name;
    QString nameEn;
    QString source;
    QColor  color;
    QVector<double> wavelengths;
    QVector<double> reflectance;
};

// ─────────────────────────────────────────────────────────────────────────────
// SpectralLibrary — 单例，支持 USGS TXT / 多列 CSV 批量加载 + SAM 匹配
// ─────────────────────────────────────────────────────────────────────────────
class SpectralLibrary
{
public:
    struct MatchResult {
        QString name;
        QString nameEn;
        QString source;
        double  angle;   // SAM 角度（弧度），越小越相似
    };

    // ── 矿物参考光谱查询结果 ─────────────────────────────────────────
    struct MineralLookupResult {
        QVector<double> wavelengths;
        QVector<double> reflectance;
        QColor          color;
        bool            found = false;
    };

    static SpectralLibrary& instance() {
        static SpectralLibrary lib;
        return lib;
    }

    const QList<MineralSpectrum>& spectra() const { return m_spectra; }

    // ── 按名称+来源查找矿物参考光谱（线程安全，优先精确匹配）────────
    MineralLookupResult findMineralSpectrum(const QString& name,
                                            const QString& source) const
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        // 精确匹配：名称（中/英）+ 来源
        for (const MineralSpectrum& ms : m_spectra) {
            if ((ms.name == name || ms.nameEn == name) && ms.source == source)
                return { ms.wavelengths, ms.reflectance, ms.color, true };
        }
        // 退化匹配：仅名称
        for (const MineralSpectrum& ms : m_spectra) {
            if (ms.name == name || ms.nameEn == name)
                return { ms.wavelengths, ms.reflectance, ms.color, true };
        }
        return {};
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lk(m_mutex);
        m_spectra.clear();
        m_cacheReady = false;
        m_commonWl   = Eigen::VectorXd();
        m_rawLibMatrix = Eigen::MatrixXd();
        m_crLibMatrix  = Eigen::MatrixXd();
    }

    // ── 是否已建立预计算缓存 ─────────────────────────────────────────────
    bool cacheReady() const {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        return m_cacheReady;
    }

    // ── 返回公共波长网格（仅缓存就绪后有效）─────────────────────────────
    Eigen::VectorXd commonWl() const {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        return m_commonWl;
    }

    // ── 单谱 CSV（两列：wavelength_nm, reflectance）────────────────────
    bool loadCSV(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        QTextStream in(&f);
        MineralSpectrum ms;
        ms.name   = QFileInfo(path).completeBaseName();
        ms.nameEn = ms.name;
        ms.source = "外部文件";
        ms.color  = QColor(220, 200, 60);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.startsWith('#') || line.isEmpty()) continue;
            const QStringList p = line.split(',');
            if (p.size() < 2) continue;
            bool ok1, ok2;
            double w = p[0].trimmed().toDouble(&ok1);
            double r = p[1].trimmed().toDouble(&ok2);
            if (ok1 && ok2) { ms.wavelengths << w; ms.reflectance << r; }
        }
        if (ms.wavelengths.isEmpty()) return false;

        std::unique_lock<std::shared_mutex> lk(m_mutex);
        m_spectra << ms;
        buildCache();
        return true;
    }

    // ── USGS ASCII Plot 格式（database1 的 TXT）───────────────────────
    int loadUSGSTxt(const QString& path) {
        // ── 先在锁外解析文件（I/O 不持锁）──────────────────────────────
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
        QTextStream in(&f);

        QVector<QString> names;   // 每列矿物名（index 0 = 波长列，跳过）
        bool headerDone = false;
        QVector<QVector<double>> cols; // cols[0]=波长, cols[1..N]=反射率

        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.startsWith("ENVI")) continue;

            if (line.startsWith("Column")) {
                // "Column 1: Wavelength"  or  "Column N: file.spc MineralName~~idx"
                int colon = line.indexOf(':');
                if (colon < 0) continue;
                QString desc = line.mid(colon + 1).trimmed();
                if (desc.startsWith("Wavelength", Qt::CaseInsensitive)) {
                    names << "";   // 占位
                } else {
                    // 取 ~~ 前的部分，去掉文件名（第一个空格前）
                    int tilde = desc.indexOf("~~");
                    QString namepart = (tilde > 0) ? desc.left(tilde) : desc;
                    int sp = namepart.indexOf(' ');
                    QString mineralName = (sp > 0) ? namepart.mid(sp + 1).trimmed() : namepart.trimmed();
                    mineralName.replace('_', ' ');
                    names << mineralName;
                }
                continue;
            }

            // 数据行
            if (!headerDone) {
                headerDone = true;
                cols.resize(names.size());
            }
            QStringList parts = line.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (parts.size() < (int)names.size()) continue;
            for (int i = 0; i < names.size() && i < parts.size(); ++i) {
                bool ok;
                double v = parts[i].toDouble(&ok);
                if (ok) cols[i] << v;
            }
        }

        if (cols.isEmpty() || cols[0].isEmpty()) return 0;

        // 波长列（index 0）单位：μm → nm
        QVector<double>& wlCol = cols[0];
        bool inMicron = (wlCol.first() < 10.0);  // USGS 通常是 μm
        if (inMicron) for (double& w : wlCol) w *= 1000.0;

        // 生成颜色序列
        static const QColor palette[] = {
            {100,180,255},{255,160,80},{120,220,120},{255,100,100},
            {200,150,255},{80,220,200},{255,220,80},{180,180,180}
        };

        int added = 0;
        QString srcName = QFileInfo(path).completeBaseName();
        QList<MineralSpectrum> newSpectra;
        for (int i = 1; i < names.size(); ++i) {
            if (cols[i].size() != wlCol.size()) continue;
            bool hasValid = false;
            for (double v : cols[i]) if (v >= 0) { hasValid = true; break; }
            if (!hasValid) continue;

            MineralSpectrum ms;
            ms.nameEn = names[i];
            ms.name   = names[i];
            ms.source = srcName;
            ms.color  = palette[(added) % 8];
            ms.wavelengths = wlCol;
            ms.reflectance = cols[i];
            for (double& v : ms.reflectance) if (v < 0) v = 0;
            newSpectra << ms;
            ++added;
        }

        if (added > 0) {
            std::unique_lock<std::shared_mutex> lk(m_mutex);
            m_spectra << newSpectra;
            buildCache();
        }
        return added;
    }

    // ── 多列 CSV + notes CSV（database2）─────────────────────────────
    // spectraPath: 第1列波长(nm)，后续列 "编号:矿物缩写"
    // notesPath:   "order #,Sample,Index"  → 编号→完整名称
    int loadMultiCSV(const QString& spectraPath, const QString& notesPath) {
        // 先读 notes：编号 → 完整名
        QMap<int, QString> idToName;
        {
            QFile fn(notesPath);
            if (fn.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&fn);
                in.readLine(); // 跳过表头
                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (line.isEmpty()) continue;
                    QStringList p = line.split(',');
                    if (p.size() < 2) continue;
                    bool ok;
                    int id = p[0].trimmed().toInt(&ok);
                    if (ok) idToName[id] = p[1].trimmed();
                }
            }
        }

        QFile fs(spectraPath);
        if (!fs.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
        QTextStream in(&fs);

        // 解析表头
        QString header = in.readLine();
        QStringList hdrs = header.split(',');
        // hdrs[0] = "Wavelength_(nm)", hdrs[i] = "000001:ActCa1." ...
        QVector<int> colIds;   // 每列对应的矿物编号（0=波长列）
        colIds << 0;
        for (int i = 1; i < hdrs.size(); ++i) {
            QString h = hdrs[i].trimmed();
            int colon = h.indexOf(':');
            bool ok;
            int id = (colon > 0) ? h.left(colon).toInt(&ok) : 0;
            colIds << (ok ? id : 0);
        }

        QVector<QVector<double>> cols(hdrs.size());
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty()) continue;
            QStringList parts = line.split(',');
            for (int i = 0; i < parts.size() && i < cols.size(); ++i) {
                bool ok;
                double v = parts[i].trimmed().toDouble(&ok);
                if (ok) cols[i] << v;
            }
        }

        if (cols.isEmpty() || cols[0].isEmpty()) return 0;

        static const QColor palette[] = {
            {100,180,255},{255,160,80},{120,220,120},{255,100,100},
            {200,150,255},{80,220,200},{255,220,80},{180,180,180}
        };

        int added = 0;
        QString srcName = QFileInfo(spectraPath).completeBaseName();
        QList<MineralSpectrum> newSpectra;
        for (int i = 1; i < cols.size(); ++i) {
            if (cols[i].size() != cols[0].size()) continue;
            bool hasValid = false;
            for (double v : cols[i]) if (v >= 0) { hasValid = true; break; }
            if (!hasValid) continue;

            MineralSpectrum ms;
            int id = (i < colIds.size()) ? colIds[i] : 0;
            ms.nameEn = idToName.value(id, hdrs[i].trimmed());
            ms.name   = ms.nameEn;
            ms.source = srcName;
            ms.color  = palette[added % 8];
            ms.wavelengths = cols[0];
            ms.reflectance = cols[i];
            for (double& v : ms.reflectance) if (v < 0) v = 0;
            newSpectra << ms;
            ++added;
        }

        if (added > 0) {
            std::unique_lock<std::shared_mutex> lk(m_mutex);
            m_spectra << newSpectra;
            buildCache();
        }
        return added;
    }

    // ── SAM 光谱角匹配（保留作回归基准，FCLS 启用后仍可对比使用）────────
    QVector<MatchResult> match(const QVector<double>& pixWl,
                               const QVector<double>& pixRef,
                               int topN = 5) const
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        if (pixWl.isEmpty() || pixRef.isEmpty() || m_spectra.isEmpty())
            return {};

        QVector<MatchResult> results;
        results.reserve(m_spectra.size());

        for (const MineralSpectrum& lib : m_spectra) {
            if (lib.wavelengths.size() < 2) continue;

            // 将像元光谱插值到库谱的波长点（或反之，取重叠区间）
            double wlMin = qMax(pixWl.first(),  lib.wavelengths.first());
            double wlMax = qMin(pixWl.last(),   lib.wavelengths.last());
            if (wlMax - wlMin < 100.0) continue;  // 重叠不足 100nm，跳过

            // 在重叠区间均匀采 50 个点
            const int N = 50;
            double dot = 0, normA = 0, normB = 0;
            for (int k = 0; k < N; ++k) {
                double wl = wlMin + (wlMax - wlMin) * k / (N - 1);
                double a = interp(pixWl, pixRef, wl);
                double b = interp(lib.wavelengths, lib.reflectance, wl);
                dot   += a * b;
                normA += a * a;
                normB += b * b;
            }
            if (normA < 1e-12 || normB < 1e-12) continue;
            double cosA = dot / (qSqrt(normA) * qSqrt(normB));
            cosA = qBound(-1.0, cosA, 1.0);
            double angle = qAcos(cosA);

            results.append({ lib.name, lib.nameEn, lib.source, angle });
        }

        std::sort(results.begin(), results.end(),
                  [](const MatchResult& a, const MatchResult& b){ return a.angle < b.angle; });
        if (results.size() > topN) results.resize(topN);
        return results;
    }

private:
    SpectralLibrary() {}

    // ── 数据 ─────────────────────────────────────────────────────────
    QList<MineralSpectrum> m_spectra;

    // ── 线程安全 ──────────────────────────────────────────────────────
    mutable std::shared_mutex m_mutex;  // match()/analyze() 持 shared_lock；load/clear 持 unique_lock

    // ── CR 预计算缓存 ─────────────────────────────────────────────────
    // 公共波长网格：400~2500 nm，步长 5 nm，共 421 点
    // 所有库谱在加载后统一插值到此网格并预计算 CR
    bool             m_cacheReady   = false;
    Eigen::VectorXd  m_commonWl;       // (421,)
    Eigen::MatrixXd  m_rawLibMatrix;   // (421, K)  原始反射率（已插值到公共网格）
    Eigen::MatrixXd  m_crLibMatrix;    // (421, K)  CR 处理后

    // ── buildCache（调用方必须已持有 unique_lock）─────────────────────
    // 先分配好矩阵（resize），再用 QtConcurrent::blockingMap 并行填列
    void buildCache()
    {
        const int GRID_START = 400, GRID_END = 2500, GRID_STEP = 5;
        const int GRID_N = (GRID_END - GRID_START) / GRID_STEP + 1;  // 421

        // 公共波长网格（只需构造一次）
        m_commonWl.resize(GRID_N);
        for (int i = 0; i < GRID_N; ++i)
            m_commonWl[i] = GRID_START + i * GRID_STEP;

        const int K = m_spectra.size();
        if (K == 0) { m_cacheReady = false; return; }

        // 先整体 resize，保证物理内存连续分配完毕，再并行填列
        m_rawLibMatrix.resize(GRID_N, K);
        m_crLibMatrix .resize(GRID_N, K);

        // 生成列索引列表供 blockingMap 迭代
        QVector<int> indices(K);
        std::iota(indices.begin(), indices.end(), 0);

        // 并行：每个 lambda 只写 col(i)，列间内存不重叠，线程安全
        SpectralAnalyzer analyzer;
        QtConcurrent::blockingMap(indices, [&](int i) {
            const MineralSpectrum& ms = m_spectra[i];

            // 将矿物谱插值到公共网格
            Eigen::VectorXd srcWl  = SpectralAnalyzer::fromQVector(ms.wavelengths);
            Eigen::VectorXd srcSp  = SpectralAnalyzer::fromQVector(ms.reflectance);

            Eigen::VectorXd col(GRID_N);
            col.setZero();
            for (int j = 0; j < GRID_N; ++j) {
                double wl = m_commonWl[j];
                if (wl >= srcWl[0] && wl <= srcWl[srcWl.size()-1])
                    col[j] = SpectralAnalyzer::linearInterp(srcWl, srcSp, wl);
                // 超出范围的波段保持 0（后续 analyze 会按重叠区裁剪）
            }
            m_rawLibMatrix.col(i) = col;

            // CR 预计算（只在有效范围内做 CR，其余置 1）
            Eigen::VectorXd crCol = Eigen::VectorXd::Ones(GRID_N);
            // 找有效波长段
            int jStart = 0, jEnd = GRID_N - 1;
            while (jStart < GRID_N && col[jStart] == 0.0) ++jStart;
            while (jEnd   > jStart && col[jEnd]   == 0.0) --jEnd;
            if (jEnd > jStart + 1) {
                Eigen::VectorXd subWl  = m_commonWl.segment(jStart, jEnd - jStart + 1);
                Eigen::VectorXd subSp  = col.segment(jStart, jEnd - jStart + 1);
                Eigen::VectorXd subCR  = analyzer.continuumRemoval(subWl, subSp);
                crCol.segment(jStart, subCR.size()) = subCR;
            }
            m_crLibMatrix.col(i) = crCol;
        });

        m_cacheReady = true;
    }

    // ── 线性插值（QVector 版，供旧版 SAM match 使用）─────────────────
    static double interp(const QVector<double>& xs, const QVector<double>& ys, double x) {
        if (x <= xs.first()) return ys.first();
        if (x >= xs.last())  return ys.last();
        int lo = 0, hi = xs.size() - 1;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (xs[mid] <= x) lo = mid; else hi = mid;
        }
        double t = (x - xs[lo]) / (xs[hi] - xs[lo]);
        return ys[lo] * (1.0 - t) + ys[hi] * t;
    }

public:
    // ── 主分析入口：SAM 粗筛 → FCLS 精解 → 置信度评分 ───────────────
    // pixWl / pixRef：像元波长和反射率（QVector，来自 DataWorker）
    // topN          ：返回的最大矿物条目数
    // 调用方在 QtConcurrent::run 中执行，不阻塞 UI 线程
    QVector<AnalysisDisplayEntry> analyze(const QVector<double>& pixWl,
                                          const QVector<double>& pixRef,
                                          int topN = 5) const
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        if (!m_cacheReady || m_spectra.isEmpty())
            return {};

        SpectralAnalyzer sa;
        const int K = m_spectra.size();

        // ── Step 1：像元预处理 ───────────────────────────────────────
        Eigen::VectorXd srcWl = SpectralAnalyzer::fromQVector(pixWl);
        Eigen::VectorXd srcSp = SpectralAnalyzer::fromQVector(pixRef);

        // SG 平滑（window=7，poly=2）
        srcSp = sa.savitzkyGolay(srcSp, 7, 2);

        // 对齐到公共网格，取回重叠区行索引
        AlignResult ar = sa.alignSpectrumWithIndices(srcWl, srcSp, m_commonWl);
        if (ar.empty()) return {};

        // 对齐后的子波长网格
        const int M = static_cast<int>(ar.indices.size());
        Eigen::VectorXd subWl(M);
        for (int j = 0; j < M; ++j)
            subWl[j] = m_commonWl[ar.indices[j]];

        // CR：像元
        Eigen::VectorXd pixCR = sa.continuumRemoval(subWl, ar.values);

        // ── Step 2：SAM 粗筛（在 CR 空间，提取 topN*4 候选）────────
        const int candidateN = std::min(topN * 4, K);
        struct SamEntry { int idx; double angle; };
        QVector<SamEntry> samScores;
        samScores.reserve(K);

        for (int i = 0; i < K; ++i) {
            // 从预计算 CR 矩阵中取出该矿物在重叠区的列
            Eigen::VectorXd libCol(M);
            for (int j = 0; j < M; ++j)
                libCol[j] = m_crLibMatrix(ar.indices[j], i);

            // 检查有效性：非零元素 < 10% 则跳过（覆盖率不足）
            int nonzero = (libCol.array() > 1e-6).count();
            if (nonzero < M / 10) continue;

            double angle = SpectralAnalyzer::spectralAngle(pixCR, libCol);
            samScores.append({i, angle});
        }

        std::sort(samScores.begin(), samScores.end(),
                  [](const SamEntry& a, const SamEntry& b){ return a.angle < b.angle; });
        if (samScores.size() > candidateN) samScores.resize(candidateN);
        if (samScores.isEmpty()) return {};

        // ── Step 3：FCLS 精解（在 CR 空间，端元矩阵取候选列）────────
        Eigen::MatrixXd endmembers(M, samScores.size());
        for (int ci = 0; ci < samScores.size(); ++ci) {
            int libIdx = samScores[ci].idx;
            for (int j = 0; j < M; ++j)
                endmembers(j, ci) = m_crLibMatrix(ar.indices[j], libIdx);
        }

        UnmixingResult umr = sa.fcls(pixCR, endmembers);

        // ── Step 4：构造显示条目，按丰度降序排列 ─────────────────────
        QVector<AnalysisDisplayEntry> entries;
        if (umr.abundances.size() == samScores.size()) {
            // 按丰度降序排索引
            QVector<int> order(samScores.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](int a, int b){
                return umr.abundances[a] > umr.abundances[b];
            });

            int shown = 0;
            for (int ci : order) {
                if (shown >= topN) break;
                double abund = umr.abundances[ci];
                if (abund < 0.01) continue;  // 丰度 < 1% 不显示

                const MineralSpectrum& ms = m_spectra[samScores[ci].idx];
                AnalysisDisplayEntry e;
                e.name           = ms.nameEn;
                e.source         = ms.source;
                e.abundance      = abund;
                e.isUnknown      = umr.isUnknown;
                // 置信度按比例分配（总分 × 丰度份额）
                e.confidenceScore = umr.isUnknown ? 0.0
                                  : umr.confidenceScore * abund;
                entries.append(e);
                ++shown;
            }
        }
        return entries;
    }

    // ── SAM 匹配入口（原始反射率，无 CR/FCLS，与 ENVI Spectral Analyst 一致）──
    QVector<AnalysisDisplayEntry> matchSAM(const QVector<double>& pixWl,
                                           const QVector<double>& pixRef,
                                           int topN = 5) const
    {
        QVector<MatchResult> raw = match(pixWl, pixRef, topN);
        QVector<AnalysisDisplayEntry> entries;
        entries.reserve(raw.size());
        for (const MatchResult& r : raw) {
            AnalysisDisplayEntry e;
            e.name            = r.nameEn;
            e.source          = r.source;
            e.abundance       = qRadiansToDegrees(r.angle);  // SAM 角度（度）
            e.confidenceScore = qCos(r.angle) * 100.0;
            e.isUnknown       = (r.angle > 0.15);       // 同 FCLS 门控阈值
            entries.append(e);
        }
        return entries;
    }
};
