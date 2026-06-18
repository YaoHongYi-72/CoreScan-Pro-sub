#include "envidataset.h"

#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QtEndian>
#include <QtConcurrent>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// 公开接口
// ─────────────────────────────────────────────────────────────────────────────

bool EnviDataset::load(const QString& hdrPath)
{
    unload();
    m_name = QFileInfo(hdrPath).completeBaseName();

    parseHdr(hdrPath);
    if (m_hdr.samples <= 0 || m_hdr.lines <= 0 || m_hdr.bands <= 0)
        return false;

    // 确定 .img 文件路径（与 .hdr 同名，扩展名替换）
    QFileInfo fi(hdrPath);
    QStringList candidates = {
        fi.dir().filePath(fi.completeBaseName() + ".img"),
        fi.dir().filePath(fi.completeBaseName() + ".dat"),
        fi.dir().filePath(fi.completeBaseName()),   // 无扩展名
        hdrPath                                     // 数据内嵌在 .hdr（含 header offset）
    };

    QString imgPath;
    for (const QString& c : candidates) {
        if (QFile::exists(c) && c != hdrPath) { imgPath = c; break; }
    }
    // 若候选均不存在，尝试去掉 .hdr 后缀
    if (imgPath.isEmpty()) {
        QString bare = hdrPath;
        if (bare.endsWith(".hdr", Qt::CaseInsensitive)) bare.chop(4);
        if (QFile::exists(bare)) imgPath = bare;
    }
    if (imgPath.isEmpty()) return false;

    // ── mmap ──────────────────────────────────────────────────────────
    m_imgFile.setFileName(imgPath);
    if (!m_imgFile.open(QIODevice::ReadOnly)) return false;

    m_mapSize = m_imgFile.size();
    m_map = m_imgFile.map(0, m_mapSize);
    if (!m_map) { m_imgFile.close(); return false; }

    return true;
}

void EnviDataset::unload()
{
    if (m_map) {
        m_imgFile.unmap(m_map);
        m_map = nullptr;
    }
    if (m_imgFile.isOpen()) m_imgFile.close();
    m_mapSize = 0;
    m_hdr = Header{};
}

DatasetMeta EnviDataset::meta() const
{
    DatasetMeta d;
    d.index      = m_index;
    d.name       = m_name;
    d.samples    = m_hdr.samples;
    d.lines      = m_hdr.lines;
    d.bands      = m_hdr.bands;
    d.dataType   = m_hdr.dataType;
    d.interleave = m_hdr.interleave;
    d.wavelengths = m_hdr.wavelengths;
    d.valid      = isValid();
    return d;
}

QImage EnviDataset::getBandImage(int bandIdx) const
{
    if (!isValid() || bandIdx < 0 || bandIdx >= m_hdr.bands)
        return {};
    return renderWithStretch(bandIdx);
}

QVector<double> EnviDataset::getPixelSpectrum(int x, int y) const
{
    if (!isValid() || x < 0 || x >= m_hdr.samples || y < 0 || y >= m_hdr.lines)
        return {};

    QVector<double> spec(m_hdr.bands);
    for (int b = 0; b < m_hdr.bands; ++b)
        spec[b] = readValue(pixelOffset(b, y, x));
    return spec;
}

