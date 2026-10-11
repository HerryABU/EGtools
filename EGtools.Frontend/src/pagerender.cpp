#include "pagerender.h"

#include "kernelclient.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

RenderedPage renderPageViaKernel(const QString& pdfPath, int page, double scale)
{
    RenderedPage r;
    if (pdfPath.isEmpty() || page < 0) { r.error = QStringLiteral("无效的 PDF 或页码"); return r; }
    if (scale <= 0) scale = 1.0;

    QTemporaryDir tmp;
    if (!tmp.isValid()) { r.error = QStringLiteral("无法创建临时目录"); return r; }
    const QString bmp = QDir(tmp.path()).filePath(QStringLiteral("page.bmp"));

    const KernelClient::Result kr = KernelClient::run(
        QStringLiteral("EGpdf2excel"),
        {QStringLiteral("--render"), QStringLiteral("--page"), QString::number(page),
         QStringLiteral("--scale"), QString::number(scale, 'g', 6),
         QStringLiteral("--render-out"), bmp, pdfPath},
        QString(), KernelClient::defaultBackend());
    if (kr.rc != 0) {
        r.error = kr.err.trimmed().isEmpty()
                      ? QStringLiteral("内核渲染失败 rc=%1").arg(kr.rc)
                      : kr.err.trimmed();
        return r;
    }

    QImage img = QImageReader(bmp).read();
    if (img.isNull()) {
        r.error = QStringLiteral("读取渲染图失败（文件 %1 字节）").arg(QFileInfo(bmp).size());
        return r;
    }
    // 强制深拷贝：QImageReader 可能延迟解码，而临时目录在本函数返回时即被删除。
    r.img = img.copy();
    r.ok = true;

    const QJsonObject o = QJsonDocument::fromJson(kr.out.toUtf8()).object();
    r.pwPt = o.value(QStringLiteral("pageWidthPt")).toDouble();
    r.phPt = o.value(QStringLiteral("pageHeightPt")).toDouble();
    r.scale = o.value(QStringLiteral("scale")).toDouble(scale);
    if (r.scale <= 0) r.scale = scale;
    if (r.pwPt <= 0) r.pwPt = r.img.width() / r.scale;
    if (r.phPt <= 0) r.phPt = r.img.height() / r.scale;

    // 亮度诊断：把"黑屏"变成可判定的事实 —— 整页近乎全黑 = 内核渲染出的图本身是黑的
    // （PDF 内容/背景问题）；亮度正常却看不到图 = 显示侧的问题。
    {
        const QImage probe = r.img.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        quint64 lum = 0; int maxL = 0;
        for (int y = 0; y < probe.height(); ++y)
            for (int x = 0; x < probe.width(); ++x) {
                const QRgb c = probe.pixel(x, y);
                const int l = (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
                lum += static_cast<quint64>(l);
                maxL = qMax(maxL, l);
            }
        r.meanLum = static_cast<int>(lum / static_cast<quint64>(probe.width() * probe.height()));
        r.maxLum  = maxL;
    }
    return r;
}

QVector<WordBox> loadWordsViaKernel(const QString& pdfPath, int page)
{
    QVector<WordBox> words;
    if (pdfPath.isEmpty() || page < 0) return words;

    const KernelClient::Result kr = KernelClient::run(
        QStringLiteral("EGpdf2excel"),
        {QStringLiteral("--words"), QString::number(page), pdfPath},
        QString(), KernelClient::defaultBackend());
    if (kr.rc != 0) return words;

    const QJsonDocument doc = QJsonDocument::fromJson(kr.out.toUtf8());
    if (!doc.isObject()) return words;
    for (const QJsonValue& wv : doc.object().value(QStringLiteral("words")).toArray()) {
        const QJsonObject w = wv.toObject();
        WordBox wb;
        const double x0 = w.value(QStringLiteral("x0")).toDouble();
        const double y0 = w.value(QStringLiteral("y0")).toDouble();
        const double x1 = w.value(QStringLiteral("x1")).toDouble();
        const double y1 = w.value(QStringLiteral("y1")).toDouble();
        wb.rectPt = QRectF(x0, y0, x1 - x0, y1 - y0);
        wb.text   = w.value(QStringLiteral("text")).toString();
        wb.page   = page;
        words.append(wb);
    }
    return words;
}
