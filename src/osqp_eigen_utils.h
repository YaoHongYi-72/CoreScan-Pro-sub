#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// osqp_eigen_utils.h
// Eigen Dense Matrix / Vector  ←→  OSQP CSC sparse format 互转工具
//
// 编译约定（匹配 Homebrew OSQP 1.0.0 构建配置）：
//   OSQPInt   = long long  （OSQP_USE_LONG 已定义）
//   OSQPFloat = double     （OSQP_USE_FLOAT 未定义）
//
// 使用规则：
//   1. eigenToOsqpCsc()  返回堆分配的 OSQPCscMatrix*，调用者负责调用
//      freeOsqpCsc() 释放（owned=0，三个数组由我们管理）。
//   2. 只处理普通稠密矩阵（所有元素均写入 CSC，不做稀疏剔零）。
//      对于 P = 2AᵀA 这种小规模半正定矩阵，稠密写法足够。
//   3. 零依赖：仅需 <Eigen/Dense> 和 <osqp/osqp.h>，无 Qt 依赖。
// ─────────────────────────────────────────────────────────────────────────────

#include <Eigen/Dense>
#include <osqp/osqp.h>

#include <cassert>
#include <cstring>
#include <stdexcept>

// ── 工厂函数：Eigen::MatrixXd → OSQPCscMatrix*（调用方拥有生命周期）──────────
//
// 返回的矩阵结构体及三个数据数组均用 new[] 分配，owned=0（由调用方管理）。
// 传入方阵或长方阵均可；上三角 / 对称标志由调用方在 osqp_setup 中指定。
inline OSQPCscMatrix* eigenToOsqpCsc(const Eigen::MatrixXd& M)
{
    const OSQPInt m   = static_cast<OSQPInt>(M.rows());
    const OSQPInt n   = static_cast<OSQPInt>(M.cols());
    const OSQPInt nnz = m * n;  // 稠密：所有元素均存储

    // 分配三个 CSC 数组
    OSQPFloat* x = new OSQPFloat[nnz];
    OSQPInt*   i = new OSQPInt[nnz];   // 行索引
    OSQPInt*   p = new OSQPInt[n + 1]; // 列指针

    OSQPInt idx = 0;
    for (OSQPInt col = 0; col < n; ++col) {
        p[col] = idx;
        for (OSQPInt row = 0; row < m; ++row) {
            x[idx] = static_cast<OSQPFloat>(M(row, col));
            i[idx] = row;
            ++idx;
        }
    }
    p[n] = idx;  // 末尾哨兵

    assert(idx == nnz);

    // 构造结构体
    OSQPCscMatrix* csc = new OSQPCscMatrix;
    csc->m     = m;
    csc->n     = n;
    csc->p     = p;
    csc->i     = i;
    csc->x     = x;
    csc->nzmax = nnz;
    csc->nz    = -1;   // -1 表示 CSC 格式（非 triplet）
    csc->owned = 0;    // 数组由调用方（我们）管理，OSQP 不应释放它们

    return csc;
}

// ── 对称上三角版本：只存 P 矩阵的上三角部分（OSQP 要求 P 只传上三角）────────
//
// OSQP 的 P 参数只需要上三角（含对角线），下三角被忽略。
// 用此函数而非 eigenToOsqpCsc() 来传递 P = 2AᵀA，节省一半内存。
inline OSQPCscMatrix* eigenToOsqpCscUpperTriangle(const Eigen::MatrixXd& M)
{
    assert(M.rows() == M.cols() && "上三角转换仅适用于方阵");

    const OSQPInt n = static_cast<OSQPInt>(M.cols());

    // 上三角非零元素数：n*(n+1)/2
    const OSQPInt nnz = n * (n + 1) / 2;

    OSQPFloat* x = new OSQPFloat[nnz];
    OSQPInt*   i = new OSQPInt[nnz];
    OSQPInt*   p = new OSQPInt[n + 1];

    OSQPInt idx = 0;
    for (OSQPInt col = 0; col < n; ++col) {
        p[col] = idx;
        for (OSQPInt row = 0; row <= col; ++row) {  // row <= col → 上三角
            x[idx] = static_cast<OSQPFloat>(M(row, col));
            i[idx] = row;
            ++idx;
        }
    }
    p[n] = idx;

    assert(idx == nnz);

    OSQPCscMatrix* csc = new OSQPCscMatrix;
    csc->m     = n;
    csc->n     = n;
    csc->p     = p;
    csc->i     = i;
    csc->x     = x;
    csc->nzmax = nnz;
    csc->nz    = -1;
    csc->owned = 0;

    return csc;
}

// ── 释放函数：与上面两个工厂函数配套使用 ────────────────────────────────────
//
// 只需传入工厂函数的返回值，内部按 owned=0 的约定释放三个数组和结构体本身。
// 传入 nullptr 是安全的（no-op）。
inline void freeOsqpCsc(OSQPCscMatrix* csc)
{
    if (!csc) return;
    delete[] csc->x;
    delete[] csc->i;
    delete[] csc->p;
    delete csc;
}

// ── OSQP Vector 辅助：Eigen::VectorXd → 裸 OSQPFloat 数组（调用方释放）──────
//
// osqp_setup 的 q / l / u 参数需要裸指针，用这个函数做临时转换。
// 返回 new[] 分配的数组，调用方用 delete[] 释放。
inline OSQPFloat* eigenVecToOsqp(const Eigen::VectorXd& v)
{
    const int n = static_cast<int>(v.size());
    OSQPFloat* arr = new OSQPFloat[n];
    for (int k = 0; k < n; ++k)
        arr[k] = static_cast<OSQPFloat>(v[k]);
    return arr;
}
