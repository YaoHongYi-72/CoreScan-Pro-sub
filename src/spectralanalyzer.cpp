// ─────────────────────────────────────────────────────────────────────────────
// spectralanalyzer.cpp
// Phase 1 骨架：各方法目前仅返回空/零值占位，Phase 2 逐一实现。
// ─────────────────────────────────────────────────────────────────────────────

#include "spectralanalyzer.h"

#include <osqp/osqp.h>
#include "osqp_eigen_utils.h"

#include <Eigen/Dense>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <vector>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// 工具：QVector ↔ Eigen
// ─────────────────────────────────────────────────────────────────────────────

Eigen::VectorXd SpectralAnalyzer::fromQVector(const QVector<double>& v)
{
    Eigen::VectorXd ev(v.size());
    for (int k = 0; k < v.size(); ++k)
        ev[k] = v[k];
    return ev;
}

QVector<double> SpectralAnalyzer::toQVector(const Eigen::VectorXd& v)
{
    QVector<double> qv(static_cast<int>(v.size()));
    for (int k = 0; k < static_cast<int>(v.size()); ++k)
        qv[k] = v[k];
    return qv;
}

// ─────────────────────────────────────────────────────────────────────────────
// 私有：线性插值
// ─────────────────────────────────────────────────────────────────────────────

double SpectralAnalyzer::linearInterp(const Eigen::VectorXd& xs,
                                      const Eigen::VectorXd& ys,
                                      double x)
{
    // 边界：严禁外推，直接返回端点值（由对齐函数保证 x 在区间内）
    if (x <= xs[0])              return ys[0];
    if (x >= xs[xs.size() - 1]) return ys[ys.size() - 1];

    // 二分查找
    int lo = 0, hi = static_cast<int>(xs.size()) - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (xs[mid] <= x) lo = mid; else hi = mid;
    }
    double t = (x - xs[lo]) / (xs[hi] - xs[lo]);
    return ys[lo] * (1.0 - t) + ys[hi] * t;
}

// ─────────────────────────────────────────────────────────────────────────────
// 私有：光谱角
// ─────────────────────────────────────────────────────────────────────────────

double SpectralAnalyzer::spectralAngle(const Eigen::VectorXd& a,
                                       const Eigen::VectorXd& b)
{
    double dot   = a.dot(b);
    double normA = a.norm();
    double normB = b.norm();
    if (normA < 1e-12 || normB < 1e-12) return M_PI / 2.0;
    double cosA = std::clamp(dot / (normA * normB), -1.0, 1.0);
    return std::acos(cosA);
}

// ─────────────────────────────────────────────────────────────────────────────
// 私有：NRMSE
// ─────────────────────────────────────────────────────────────────────────────

double SpectralAnalyzer::computeNRMSE(const Eigen::VectorXd& ref,
                                      const Eigen::VectorXd& rec)
{
    int D = static_cast<int>(ref.size());
    if (D == 0) return 1.0;

    double rng = ref.maxCoeff() - ref.minCoeff();
    if (rng < 1e-12) return 1.0;  // 平坦参考谱，无法计算有意义的 NRMSE

    Eigen::VectorXd diff = ref - rec;
    double rmse = std::sqrt(diff.squaredNorm() / D);
    return rmse / (std::sqrt(static_cast<double>(D)) * rng);
}

// ─────────────────────────────────────────────────────────────────────────────
// 私有：SG 滤波器系数（Gram 多项式法，返回中心点卷积核）
//
// 算法：在 [-h, h]（h = window/2）上用最小二乘拟合 poly 阶多项式，
// 取中心点（index=h）的拟合值对应的卷积系数。
// 等价于：构造 Vandermonde 矩阵 J（window × (poly+1)），
// 系数 = (JᵀJ)⁻¹Jᵀ 的第 h 行（即中心点的伪逆行）。
// ─────────────────────────────────────────────────────────────────────────────

