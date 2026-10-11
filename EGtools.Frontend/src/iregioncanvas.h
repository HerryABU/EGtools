#pragma once

#include <QString>
#include <QVector>

#include "types.h"

// ---------------------------------------------------------------------------
// 画布能力的统一接口（框选 / 语义 / 缩放 / 叠加层 / 跳页）
//
// 为什么需要它：本 app 有两种画布形态 ——
//   PdfCanvas          —— 单页精看（一屏一页，GPU 或光栅壳承载）
//   ContinuousPdfView  —— Adobe 式连续滚动（一屏多页，懒渲染）
// 二者都实现这个接口，MainWindow 只面向接口操作，于是"切换视图"不需要改任何
// 业务代码（框选、提取、导出 JSON、缩放菜单全都照旧工作）。
//
// 注意：接口只放**两种视图都能成立**的操作。单页专有的（换图、取当前页图等）
// 不进接口，由 MainWindow 直接对具体类型调用。
// ---------------------------------------------------------------------------
class IRegionCanvas
{
public:
    virtual ~IRegionCanvas() = default;

    // ---- 框选与语义 ----
    virtual void addRegion(const Region& r)  = 0;
    virtual void clearRegions()              = 0;
    virtual QVector<Region> regions() const  = 0;
    virtual void setKind(const QString& kind) = 0;
    virtual QString kind() const             = 0;

    // ---- 缩放（widgetPx = pt * zoom）----
    virtual void setZoom(double z)           = 0;
    virtual double zoom() const              = 0;
    virtual void zoomIn()                    = 0;
    virtual void zoomOut()                   = 0;
    virtual void fitToWidth(int viewportWidthPx) = 0;

    // ---- 叠加层 ----
    virtual void setWordBoxes(const QVector<WordBox>& words) = 0;
    virtual void clearWordBoxes()            = 0;

    // ---- 跳页：单页视图=切换显示页；连续视图=滚动到该页 ----
    virtual void showPage(int page)          = 0;
};
