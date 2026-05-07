#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SpectralAnalyzer — 纯数学引擎，零 Qt UI 依赖
//
// 职责：
//   • Savitzky-Golay 平滑（去噪）
//   • 波长截断与插值对齐
//   • 连续统去除（Continuum Removal）
//   • FCLS 解混（OSQP 求解器）
//   • 双重门控置信度评分
//
// 约束：
//   • 本文件及其 .cpp 内，唯一允许出现的 Qt 类型是 QVector<double>
//     （对外接口层，方便与现有 DataWorker 信号槽对接）
//   • 内部计算全部使用 Eigen::VectorXd / Eigen::MatrixXd
//   • 禁止包含任何 QWidget / QDialog / QLabel 等 UI 头文件
// ─────────────────────────────────────────────────────────────────────────────

#include <Eigen/Dense>
#include <QVector>
#include <QString>
#include <QList>

// ─────────────────────────────────────────────────────────────────────────────
// 结构体：对齐结果（含回填索引，用于从预计算库矩阵中提取对应行）
// ─────────────────────────────────────────────────────────────────────────────
struct AlignResult {
    Eigen::VectorXd  values;   // 对齐后的光谱值（长度 = indices.size()）
    std::vector<int> indices;  // 对应的目标网格行索引（indices[k] → tgtWl[indices[k]]）
    bool empty() const { return values.size() == 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// 结构体：UI 展示条目（从 UnmixingResult + 矿物名称查找中生成）
// ─────────────────────────────────────────────────────────────────────────────
struct AnalysisDisplayEntry {
    QString name;            // 矿物名称
    QString source;          // 数据来源（库名）
    double  abundance  = 0;  // 丰度（0~1）
    double  confidenceScore = 0;  // 置信度（0~100）
    bool    isUnknown  = true;    // 是否未通过双重门控
};

// ─────────────────────────────────────────────────────────────────────────────
// 结构体：单次解混结果
// ─────────────────────────────────────────────────────────────────────────────
struct UnmixingResult
{
    // 每种端元的丰度（长度 = 端元数，之和 = 1.0）
    Eigen::VectorXd abundances;

    // 重建光谱 y_rec = A * abundances
    Eigen::VectorXd reconstructed;

    // 光谱角（弧度），越小越相似
    double spectralAngle = 0.0;

    // 归一化均方根误差 NRMSE
    double nrmse = 0.0;

    // 联合置信度得分（0~100%），isUnknown=true 时无效
    double confidenceScore = 0.0;

    // 是否未通过双重门控（任一超阈则为 true，判"未知物质"）
    bool isUnknown = true;

    // 求解器状态字符串（"solved" / "max_iter_reached" 等）
    QString solverStatus;
};

// ─────────────────────────────────────────────────────────────────────────────
// 结构体：SAM 粗筛单条结果（供 FCLS 前的候选端元筛选使用）
// ─────────────────────────────────────────────────────────────────────────────
struct SamResult
{
    QString name;
    QString nameEn;
    QString source;
    double  angle = 0.0;   // 光谱角（弧度）
};

// ─────────────────────────────────────────────────────────────────────────────
// SpectralAnalyzer
// ─────────────────────────────────────────────────────────────────────────────
class SpectralAnalyzer
{
public:
    SpectralAnalyzer() = default;

    // ── 双重门控阈值（ADR 约定，可覆盖用于测试）──────────────────────────
    double thresholdSA    = 0.15;   // 光谱角硬阈值（弧度）
    double thresholdNRMSE = 0.08;   // NRMSE 硬阈值

    // 置信度权重
    double lambdaSA    = 0.6;
    double lambdaNRMSE = 0.4;

    // ── Step 1：Savitzky-Golay 平滑 ───────────────────────────────────────
    // spectrum：输入光谱（长度 N）
    // window  ：滤波窗口宽度，必须为奇数且 ≥ poly+1（建议 5 或 7）
    // poly    ：拟合多项式阶数（建议 2 或 3）
    // 返回：平滑后光谱，长度与输入相同；两端用原始值填充（不做镜像延伸）
    Eigen::VectorXd savitzkyGolay(const Eigen::VectorXd& spectrum,
                                  int window = 5,
                                  int poly   = 2) const;

    // ── Step 2：波长截断与插值对齐 ────────────────────────────────────────
    // 将 (srcWl, srcSpec) 对齐到 tgtWl 定义的波长网格
    // 只保留 [max(srcWl.first, tgtWl.first), min(srcWl.last, tgtWl.last)] 重叠区间
    // 重叠区间内使用线性插值；严禁外推
    // 若重叠区间长度 < 100 nm，返回空向量（调用方应跳过该端元）
    // 输出：长度等于 tgtWl 在重叠区间内的点数
    Eigen::VectorXd alignSpectrum(const Eigen::VectorXd& srcWl,
                                  const Eigen::VectorXd& srcSpec,
                                  const Eigen::VectorXd& tgtWl) const;

    // 同上，但额外返回所用的目标网格行索引（供从预计算矩阵中提取对应行使用）
    AlignResult alignSpectrumWithIndices(const Eigen::VectorXd& srcWl,
                                         const Eigen::VectorXd& srcSpec,
                                         const Eigen::VectorXd& tgtWl) const;

    // ── Step 3：连续统去除（Continuum Removal）───────────────────────────
    // wavelengths / spectrum 长度必须相同且已通过 alignSpectrum 对齐
    // 返回：CR 后光谱，与输入等长；端点值强制为 1.0，所有值钳制 ≤ 1.0
    Eigen::VectorXd continuumRemoval(const Eigen::VectorXd& wavelengths,
                                     const Eigen::VectorXd& spectrum) const;

    // ── Step 4：FCLS 解混（OSQP QP 求解器）──────────────────────────────
    // pixel  ：像元光谱（已做 SG + 对齐 + CR，长度 D）
    // library：端元矩阵（D × K，每列一个端元，已做同样预处理）
    // 返回：UnmixingResult（含丰度、重建谱、SA、NRMSE、置信度）
    UnmixingResult fcls(const Eigen::VectorXd& pixel,
                        const Eigen::MatrixXd& library) const;

    // ── 双重门控 + 置信度计算（fcls 内部调用，也可独立使用）─────────────
    // sa    ：光谱角（弧度）
    // nrmse ：NRMSE
    // 填充 result.isUnknown 和 result.confidenceScore
    void applyGatingAndScore(double sa, double nrmse,
                             UnmixingResult& result) const;

    // ── 工具：QVector<double> ↔ Eigen::VectorXd 互转（接口层使用）────────
    static Eigen::VectorXd fromQVector(const QVector<double>& v);
    static QVector<double> toQVector(const Eigen::VectorXd& v);

    // 线性插值（public static，供 SpectralLibrary 的 buildCache 直接调用）
    static double linearInterp(const Eigen::VectorXd& xs,
                               const Eigen::VectorXd& ys,
                               double x);

    // 计算光谱角（弧度），两向量长度必须相同
    static double spectralAngle(const Eigen::VectorXd& a,
                                const Eigen::VectorXd& b);

    // 计算 NRMSE
    // D = a.size()，分母 = sqrt(D) * (max(ref) - min(ref))
    static double computeNRMSE(const Eigen::VectorXd& ref,
                               const Eigen::VectorXd& rec);

private:
    static Eigen::VectorXd sgCoeffs(int window, int poly);
};