Eigen::VectorXd SpectralAnalyzer::sgCoeffs(int window, int poly)
{
    Q_ASSERT(window % 2 == 1 && window >= poly + 1);

    int h = window / 2;  // 半窗口大小

    // 构造 Vandermonde 矩阵 J（window 行 × poly+1 列）
    // J[i][p] = (i - h)^p，i ∈ [0, window)，p ∈ [0, poly]
    Eigen::MatrixXd J(window, poly + 1);
    for (int i = 0; i < window; ++i) {
        double xi = static_cast<double>(i - h);
        double xp = 1.0;
        for (int p = 0; p <= poly; ++p) {
            J(i, p) = xp;
            xp *= xi;
        }
    }

    // 伪逆：(JᵀJ)⁻¹Jᵀ，形状 (poly+1) × window
    // 中心点行 = 伪逆的第 h 列（对应 x=0 的预测值）
    Eigen::MatrixXd pinv = (J.transpose() * J).ldlt().solve(J.transpose());

    // 中心点系数 = 伪逆第 0 行（p=0 项，即常数项，对应 x=0 时的拟合值）
    // 等价于 J[h] · pinv，直接取 pinv 的第 0 行（因为 J_center = [1,0,0,...]）
    return pinv.row(0);  // 形状 (window,)
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 1：Savitzky-Golay 平滑
// ─────────────────────────────────────────────────────────────────────────────

Eigen::VectorXd SpectralAnalyzer::savitzkyGolay(const Eigen::VectorXd& spectrum,
                                                 int window,
                                                 int poly) const
{
    const int N = static_cast<int>(spectrum.size());

    // 参数合法性修正：window 必须为奇数且 ≥ poly+1，且 ≤ N
    if (window % 2 == 0) window += 1;
    window = std::min(window, N % 2 == 0 ? N - 1 : N);
    window = std::max(window, poly + 1);
    if (window % 2 == 0) window += 1;

    // 若窗口退化（信号太短），直接返回原始信号
    if (window > N || N < 3) return spectrum;

    int h = window / 2;

    // 获取中心点卷积核系数（长度 = window）
    Eigen::VectorXd coeff = sgCoeffs(window, poly);

    Eigen::VectorXd result(N);

    for (int i = 0; i < N; ++i) {
        if (i < h || i >= N - h) {
            // 边界区域：填充原始值，不做平滑（避免引入虚假边界特征）
            result[i] = spectrum[i];
        } else {
            // 中心区域：卷积
            double val = 0.0;
            for (int k = 0; k < window; ++k)
                val += coeff[k] * spectrum[i - h + k];
            result[i] = val;
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 2：波长对齐与截断
// ─────────────────────────────────────────────────────────────────────────────

Eigen::VectorXd SpectralAnalyzer::alignSpectrum(const Eigen::VectorXd& srcWl,
                                                 const Eigen::VectorXd& srcSpec,
                                                 const Eigen::VectorXd& tgtWl) const
{
    AlignResult ar = alignSpectrumWithIndices(srcWl, srcSpec, tgtWl);
    return ar.values;
}

AlignResult SpectralAnalyzer::alignSpectrumWithIndices(const Eigen::VectorXd& srcWl,
                                                        const Eigen::VectorXd& srcSpec,
                                                        const Eigen::VectorXd& tgtWl) const
{
    AlignResult ar;
    if (srcWl.size() < 2 || srcSpec.size() != srcWl.size() || tgtWl.size() < 1)
        return ar;

    double wlMin = std::max(srcWl[0],              tgtWl[0]);
    double wlMax = std::min(srcWl[srcWl.size()-1], tgtWl[tgtWl.size()-1]);

    if (wlMax - wlMin < 100.0) return ar;

    for (int j = 0; j < static_cast<int>(tgtWl.size()); ++j) {
        if (tgtWl[j] >= wlMin && tgtWl[j] <= wlMax)
            ar.indices.push_back(j);
    }

    if (ar.indices.empty()) return ar;

    ar.values.resize(static_cast<int>(ar.indices.size()));
    for (int k = 0; k < static_cast<int>(ar.indices.size()); ++k)
        ar.values[k] = linearInterp(srcWl, srcSpec, tgtWl[ar.indices[k]]);

    return ar;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 3：连续统去除
// ─────────────────────────────────────────────────────────────────────────────

Eigen::VectorXd SpectralAnalyzer::continuumRemoval(const Eigen::VectorXd& wavelengths,
                                                    const Eigen::VectorXd& spectrum) const
{
    const int N = static_cast<int>(spectrum.size());
    Q_ASSERT(wavelengths.size() == spectrum.size() && N >= 2);

    // ── 1. 构造"上凸包"（convex hull of the upper envelope）──────────────
    //
    // 算法：单调栈，保留所有使折线"向左转"的点（即上凸）。
    // 使用斜率比较：从左到右，若新点使当前折线向下弯，则弹栈。
    //
    // 强制锚定：端点 0 和 N-1 无条件入栈，保证端点 CR = 1.0。

    std::vector<int> hull;
    hull.push_back(0);

    for (int i = 1; i < N; ++i) {
        // 弹出不满足上凸条件的点
        while (hull.size() >= 2) {
            int a = hull[hull.size() - 2];
            int b = hull[hull.size() - 1];
            // 检查 b 是否在 a→i 连线的下方（或等于），若是则弹出 b
            // 上凸条件：cross product (b-a) × (i-a) <= 0
            double ax = wavelengths[a], ay = spectrum[a];
            double bx = wavelengths[b], by = spectrum[b];
            double ix = wavelengths[i], iy = spectrum[i];
            // cross = (bx-ax)*(iy-ay) - (by-ay)*(ix-ax)
            // cross <= 0 → b 在 a→i 连线上方或等于（上凸，保留）
            // cross >  0 → b 在 a→i 连线下方（非上凸，弹出）
            double cross = (bx - ax) * (iy - ay) - (by - ay) * (ix - ax);
            if (cross > 0.0)
                hull.pop_back();
            else
                break;
        }
        hull.push_back(i);
    }

    // 确保最后一个点在栈中（端点锚定）
    if (hull.back() != N - 1) hull.push_back(N - 1);

    // ── 2. 构造"凸包上包络"逐点插值，并计算 CR = spectrum / envelope ──

    Eigen::VectorXd cr(N);
    int hIdx = 0;  // 当前凸包段的左端点索引

    for (int i = 0; i < N; ++i) {
        // 推进凸包段：找到包含当前点 i 的段 [hull[hIdx], hull[hIdx+1]]
        while (hIdx + 1 < static_cast<int>(hull.size()) - 1
               && hull[hIdx + 1] <= i)
        {
            ++hIdx;
        }

        int   la = hull[hIdx];
        int   lb = hull[hIdx + 1];
        double xa = wavelengths[la], ya = spectrum[la];
        double xb = wavelengths[lb], yb = spectrum[lb];

        double envelope;
        if (std::abs(xb - xa) < 1e-10) {
            envelope = std::max(ya, yb);
        } else {
            double t = (wavelengths[i] - xa) / (xb - xa);
            envelope = ya + t * (yb - ya);
        }

        // 端点强制 CR = 1.0（锚定）
        if (i == 0 || i == N - 1) {
            cr[i] = 1.0;
        } else {
            // 防除零 + 钳制：物理上 CR 不应超过 1.0
            cr[i] = (envelope > 1e-12)
                    ? std::min(spectrum[i] / envelope, 1.0)
                    : 1.0;
        }
    }

    return cr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 双重门控 + 置信度
// ─────────────────────────────────────────────────────────────────────────────

void SpectralAnalyzer::applyGatingAndScore(double sa, double nrmse,
                                           UnmixingResult& result) const
{
    result.spectralAngle = sa;
    result.nrmse         = nrmse;

    // 硬阈值双重门控
    if (sa >= thresholdSA || nrmse >= thresholdNRMSE) {
        result.isUnknown      = true;
        result.confidenceScore = 0.0;
        return;
    }

    result.isUnknown = false;

    // 指数衰减置信度：Score = 100 × exp(-(λ₁·SA/τ₁ + λ₂·NRMSE/τ₂))
    double exponent = lambdaSA    * (sa    / thresholdSA)
                    + lambdaNRMSE * (nrmse / thresholdNRMSE);
    result.confidenceScore = 100.0 * std::exp(-exponent);
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 4：FCLS 解混（OSQP）
// ─────────────────────────────────────────────────────────────────────────────

UnmixingResult SpectralAnalyzer::fcls(const Eigen::VectorXd& pixel,
                                      const Eigen::MatrixXd& library) const
{
    UnmixingResult result;

    const int D = static_cast<int>(library.rows());  // 波段数
    const int K = static_cast<int>(library.cols());  // 端元数

    if (D < 1 || K < 1 || pixel.size() != D) {
        result.solverStatus = "invalid_input";
        return result;
    }

    // ── 1. 构造 QP 矩阵 ──────────────────────────────────────────────────
    //
    // 目标函数：min ½ xᵀPx + qᵀx
    //   P = 2AᵀA  （ADR 约定，保留对偶变量的物理量纲）
    //   q = -2Aᵀy
    //
    // 约束：l ≤ Cx ≤ u
    //   ANC（非负）：-I·x ≤ 0  →  C 上 K 行 = -I，l[0..K-1] = -∞，u[0..K-1] = 0
    //   ASC（和为1）：1ᵀx = 1  →  C 最后 1 行 = 1ᵀ，l[K] = u[K] = 1

    const Eigen::MatrixXd A = library;  // D×K
    const Eigen::VectorXd y = pixel;    // D

    Eigen::MatrixXd P_dense = 2.0 * A.transpose() * A;  // K×K
    Eigen::VectorXd q_vec   = -2.0 * A.transpose() * y; // K

    // 约束矩阵 C：(K+1)×K
    Eigen::MatrixXd C_dense = Eigen::MatrixXd::Zero(K + 1, K);
    C_dense.topRows(K)    = -Eigen::MatrixXd::Identity(K, K);  // ANC
    C_dense.bottomRows(1) =  Eigen::MatrixXd::Ones(1, K);      // ASC

    // 约束上下界
    const double kInf = 1e30;
    Eigen::VectorXd l_vec(K + 1), u_vec(K + 1);
    l_vec.head(K).setConstant(-kInf);  // ANC 下界：-∞
    u_vec.head(K).setConstant(0.0);    // ANC 上界：0（配合 -I 实现 x ≥ 0）
    l_vec[K] = 1.0;                    // ASC 等式约束
    u_vec[K] = 1.0;

    // ── 2. 转换为 OSQP CSC 格式 ──────────────────────────────────────────
    OSQPCscMatrix* P_csc = eigenToOsqpCscUpperTriangle(P_dense);
    OSQPCscMatrix* C_csc = eigenToOsqpCsc(C_dense);
    OSQPFloat*     q_raw = eigenVecToOsqp(q_vec);
    OSQPFloat*     l_raw = eigenVecToOsqp(l_vec);
    OSQPFloat*     u_raw = eigenVecToOsqp(u_vec);

    // ── 3. 配置 OSQP 求解器 ───────────────────────────────────────────────
    OSQPSettings settings;
    osqp_set_default_settings(&settings);
    settings.eps_abs      = 1e-5;
    settings.eps_rel      = 1e-5;
    settings.max_iter     = 4000;
    settings.warm_starting = 1;
    settings.verbose      = 0;   // 静默模式，不打印迭代信息
    settings.polishing    = 1;   // 开启 polish，提高解的精度

    OSQPSolver* solver = nullptr;
    OSQPInt exitflag = osqp_setup(&solver,
                                  P_csc, q_raw,
                                  C_csc, l_raw, u_raw,
                                  static_cast<OSQPInt>(K + 1),  // m：约束数
                                  static_cast<OSQPInt>(K),      // n：变量数
                                  &settings);

    // 释放临时 CSC 内存（setup 完成后 OSQP 已内部复制，原始数组可释放）
    freeOsqpCsc(P_csc);
    freeOsqpCsc(C_csc);
    delete[] q_raw;
    delete[] l_raw;
    delete[] u_raw;

    if (exitflag != 0 || solver == nullptr) {
        result.solverStatus = "setup_failed";
        if (solver) osqp_cleanup(solver);
        return result;
    }

    // ── 4. 求解 ───────────────────────────────────────────────────────────
    osqp_solve(solver);

    // 读取状态
    const char* statusStr = solver->info->status;
    result.solverStatus = QString::fromLatin1(statusStr);

    bool solved = (solver->info->status_val == OSQP_SOLVED ||
                   solver->info->status_val == OSQP_SOLVED_INACCURATE);

    if (solved && solver->solution && solver->solution->x) {
        // ── 5. 提取解并后处理 ─────────────────────────────────────────────
        Eigen::VectorXd x(K);
        for (int k = 0; k < K; ++k)
            x[k] = static_cast<double>(solver->solution->x[k]);

        // 钳制负值（数值误差可能产生极小负数）
        x = x.cwiseMax(0.0);

        // 重新归一化（防止数值误差破坏 ASC）
        double sumX = x.sum();
        if (sumX > 1e-10)
            x /= sumX;
        else
            x.setConstant(1.0 / K);  // 退化情况：均匀分配

        result.abundances    = x;
        result.reconstructed = A * x;

        // ── 6. 计算 SA 和 NRMSE，应用双重门控 ────────────────────────────
        double sa    = spectralAngle(y, result.reconstructed);
        double nrmse = computeNRMSE(y, result.reconstructed);
        applyGatingAndScore(sa, nrmse, result);
    } else {
        result.isUnknown = true;
    }

    osqp_cleanup(solver);
    return result;
}