// ─────────────────────────────────────────────────────────────────────────────
// 状态机 .hdr 解析器
// ─────────────────────────────────────────────────────────────────────────────
void EnviDataset::parseHdr(const QString& hdrPath)
{
    QFile f(hdrPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);

    // 用于匹配 "key = value" 和 "key = {"
    static const QRegularExpression reKeyVal(R"(^\s*([^=]+?)\s*=\s*(.+)$)");
    static const QRegularExpression reKeyBrace(R"(^\s*([^=]+?)\s*=\s*\{)");

    enum class State { IDLE, IN_BRACE } state = State::IDLE;
    QString braceKey;
    QString braceBuffer;

    auto applyKeyVal = [&](const QString& key, const QString& val) {
        QString k = key.trimmed().toLower();
        QString v = val.trimmed();
        if      (k == "samples")        m_hdr.samples      = v.toInt();
        else if (k == "lines")          m_hdr.lines        = v.toInt();
        else if (k == "bands")          m_hdr.bands        = v.toInt();
        else if (k == "data type")      m_hdr.dataType     = v.toInt();
        else if (k == "byte order")     m_hdr.byteOrder    = v.toInt();
        else if (k == "header offset")  m_hdr.headerOffset = v.toLongLong();
        else if (k == "interleave")     m_hdr.interleave   = v.toLower();
    };

    auto parseList = [&](const QString& key, const QString& raw) {
        if (key.trimmed().toLower() != "wavelength") return;
        // 从 { ... } 内部提取数值
        QString content = raw;
        int lb = content.indexOf('{'); if (lb >= 0) content = content.mid(lb + 1);
        int rb = content.indexOf('}'); if (rb >= 0) content = content.left(rb);
        const QStringList parts = content.split(',', Qt::SkipEmptyParts);
        m_hdr.wavelengths.reserve(parts.size());
        for (const QString& p : parts) {
            bool ok;
            double w = p.trimmed().toDouble(&ok);
            if (ok) m_hdr.wavelengths.append(w);
        }
    };

    while (!in.atEnd()) {
        QString line = in.readLine();

        if (state == State::IDLE) {
            // 跳过文件头标识行
            if (line.trimmed().compare("ENVI", Qt::CaseInsensitive) == 0) continue;

            // 检查是否是 "key = {" 格式
            auto mBrace = reKeyBrace.match(line);
            if (mBrace.hasMatch()) {
                braceKey    = mBrace.captured(1);
                braceBuffer = line;

                if (line.contains('}')) {
                    // 内联列表：同一行包含 { ... }
                    parseList(braceKey, braceBuffer);
                    braceKey.clear(); braceBuffer.clear();
                } else {
                    state = State::IN_BRACE;
                }
                continue;
            }

            // 普通 key = value
            auto mKV = reKeyVal.match(line);
            if (mKV.hasMatch())
                applyKeyVal(mKV.captured(1), mKV.captured(2));

        } else {  // IN_BRACE
            braceBuffer += ' ' + line;
            if (line.contains('}')) {
                parseList(braceKey, braceBuffer);
                braceKey.clear(); braceBuffer.clear();
                state = State::IDLE;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 像素寻址
// ─────────────────────────────────────────────────────────────────────────────
int EnviDataset::bytesPerPixel() const
{
    switch (m_hdr.dataType) {
    case 1:  return 1;   // uint8
    case 2:  return 2;   // int16
    case 3:  return 4;   // int32
    case 4:  return 4;   // float32
    case 5:  return 8;   // float64
    case 12: return 2;   // uint16
    case 13: return 4;   // uint32
    default: return 1;
    }
}

qint64 EnviDataset::pixelOffset(int band, int line, int sample) const
{
    const qint64 bpp  = bytesPerPixel();
    const qint64 base = m_hdr.headerOffset;
    const qint64 S    = m_hdr.samples;
    const qint64 L    = m_hdr.lines;
    const qint64 B    = m_hdr.bands;

    if (m_hdr.interleave == "bsq")
        return base + (band * L * S + (qint64)line * S + sample) * bpp;
    if (m_hdr.interleave == "bil")
        return base + ((qint64)line * B * S + band * S + sample) * bpp;
    // bip（默认）
    return base + ((qint64)line * S * B + (qint64)sample * B + band) * bpp;
}

// ─────────────────────────────────────────────────────────────────────────────
// 字节序安全读值（QtEndian 严格校验）
// ─────────────────────────────────────────────────────────────────────────────
double EnviDataset::readValue(qint64 offset) const
{
    if (offset < 0 || offset + bytesPerPixel() > m_mapSize) return 0.0;

    const uchar* p  = m_map + offset;
    const bool   le = (m_hdr.byteOrder == 0);  // 0 = little-endian

    switch (m_hdr.dataType) {
    case 1:
        return static_cast<double>(*p);

    case 2: {
        qint16 v = le ? qFromLittleEndian<qint16>(p) : qFromBigEndian<qint16>(p);
        return static_cast<double>(v);
    }
    case 3: {
        qint32 v = le ? qFromLittleEndian<qint32>(p) : qFromBigEndian<qint32>(p);
        return static_cast<double>(v);
    }
    case 4: {
        // float32：先以无符号整型读取位模式，再 memcpy 还原浮点——规避 strict-aliasing
        quint32 raw = le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
        float v; std::memcpy(&v, &raw, sizeof(float));
        return static_cast<double>(std::isfinite(v) ? v : 0.f);
    }
    case 5: {
        quint64 raw = le ? qFromLittleEndian<quint64>(p) : qFromBigEndian<quint64>(p);
        double v; std::memcpy(&v, &raw, sizeof(double));
        return std::isfinite(v) ? v : 0.0;
    }
    case 12: {
        quint16 v = le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
        return static_cast<double>(v);
    }
    case 13: {
        quint32 v = le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
        return static_cast<double>(v);
    }
    default:
        return 0.0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2% Linear Stretch — 提取拉伸参数（两遍直接扫描 mmap，零中间数组）
// ─────────────────────────────────────────────────────────────────────────────
EnviDataset::StretchParams EnviDataset::computeStretch(int bandIdx) const
{
    const int S = m_hdr.samples;
    const int L = m_hdr.lines;
    const int N = S * L;

    // 第一遍：min / max
    float mn = FLT_MAX, mx = -FLT_MAX;
    for (int y = 0; y < L; ++y)
        for (int x = 0; x < S; ++x) {
            float v = static_cast<float>(readValue(pixelOffset(bandIdx, y, x)));
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    const float range = (mx > mn) ? (mx - mn) : 1.f;

    // 第二遍：65536-bin 直方图（栈分配，CPU cache 友好）
    constexpr int BINS = 65536;
    int hist[BINS] = {};
    for (int y = 0; y < L; ++y)
        for (int x = 0; x < S; ++x) {
            float v = static_cast<float>(readValue(pixelOffset(bandIdx, y, x)));
            hist[std::clamp((int)((v - mn) / range * (BINS - 1)), 0, BINS - 1)]++;
        }

    const int lo2 = static_cast<int>(N * 0.02f);
    const int hi2 = static_cast<int>(N * 0.98f);
    float low = mn, high = mx;
    int cum = 0;
    for (int i = 0; i < BINS; ++i) {
        cum += hist[i];
        if (cum >= lo2 && low  == mn) low  = mn + static_cast<float>(i) / BINS * range;
        if (cum >= hi2)             { high = mn + static_cast<float>(i) / BINS * range; break; }
    }
    if (high <= low) high = low + 1.f;
    return { low, high, 1.f / (high - low) };
}

// 灰度单波段渲染（复用 computeStretch，第三遍直接写 scanLine）
QImage EnviDataset::renderWithStretch(int bandIdx) const
{
    const StretchParams sp = computeStretch(bandIdx);
    QImage img(m_hdr.samples, m_hdr.lines, QImage::Format_Grayscale8);
    for (int y = 0; y < m_hdr.lines; ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < m_hdr.samples; ++x) {
            float v = static_cast<float>(readValue(pixelOffset(bandIdx, y, x)));
            row[x]  = static_cast<uchar>(std::clamp((v - sp.low) * sp.invSpan, 0.f, 1.f) * 255.f);
        }
    }
    return img;
}

// ─────────────────────────────────────────────────────────────────────────────
// RGB 真/假彩色合成
//   · QtConcurrent::run 并行计算 R/G/B 三通道拉伸参数
//   · 合成遍直接写 QRgb* scanLine，零中间数组
// ─────────────────────────────────────────────────────────────────────────────
QImage EnviDataset::getRGBComposite(int rBand, int gBand, int bBand) const
{
    if (!isValid()) return {};
    rBand = std::clamp(rBand, 0, m_hdr.bands - 1);
    gBand = std::clamp(gBand, 0, m_hdr.bands - 1);
    bBand = std::clamp(bBand, 0, m_hdr.bands - 1);

    // 三通道拉伸参数并行计算（DataWorker 线程内启动，不阻塞 GUI）
    auto futR = QtConcurrent::run([this, rBand]() { return computeStretch(rBand); });
    auto futG = QtConcurrent::run([this, gBand]() { return computeStretch(gBand); });
    auto futB = QtConcurrent::run([this, bBand]() { return computeStretch(bBand); });

    const StretchParams spR = futR.result();
    const StretchParams spG = futG.result();
    const StretchParams spB = futB.result();

    // 合成遍：直接写 QRgb* scanLine，无中间缓冲
    auto stretch = [](float v, const StretchParams& sp) -> uchar {
        return static_cast<uchar>(std::clamp((v - sp.low) * sp.invSpan, 0.f, 1.f) * 255.f);
    };

    QImage img(m_hdr.samples, m_hdr.lines, QImage::Format_RGB32);
    for (int y = 0; y < m_hdr.lines; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < m_hdr.samples; ++x) {
            uchar r = stretch((float)readValue(pixelOffset(rBand, y, x)), spR);
            uchar g = stretch((float)readValue(pixelOffset(gBand, y, x)), spG);
            uchar b = stretch((float)readValue(pixelOffset(bBand, y, x)), spB);
            row[x]  = qRgb(r, g, b);
        }
    }
    return img;  // QImage 隐式共享，跨线程 emit 安全
}

// RGB 合成（支持通道选择性显示）
QImage EnviDataset::getRGBComposite(int rBand, int gBand, int bBand,
                                    bool visR, bool visG, bool visB) const
{
    if (!isValid()) return {};
    rBand = std::clamp(rBand, 0, m_hdr.bands - 1);
    gBand = std::clamp(gBand, 0, m_hdr.bands - 1);
    bBand = std::clamp(bBand, 0, m_hdr.bands - 1);

    auto futR = visR ? QtConcurrent::run([this, rBand]() { return computeStretch(rBand); }) : QFuture<StretchParams>();
    auto futG = visG ? QtConcurrent::run([this, gBand]() { return computeStretch(gBand); }) : QFuture<StretchParams>();
    auto futB = visB ? QtConcurrent::run([this, bBand]() { return computeStretch(bBand); }) : QFuture<StretchParams>();

    const StretchParams spR = visR ? futR.result() : StretchParams{};
    const StretchParams spG = visG ? futG.result() : StretchParams{};
    const StretchParams spB = visB ? futB.result() : StretchParams{};

    auto stretch = [](float v, const StretchParams& sp) -> uchar {
        return static_cast<uchar>(std::clamp((v - sp.low) * sp.invSpan, 0.f, 1.f) * 255.f);
    };

    QImage img(m_hdr.samples, m_hdr.lines, QImage::Format_RGB32);
    for (int y = 0; y < m_hdr.lines; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < m_hdr.samples; ++x) {
            uchar r = visR ? stretch((float)readValue(pixelOffset(rBand, y, x)), spR) : 0;
            uchar g = visG ? stretch((float)readValue(pixelOffset(gBand, y, x)), spG) : 0;
            uchar b = visB ? stretch((float)readValue(pixelOffset(bBand, y, x)), spB) : 0;
            row[x]  = qRgb(r, g, b);
        }
    }
    return img;
}


// ─────────────────────────────────────────────────────────────────────────────
// 内置 mock 生成器：产生合法 BSQ float32 小文件用于测试
// ─────────────────────────────────────────────────────────────────────────────
QString EnviDataset::createMockEnviFile(const QString& tmpDir)
{
    const int S = 64, L = 64, B = 10;
    const QString baseName = tmpDir + "/mock_coresccan";
    const QString imgPath  = baseName + ".img";
    const QString hdrPath  = baseName + ".hdr";

    // 写二进制 .img（BSQ float32，小端）
    QFile img(imgPath);
    if (!img.open(QIODevice::WriteOnly)) return {};
    for (int b = 0; b < B; ++b)
        for (int y = 0; y < L; ++y)
            for (int x = 0; x < S; ++x) {
                float v = static_cast<float>(
                    0.3 + 0.5 * std::sin(b * 0.5) * std::cos(x * 0.1) * std::cos(y * 0.1)
                );
                img.write(reinterpret_cast<const char*>(&v), 4);
            }
    img.close();

    // 构造合法波长数组
    QStringList wls;
    for (int b = 0; b < B; ++b) wls << QString::number(400 + b * 200);

    // 写 .hdr
    QFile hdr(hdrPath);
    if (!hdr.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream out(&hdr);
    out << "ENVI\n"
        << "description = { 岩心光谱智能解译系统 mock dataset }\n"
        << "samples = "    << S << "\n"
        << "lines = "      << L << "\n"
        << "bands = "      << B << "\n"
        << "data type = 4\n"
        << "interleave = bsq\n"
        << "byte order = 0\n"
        << "header offset = 0\n"
        << "wavelength units = nanometers\n"
        << "wavelength = { " << wls.join(", ") << " }\n";
    hdr.close();

    return hdrPath;
}
