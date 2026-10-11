#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include "types.h"

// ---------------------------------------------------------------------------
// 页面渲染 / 词盒提取的**共享实现**（单页精看视图与连续滚动视图都调这里）
//
// 全部走内核融合层（内嵌优先，失败回退命令行），因此：
//   - 渲染结果与抽取结果永远同一来源，不会出现"两个视图算法不一致"；
//   - 调用被 KernelClient 内部的全局锁串行化（内嵌运行时是进程级单例），
//     所以多个视图/多个后台任务并发请求是安全的，只是排队执行。
// ---------------------------------------------------------------------------
struct RenderedPage
{
    bool   ok = false;
    QImage img;
    double scale = 1.0;        // image px per PDF pt（内核下发）
    double pwPt = 0.0, phPt = 0.0;
    int    meanLum = -1, maxLum = -1;   // 亮度诊断：判定"是否真的渲染出了一张黑图"
    QString error;
};

// 渲染一页。scale = 每 pt 渲染多少像素（1.0 = 该 PDF 的默认点尺寸）。
// 这**不是**界面缩放：界面缩放只影响显示，见各视图里的 zoom。
RenderedPage renderPageViaKernel(const QString& pdfPath, int page, double scale = 1.0);

// 读取某页的全部矢量词盒（用于框选时的实时文字识别）。
QVector<WordBox> loadWordsViaKernel(const QString& pdfPath, int page);
