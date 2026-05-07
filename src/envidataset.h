#pragma once

#include <QString>
#include <QVector>
#include <QImage>
#include <QFile>
#include <QMetaType>

// ─────────────────────────────────────────────────────────────────────────────
// DatasetMeta — 纯数据结构（POD-like），可安全跨 Qt 线程边界传递
// 不继承 QObject，无虚函数，无线程亲和性问题
// ─────────────────────────────────────────────────────────────────────────────
struct DatasetMeta {
    int     index      = -1;
    QString name;
    int     samples    = 0;
    int     lines      = 0;
    int     bands      = 0;
    int     dataType   = 4;      // 4 = float32（ENVI 最常见格式）
    QString interleave = "bsq";
    QVector<double> wavelengths; // 每波段中心波长（nm），可为空
    bool    valid      = false;
};
Q_DECLARE_METATYPE(DatasetMeta)

// ─────────────────────────────────────────────────────────────────────────────
// EnviDataset — 高光谱数据引擎
//   · 不继承 QObject，纯数据模型，由 DataWorker 线程独占所有权
//   · 底层通过 QFile::map()（OS mmap）实现零拷贝随机寻址
//   · 2% Linear Stretch 算法直接在 mmap 指针上三遍扫描，不分配中间数组
//   · 所有像素读取经 QtEndian 严格校验字节序
// ─────────────────────────────────────────────────────────────────────────────
class EnviDataset {
public:
    EnviDataset() = default;
    ~EnviDataset() { unload(); }

    // 禁止拷贝（mmap 资源独占）
    EnviDataset(const EnviDataset&)            = delete;
    EnviDataset& operator=(const EnviDataset&) = delete;

    bool        load(const QString& hdrPath);
    void        unload();

    DatasetMeta meta()    const;
    bool        isValid() const { return m_map != nullptr; }

    // ── 供 Worker 线程调用的数据接口 ──────────────────────────────────
    // 返回指定波段的 2% Linear Stretch 灰度图（直接操作 mmap，无中间数组）
    QImage          getBandImage(int bandIdx)        const;
    // 真/假彩色 RGB 合成：3 通道 QtConcurrent 并行 2% 拉伸 → Format_RGB32
    QImage          getRGBComposite(int rBand, int gBand, int bBand) const;
    // RGB 合成（支持通道选择性显示）
    QImage          getRGBComposite(int rBand, int gBand, int bBand,
                                    bool visR, bool visG, bool visB) const;
    // 提取 (x, y) 处全波段光谱向量（字节序安全）
    QVector<double> getPixelSpectrum(int x, int y)  const;

    // 内置 mock：在 tmpDir 生成一个合法的小型 ENVI BSQ float32 文件用于测试
    static QString createMockEnviFile(const QString& tmpDir);

private:
    struct Header {
        int     samples      = 0;
        int     lines        = 0;
        int     bands        = 0;
        int     dataType     = 4;
        int     byteOrder    = 0;    // 0=little-endian, 1=big-endian
        qint64  headerOffset = 0;
        QString interleave   = "bsq";
        QVector<double> wavelengths;
    } m_hdr;

    mutable QFile m_imgFile;
    uchar*        m_map     = nullptr;
    qint64        m_mapSize = 0;
    int           m_index   = -1;
    QString       m_name;

    // ── 内部实现 ──────────────────────────────────────────────────────
    void    parseHdr(const QString& hdrPath);       // 状态机解析器
    int     bytesPerPixel()                     const;
    qint64  pixelOffset(int band, int line, int sample) const;
    double  readValue(qint64 byteOffset)        const; // QtEndian 字节序分派

    // 2% Linear Stretch 参数（min/max pass + 直方图 pass），供单波段和 RGB 共用
    struct StretchParams { float low, high, invSpan; };
    StretchParams computeStretch(int bandIdx)   const;
    QImage  renderWithStretch(int bandIdx)      const; // 三遍 mmap 扫描（调用上方方法）
};
