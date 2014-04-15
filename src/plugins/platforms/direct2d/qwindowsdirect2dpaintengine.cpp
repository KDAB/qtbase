/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qwindowsdirect2dpaintengine.h"
#include "qwindowsdirect2dplatformpixmap.h"
#include "qwindowsdirect2dpaintdevice.h"
#include "qwindowsdirect2dcontext.h"
#include "qwindowsdirect2dhelpers.h"
#include "qwindowsdirect2dbitmap.h"
#include "qwindowsdirect2ddevicecontext.h"

#include "qwindowsfontengine.h"
#include "qwindowsfontenginedirectwrite.h"
#include "qwindowsfontdatabase.h"
#include "qwindowsintegration.h"

#include <QtGui/private/qpaintengine_p.h>
#include <QtGui/private/qtextengine_p.h>
#include <QtGui/private/qfontengine_p.h>
#include <QtGui/private/qstatictext_p.h>

#include <wrl.h>
using Microsoft::WRL::ComPtr;

QT_BEGIN_NAMESPACE

// The enum values below are set as tags on the device context
// in the various draw methods. When EndDraw is called the device context
// will report the last set tag number in case of errors
// along with an error code

// Microsoft keeps a list of d2d error codes here:
// http://msdn.microsoft.com/en-us/library/windows/desktop/dd370979(v=vs.85).aspx
enum {
    D2DDebugDrawInitialStateTag = -1,
    D2DDebugDrawImageTag = 1,
    D2DDebugFillTag,
    D2DDebugDrawPixmapTag,
    D2DDebugDrawStaticTextItemTag,
    D2DDebugDrawTextItemTag
};

//Clipping flags
enum {
    UserClip = 0x1,
    SimpleSystemClip = 0x2
};
#define D2D_TAG(tag) d->dc()->SetTags(tag, tag)

Q_GUI_EXPORT QImage qt_imageForBrush(int brushStyle, bool invert);

static inline ID2D1Factory1 *factory()
{
    return QWindowsDirect2DContext::instance()->d2dFactory();
}

class Direct2DPathGeometryWriter
{
public:
    Direct2DPathGeometryWriter()
        : m_inFigure(false)
        , m_roundCoordinates(false)
    {

    }

    bool begin()
    {
        HRESULT hr = factory()->CreatePathGeometry(&m_geometry);
        if (FAILED(hr)) {
            qWarning("%s: Could not create path geometry: %#x", __FUNCTION__, hr);
            return false;
        }

        hr = m_geometry->Open(&m_sink);
        if (FAILED(hr)) {
            qWarning("%s: Could not create geometry sink: %#x", __FUNCTION__, hr);
            return false;
        }

        return true;
    }

    void setWindingFillEnabled(bool enable)
    {
        if (enable)
            m_sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        else
            m_sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
    }

    void setAliasingEnabled(bool enable)
    {
        m_roundCoordinates = enable;
    }

    bool isInFigure() const
    {
        return m_inFigure;
    }

    void moveTo(const QPointF &point)
    {
        if (m_inFigure)
            m_sink->EndFigure(D2D1_FIGURE_END_OPEN);

        m_sink->BeginFigure(adjusted(point), D2D1_FIGURE_BEGIN_FILLED);
        m_inFigure = true;
    }

    void lineTo(const QPointF &point)
    {
        m_sink->AddLine(adjusted(point));
    }

    void curveTo(const QPointF &p1, const QPointF &p2, const QPointF &p3)
    {
        D2D1_BEZIER_SEGMENT segment = {
            adjusted(p1),
            adjusted(p2),
            adjusted(p3)
        };

        m_sink->AddBezier(segment);
    }

    void close()
    {
        if (m_inFigure)
            m_sink->EndFigure(D2D1_FIGURE_END_OPEN);

        m_sink->Close();
    }

    ComPtr<ID2D1PathGeometry1> geometry() const
    {
        return m_geometry;
    }

private:
    D2D1_POINT_2F adjusted(const QPointF &point)
    {
        if (m_roundCoordinates)
            return to_d2d_point_2f(point.toPoint());
        else
            return to_d2d_point_2f(point);
    }

    ComPtr<ID2D1PathGeometry1> m_geometry;
    ComPtr<ID2D1GeometrySink> m_sink;

    bool m_inFigure;
    bool m_roundCoordinates;
};

static ComPtr<ID2D1PathGeometry1> painterPathToID2D1PathGeometry(const QPainterPath &path, bool alias)
{
    Direct2DPathGeometryWriter writer;
    if (!writer.begin())
        return NULL;

    writer.setWindingFillEnabled(path.fillRule() == Qt::WindingFill);
    writer.setAliasingEnabled(alias);

    for (int i = 0; i < path.elementCount(); i++) {
        const QPainterPath::Element element = path.elementAt(i);

        switch (element.type) {
        case QPainterPath::MoveToElement:
            writer.moveTo(element);
            break;

        case QPainterPath::LineToElement:
            writer.lineTo(element);
            break;

        case QPainterPath::CurveToElement:
        {
            const QPainterPath::Element data1 = path.elementAt(++i);
            const QPainterPath::Element data2 = path.elementAt(++i);

            Q_ASSERT(i < path.elementCount());

            Q_ASSERT(data1.type == QPainterPath::CurveToDataElement);
            Q_ASSERT(data2.type == QPainterPath::CurveToDataElement);

            writer.curveTo(element, data1, data2);
        }
            break;

        case QPainterPath::CurveToDataElement:
            qWarning("%s: Unhandled Curve Data Element", __FUNCTION__);
            break;
        }
    }

    writer.close();
    return writer.geometry();
}

static ComPtr<ID2D1PathGeometry1> vectorPathToID2D1PathGeometry(const QVectorPath &path, bool alias)
{
    Direct2DPathGeometryWriter writer;
    if (!writer.begin())
        return NULL;

    writer.setWindingFillEnabled(path.hasWindingFill());
    writer.setAliasingEnabled(alias);

    const QPainterPath::ElementType *types = path.elements();
    const int count = path.elementCount();
    const qreal *points = path.points();

    Q_ASSERT(points);

    if (types) {
        qreal x, y;

        for (int i = 0; i < count; i++) {
            x = points[i * 2];
            y = points[i * 2 + 1];

            switch (types[i]) {
            case QPainterPath::MoveToElement:
                writer.moveTo(QPointF(x, y));
                break;

            case QPainterPath::LineToElement:
                writer.lineTo(QPointF(x, y));
                break;

            case QPainterPath::CurveToElement:
            {
                Q_ASSERT((i + 2) < count);
                Q_ASSERT(types[i+1] == QPainterPath::CurveToDataElement);
                Q_ASSERT(types[i+2] == QPainterPath::CurveToDataElement);

                i++;
                const qreal x2 = points[i * 2];
                const qreal y2 = points[i * 2 + 1];

                i++;
                const qreal x3 = points[i * 2];
                const qreal y3 = points[i * 2 + 1];

                writer.curveTo(QPointF(x, y), QPointF(x2, y2), QPointF(x3, y3));
            }
                break;

            case QPainterPath::CurveToDataElement:
                qWarning("%s: Unhandled Curve Data Element", __FUNCTION__);
                break;
            }
        }
    } else {
        writer.moveTo(QPointF(points[0], points[1]));
        for (int i = 1; i < count; i++)
            writer.lineTo(QPointF(points[i * 2], points[i * 2 + 1]));
    }

    if (writer.isInFigure())
        if (path.hasImplicitClose())
            writer.lineTo(QPointF(points[0], points[1]));

    writer.close();
    return writer.geometry();
}

class QWindowsDirect2DPaintEnginePrivate : public QPaintEngineExPrivate
{
    Q_DECLARE_PUBLIC(QWindowsDirect2DPaintEngine)
public:
    QWindowsDirect2DPaintEnginePrivate(QWindowsDirect2DBitmap *bm)
        : bitmap(bm)
        , clipFlags(0)
    {
        pen.reset();
        brush.reset();

        dc()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    }

    QWindowsDirect2DBitmap *bitmap;

    QPainterPath clipPath;
    unsigned int clipFlags;

    QPointF currentBrushOrigin;

    struct {
        bool emulate;
        QPen qpen;
        ComPtr<ID2D1Brush> brush;
        ComPtr<ID2D1StrokeStyle1> strokeStyle;

        inline void reset() {
            emulate = false;
            qpen = QPen();
            brush.Reset();
            strokeStyle.Reset();
        }
    } pen;

    struct {
        bool emulate;
        QBrush qbrush;
        ComPtr<ID2D1Brush> brush;

        inline void reset() {
            emulate = false;
            brush.Reset();
            qbrush = QBrush();
        }
    } brush;

    inline ID2D1DeviceContext *dc() const
    {
        Q_ASSERT(bitmap);
        return bitmap->deviceContext()->get();
    }

    inline D2D1_INTERPOLATION_MODE interpolationMode() const
    {
        Q_Q(const QWindowsDirect2DPaintEngine);
        // XXX are we choosing the right d2d interpolation modes?
        return (q->state()->renderHints & QPainter::SmoothPixmapTransform) ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                                                           : D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    }

    inline D2D1_ANTIALIAS_MODE antialiasMode() const
    {
        Q_Q(const QWindowsDirect2DPaintEngine);
        return (q->state()->renderHints & QPainter::Antialiasing) ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                                                                  : D2D1_ANTIALIAS_MODE_ALIASED;
    }

    void updateTransform()
    {
        Q_Q(const QWindowsDirect2DPaintEngine);
        // Note the loss of info going from 3x3 to 3x2 matrix here
        dc()->SetTransform(to_d2d_matrix_3x2_f(q->state()->transform()));
    }

    void updateOpacity()
    {
        Q_Q(const QWindowsDirect2DPaintEngine);
        qreal opacity = q->state()->opacity;
        if (brush.brush)
            brush.brush->SetOpacity(opacity);
        if (pen.brush)
            pen.brush->SetOpacity(opacity);
    }

    void pushClip()
    {
        popClip();

        ComPtr<ID2D1PathGeometry1> geometry = painterPathToID2D1PathGeometry(clipPath, antialiasMode() == D2D1_ANTIALIAS_MODE_ALIASED);
        if (!geometry)
            return;

        dc()->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(),
                                               geometry.Get(),
                                               antialiasMode(),
                                               D2D1::IdentityMatrix(),
                                               1.0,
                                               NULL,
                                               D2D1_LAYER_OPTIONS1_INITIALIZE_FROM_BACKGROUND),
                        NULL);
        clipFlags |= UserClip;
    }

    void popClip()
    {
        if (clipFlags & UserClip) {
            dc()->PopLayer();
            clipFlags &= ~UserClip;
        }
    }

    void updateClipEnabled()
    {
        Q_Q(const QWindowsDirect2DPaintEngine);
        if (!q->state()->clipEnabled)
            popClip();
        else if (!(clipFlags & UserClip))
            pushClip();
    }

    void updateClipPath(const QPainterPath &path, Qt::ClipOperation operation)
    {
        switch (operation) {
        case Qt::NoClip:
            popClip();
            break;
        case Qt::ReplaceClip:
            clipPath = path;
            pushClip();
            break;
        case Qt::IntersectClip:
            clipPath &= path;
            pushClip();
            break;
        }
    }

    void updateCompositionMode()
    {
        Q_Q(const QWindowsDirect2DPaintEngine);
        QPainter::CompositionMode mode = q->state()->compositionMode();

        switch (mode) {
        case QPainter::CompositionMode_Source:
            dc()->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
            break;
        case QPainter::CompositionMode_SourceOver:
            dc()->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            break;

        default:
            qWarning("Unsupported composition mode: %d", mode);
            break;
        }
    }

    void updateBrush(const QBrush &newBrush)
    {
        Q_Q(const QWindowsDirect2DPaintEngine);

        if (qbrush_fast_equals(brush.qbrush, newBrush))
            return;

        brush.brush = to_d2d_brush(newBrush, &brush.emulate);
        brush.qbrush = newBrush;

        if (brush.brush) {
            brush.brush->SetOpacity(q->state()->opacity);
            applyBrushOrigin(currentBrushOrigin);
        }
    }

    void updateBrushOrigin()
    {
        Q_Q(const QWindowsDirect2DPaintEngine);

        negateCurrentBrushOrigin();
        applyBrushOrigin(q->state()->brushOrigin);
    }

    void negateCurrentBrushOrigin()
    {
        if (brush.brush && !currentBrushOrigin.isNull()) {
            D2D1_MATRIX_3X2_F transform;
            brush.brush->GetTransform(&transform);

            brush.brush->SetTransform(*(D2D1::Matrix3x2F::ReinterpretBaseType(&transform))
                                      * D2D1::Matrix3x2F::Translation(-currentBrushOrigin.x(),
                                                                      -currentBrushOrigin.y()));
        }
    }

    void applyBrushOrigin(const QPointF &origin)
    {
        if (brush.brush && !origin.isNull()) {
            D2D1_MATRIX_3X2_F transform;
            brush.brush->GetTransform(&transform);

            brush.brush->SetTransform(*(D2D1::Matrix3x2F::ReinterpretBaseType(&transform))
                                      * D2D1::Matrix3x2F::Translation(origin.x(), origin.y()));
        }

        currentBrushOrigin = origin;
    }

    void updatePen()
    {
        Q_Q(const QWindowsDirect2DPaintEngine);
        const QPen &newPen = q->state()->pen;

        if (qpen_fast_equals(newPen, pen.qpen))
            return;

        pen.reset();
        pen.qpen = newPen;

        if (newPen.style() == Qt::NoPen)
            return;

        pen.brush = to_d2d_brush(newPen.brush(), &pen.emulate);
        if (!pen.brush)
            return;

        pen.brush->SetOpacity(q->state()->opacity);

        D2D1_STROKE_STYLE_PROPERTIES1 props = {};

        switch (newPen.capStyle()) {
        case Qt::SquareCap:
            props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_SQUARE;
            break;
        case Qt::RoundCap:
            props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_ROUND;
        case Qt::FlatCap:
        default:
            props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_FLAT;
            break;
        }

        switch (newPen.joinStyle()) {
        case Qt::BevelJoin:
            props.lineJoin = D2D1_LINE_JOIN_BEVEL;
            break;
        case Qt::RoundJoin:
            props.lineJoin = D2D1_LINE_JOIN_ROUND;
            break;
        case Qt::MiterJoin:
        default:
            props.lineJoin = D2D1_LINE_JOIN_MITER;
            break;
        }

        props.miterLimit = newPen.miterLimit() * qreal(2.0); // D2D and Qt miter specs differ
        props.dashOffset = newPen.dashOffset();
        props.transformType = qIsNull(newPen.widthF()) ? D2D1_STROKE_TRANSFORM_TYPE_HAIRLINE
                                                       : newPen.isCosmetic() ? D2D1_STROKE_TRANSFORM_TYPE_FIXED
                                                                             : D2D1_STROKE_TRANSFORM_TYPE_NORMAL;

        switch (newPen.style()) {
        case Qt::SolidLine:
            props.dashStyle = D2D1_DASH_STYLE_SOLID;
            break;

        case Qt::DotLine:
        case Qt::DashDotLine:
        case Qt::DashDotDotLine:
            // Try and match Qt's raster engine in output as closely as possible
            if (newPen.widthF() <= 1.0)
                props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_FLAT;

            // fall through
        default:
            props.dashStyle = D2D1_DASH_STYLE_CUSTOM;
            break;
        }

        HRESULT hr;

        if (props.dashStyle == D2D1_DASH_STYLE_CUSTOM) {
            QVector<qreal> dashes = newPen.dashPattern();
            QVector<FLOAT> converted(dashes.size());

            for (int i = 0; i < dashes.size(); i++) {
                converted[i] = dashes[i];
            }

            hr = factory()->CreateStrokeStyle(props, converted.constData(), converted.size(), &pen.strokeStyle);
        } else {
            hr = factory()->CreateStrokeStyle(props, NULL, 0, &pen.strokeStyle);
        }

        if (FAILED(hr))
            qWarning("%s: Could not create stroke style: %#x", __FUNCTION__, hr);
    }

    ComPtr<ID2D1Brush> to_d2d_brush(const QBrush &newBrush, bool *needsEmulation)
    {
        HRESULT hr;
        ComPtr<ID2D1Brush> result;

        Q_ASSERT(needsEmulation);

        *needsEmulation = false;

        switch (newBrush.style()) {
        case Qt::NoBrush:
            break;

        case Qt::SolidPattern:
        {
            ComPtr<ID2D1SolidColorBrush> solid;

            hr = dc()->CreateSolidColorBrush(to_d2d_color_f(newBrush.color()), &solid);
            if (FAILED(hr)) {
                qWarning("%s: Could not create solid color brush: %#x", __FUNCTION__, hr);
                break;
            }

            hr = solid.As(&result);
            if (FAILED(hr))
                qWarning("%s: Could not convert solid color brush: %#x", __FUNCTION__, hr);
        }
            break;

        case Qt::Dense1Pattern:
        case Qt::Dense2Pattern:
        case Qt::Dense3Pattern:
        case Qt::Dense4Pattern:
        case Qt::Dense5Pattern:
        case Qt::Dense6Pattern:
        case Qt::Dense7Pattern:
        case Qt::HorPattern:
        case Qt::VerPattern:
        case Qt::CrossPattern:
        case Qt::BDiagPattern:
        case Qt::FDiagPattern:
        case Qt::DiagCrossPattern:
        {
            ComPtr<ID2D1BitmapBrush1> bitmapBrush;
            D2D1_BITMAP_BRUSH_PROPERTIES1 bitmapBrushProperties = {
                D2D1_EXTEND_MODE_WRAP,
                D2D1_EXTEND_MODE_WRAP,
                interpolationMode()
            };

            QImage brushImg = qt_imageForBrush(newBrush.style(), false);
            brushImg.setColor(0, newBrush.color().rgba());
            brushImg.setColor(1, qRgba(0, 0, 0, 0));

            QWindowsDirect2DBitmap bitmap;
            bool success = bitmap.fromImage(brushImg, Qt::AutoColor);
            if (!success) {
                qWarning("%s: Could not create Direct2D bitmap from Qt pattern brush image", __FUNCTION__);
                break;
            }

            hr = dc()->CreateBitmapBrush(bitmap.bitmap(),
                                         bitmapBrushProperties,
                                         &bitmapBrush);
            if (FAILED(hr)) {
                qWarning("%s: Could not create Direct2D bitmap brush for Qt pattern brush: %#x", __FUNCTION__, hr);
                break;
            }

            hr = bitmapBrush.As(&result);
            if (FAILED(hr))
                qWarning("%s: Could not convert Direct2D bitmap brush for Qt pattern brush: %#x", __FUNCTION__, hr);
        }
            break;

        case Qt::LinearGradientPattern:
            if (newBrush.gradient()->spread() != QGradient::PadSpread) {
                *needsEmulation = true;
            } else {
                ComPtr<ID2D1LinearGradientBrush> linear;
                const QLinearGradient *qlinear = static_cast<const QLinearGradient *>(newBrush.gradient());

                D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES linearGradientBrushProperties;
                ComPtr<ID2D1GradientStopCollection> gradientStopCollection;

                const QGradientStops &qstops = qlinear->stops();
                QVector<D2D1_GRADIENT_STOP> stops(qstops.count());

                linearGradientBrushProperties.startPoint = to_d2d_point_2f(qlinear->start());
                linearGradientBrushProperties.endPoint = to_d2d_point_2f(qlinear->finalStop());

                for (int i = 0; i < stops.size(); i++) {
                    stops[i].position = qstops[i].first;
                    stops[i].color = to_d2d_color_f(qstops[i].second);
                }

                hr = dc()->CreateGradientStopCollection(stops.constData(), stops.size(), &gradientStopCollection);
                if (FAILED(hr)) {
                    qWarning("%s: Could not create gradient stop collection for linear gradient: %#x", __FUNCTION__, hr);
                    break;
                }

                hr = dc()->CreateLinearGradientBrush(linearGradientBrushProperties, gradientStopCollection.Get(),
                                                     &linear);
                if (FAILED(hr)) {
                    qWarning("%s: Could not create Direct2D linear gradient brush: %#x", __FUNCTION__, hr);
                    break;
                }

                hr = linear.As(&result);
                if (FAILED(hr)) {
                    qWarning("%s: Could not convert Direct2D linear gradient brush: %#x", __FUNCTION__, hr);
                    break;
                }
            }
            break;

        case Qt::RadialGradientPattern:
            if (newBrush.gradient()->spread() != QGradient::PadSpread) {
                *needsEmulation = true;
            } else {
                ComPtr<ID2D1RadialGradientBrush> radial;
                const QRadialGradient *qradial = static_cast<const QRadialGradient *>(newBrush.gradient());

                D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES radialGradientBrushProperties;
                ComPtr<ID2D1GradientStopCollection> gradientStopCollection;

                const QGradientStops &qstops = qradial->stops();
                QVector<D2D1_GRADIENT_STOP> stops(qstops.count());

                radialGradientBrushProperties.center = to_d2d_point_2f(qradial->center());
                radialGradientBrushProperties.gradientOriginOffset = to_d2d_point_2f(qradial->focalPoint() - qradial->center());
                radialGradientBrushProperties.radiusX = qradial->radius();
                radialGradientBrushProperties.radiusY = qradial->radius();

                for (int i = 0; i < stops.size(); i++) {
                    stops[i].position = qstops[i].first;
                    stops[i].color = to_d2d_color_f(qstops[i].second);
                }

                hr = dc()->CreateGradientStopCollection(stops.constData(), stops.size(), &gradientStopCollection);
                if (FAILED(hr)) {
                    qWarning("%s: Could not create gradient stop collection for radial gradient: %#x", __FUNCTION__, hr);
                    break;
                }

                hr = dc()->CreateRadialGradientBrush(radialGradientBrushProperties, gradientStopCollection.Get(),
                                                     &radial);
                if (FAILED(hr)) {
                    qWarning("%s: Could not create Direct2D radial gradient brush: %#x", __FUNCTION__, hr);
                    break;
                }

                radial.As(&result);
                if (FAILED(hr)) {
                    qWarning("%s: Could not convert Direct2D radial gradient brush: %#x", __FUNCTION__, hr);
                    break;
                }
            }
            break;

        case Qt::ConicalGradientPattern:
            *needsEmulation = true;
            break;

        case Qt::TexturePattern:
        {
            ComPtr<ID2D1BitmapBrush1> bitmapBrush;
            D2D1_BITMAP_BRUSH_PROPERTIES1 bitmapBrushProperties = {
                D2D1_EXTEND_MODE_WRAP,
                D2D1_EXTEND_MODE_WRAP,
                interpolationMode()
            };

            QWindowsDirect2DPlatformPixmap *pp = static_cast<QWindowsDirect2DPlatformPixmap *>(newBrush.texture().handle());
            QWindowsDirect2DBitmap *bitmap = pp->bitmap();
            hr = dc()->CreateBitmapBrush(bitmap->bitmap(),
                                         bitmapBrushProperties,
                                         &bitmapBrush);

            if (FAILED(hr)) {
                qWarning("%s: Could not create texture brush: %#x", __FUNCTION__, hr);
                break;
            }

            hr = bitmapBrush.As(&result);
            if (FAILED(hr))
                qWarning("%s: Could not convert texture brush: %#x", __FUNCTION__, hr);
        }
            break;
        }

        if (result && !newBrush.transform().isIdentity())
            result->SetTransform(to_d2d_matrix_3x2_f(newBrush.transform()));

        return result;
    }

    void updateHints()
    {
        dc()->SetAntialiasMode(antialiasMode());
    }
};

QWindowsDirect2DPaintEngine::QWindowsDirect2DPaintEngine(QWindowsDirect2DBitmap *bitmap)
    : QPaintEngineEx(*(new QWindowsDirect2DPaintEnginePrivate(bitmap)))
{
    QPaintEngine::PaintEngineFeatures unsupported =
            // As of 1.1 Direct2D does not natively support complex composition modes
            // However, using Direct2D effects that implement them should be possible
            QPaintEngine::PorterDuff
            | QPaintEngine::BlendModes
            | QPaintEngine::RasterOpModes

            // As of 1.1 Direct2D does not natively support perspective transforms
            // However, writing a custom effect that implements them should be possible
            // The built-in 3D transform effect unfortunately changes output image size, making
            // it unusable for us.
            | QPaintEngine::PerspectiveTransform;

    gccaps &= ~unsupported;
}

bool QWindowsDirect2DPaintEngine::begin(QPaintDevice * pdev)
{
    Q_D(QWindowsDirect2DPaintEngine);

    d->bitmap->deviceContext()->begin();
    d->dc()->SetTransform(D2D1::Matrix3x2F::Identity());

    if (systemClip().rectCount() > 1) {
        QPainterPath p;
        p.addRegion(systemClip());

        ComPtr<ID2D1PathGeometry1> geometry = painterPathToID2D1PathGeometry(p, d->antialiasMode() == D2D1_ANTIALIAS_MODE_ALIASED);
        if (!geometry)
            return false;

        d->dc()->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(),
                                               geometry.Get(),
                                               d->antialiasMode(),
                                               D2D1::IdentityMatrix(),
                                               1.0,
                                               NULL,
                                               D2D1_LAYER_OPTIONS1_INITIALIZE_FROM_BACKGROUND),
                        NULL);
    } else {
        QRect clip(0, 0, pdev->width(), pdev->height());
        if (!systemClip().isEmpty())
            clip &= systemClip().boundingRect();
        d->dc()->PushAxisAlignedClip(to_d2d_rect_f(clip), D2D1_ANTIALIAS_MODE_ALIASED);
        d->clipFlags |= SimpleSystemClip;
    }

    D2D_TAG(D2DDebugDrawInitialStateTag);

    return true;
}

bool QWindowsDirect2DPaintEngine::end()
{
    Q_D(QWindowsDirect2DPaintEngine);
    // First pop any user-applied clipping
    d->popClip();
    // Now the system clip from begin() above
    if (d->clipFlags & SimpleSystemClip) {
        d->dc()->PopAxisAlignedClip();
        d->clipFlags &= ~SimpleSystemClip;
    } else {
        d->dc()->PopLayer();
    }
    return d->bitmap->deviceContext()->end();
}

QPaintEngine::Type QWindowsDirect2DPaintEngine::type() const
{
    return QPaintEngine::Direct2D;
}

void QWindowsDirect2DPaintEngine::fill(const QVectorPath &path, const QBrush &brush)
{
    Q_D(QWindowsDirect2DPaintEngine);
    D2D_TAG(D2DDebugFillTag);

    if (path.isEmpty())
        return;

    d->updateBrush(brush);

    if (d->brush.emulate) {
        // We mostly (only?) get here when gradients are required.
        // We natively support only linear and radial gradients that have pad reflect due to D2D limitations

        QImage img(d->bitmap->size(), QImage::Format_ARGB32);
        img.fill(Qt::transparent);

        QPainter p;
        QPaintEngine *engine = img.paintEngine();
        if (engine->isExtended() && p.begin(&img)) {
            QPaintEngineEx *extended = static_cast<QPaintEngineEx *>(engine);
            extended->fill(path, brush);
            if (!p.end())
                qWarning("%s: Paint Engine end returned false", __FUNCTION__);

            drawImage(img.rect(), img, img.rect());
        } else {
            qWarning("%s: Could not fall back to QImage", __FUNCTION__);
        }

        return;
    }

    if (!d->brush.brush)
        return;

    if (path.hints() & QVectorPath::RectangleShapeMask) {
        const qreal * const points = path.points();
        D2D_RECT_F rect = {
            points[0], // left
            points[1], // top
            points[2], // right,
            points[5]  // bottom
        };

        d->dc()->FillRectangle(rect, d->brush.brush.Get());
    } else {
        ComPtr<ID2D1Geometry> geometry = vectorPathToID2D1PathGeometry(path, d->antialiasMode() == D2D1_ANTIALIAS_MODE_ALIASED);
        if (!geometry) {
            qWarning("%s: Could not convert path to d2d geometry", __FUNCTION__);
            return;
        }

        d->dc()->FillGeometry(geometry.Get(), d->brush.brush.Get());
    }
}

// For clipping we convert everything to painter paths since it allows
// calculating intersections easily. It might be faster to convert to
// ID2D1Geometry and use its operations, although that needs to measured.
// The implementation would be more complex in any case.

void QWindowsDirect2DPaintEngine::clip(const QVectorPath &path, Qt::ClipOperation op)
{
    clip(path.convertToPainterPath(), op);
}

void QWindowsDirect2DPaintEngine::clip(const QRect &rect, Qt::ClipOperation op)
{
    QPainterPath p;
    p.addRect(rect);
    clip(p, op);
}

void QWindowsDirect2DPaintEngine::clip(const QRegion &region, Qt::ClipOperation op)
{
    QPainterPath p;
    p.addRegion(region);
    clip(p, op);
}

void QWindowsDirect2DPaintEngine::clip(const QPainterPath &path, Qt::ClipOperation op)
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateClipPath(path, op);
}

void QWindowsDirect2DPaintEngine::clipEnabledChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateClipEnabled();
}

void QWindowsDirect2DPaintEngine::penChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updatePen();
}

void QWindowsDirect2DPaintEngine::brushChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateBrush(state()->brush);
}

void QWindowsDirect2DPaintEngine::brushOriginChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateBrushOrigin();
}

void QWindowsDirect2DPaintEngine::opacityChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateOpacity();
}

void QWindowsDirect2DPaintEngine::compositionModeChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateCompositionMode();
}

void QWindowsDirect2DPaintEngine::renderHintsChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateHints();
}

void QWindowsDirect2DPaintEngine::transformChanged()
{
    Q_D(QWindowsDirect2DPaintEngine);
    d->updateTransform();
}

void QWindowsDirect2DPaintEngine::drawImage(const QRectF &rectangle, const QImage &image,
                                            const QRectF &sr, Qt::ImageConversionFlags flags)
{
    Q_D(QWindowsDirect2DPaintEngine);
    D2D_TAG(D2DDebugDrawImageTag);

    QPixmap pixmap = QPixmap::fromImage(image, flags);
    drawPixmap(rectangle, pixmap, sr);
}

void QWindowsDirect2DPaintEngine::drawPixmap(const QRectF &r,
                                             const QPixmap &pm,
                                             const QRectF &sr)
{
    Q_D(QWindowsDirect2DPaintEngine);
    D2D_TAG(D2DDebugDrawPixmapTag);

    if (pm.isNull())
        return;

    if (pm.handle()->pixelType() == QPlatformPixmap::BitmapType) {
        QImage i = pm.toImage();
        i.setColor(0, qRgba(0, 0, 0, 0));
        i.setColor(1, d->pen.qpen.color().rgba());
        drawImage(r, i, sr);
        return;
    }

    QWindowsDirect2DPlatformPixmap *pp = static_cast<QWindowsDirect2DPlatformPixmap *>(pm.handle());
    QWindowsDirect2DBitmap *bitmap = pp->bitmap();

    if (bitmap->bitmap() != d->bitmap->bitmap()) {
        // Good, src bitmap != dst bitmap
        if (sr.isValid())
            d->dc()->DrawBitmap(bitmap->bitmap(),
                                to_d2d_rect_f(r), state()->opacity,
                                d->interpolationMode(),
                                to_d2d_rect_f(sr));
        else
            d->dc()->DrawBitmap(bitmap->bitmap(),
                                to_d2d_rect_f(r), state()->opacity,
                                d->interpolationMode());
    } else {
        // Ok, so the source pixmap and destination pixmap is the same.
        // D2D is not fond of this scenario, deal with it through
        // an intermediate bitmap
        QWindowsDirect2DBitmap intermediate;

        if (sr.isValid()) {
            bool r = intermediate.resize(sr.width(), sr.height());
            if (!r) {
                qWarning("%s: Could not resize intermediate bitmap to source rect size", __FUNCTION__);
                return;
            }

            D2D1_RECT_U d2d_sr =  to_d2d_rect_u(sr.toRect());
            HRESULT hr = intermediate.bitmap()->CopyFromBitmap(NULL,
                                                               bitmap->bitmap(),
                                                               &d2d_sr);
            if (FAILED(hr)) {
                qWarning("%s: Could not copy source rect area from source bitmap to intermediate bitmap: %#x", __FUNCTION__, hr);
                return;
            }
        } else {
            bool r = intermediate.resize(bitmap->size().width(),
                                         bitmap->size().height());
            if (!r) {
                qWarning("%s: Could not resize intermediate bitmap to source bitmap size", __FUNCTION__);
                return;
            }

            HRESULT hr = intermediate.bitmap()->CopyFromBitmap(NULL,
                                                               bitmap->bitmap(),
                                                               NULL);
            if (FAILED(hr)) {
                qWarning("%s: Could not copy source bitmap to intermediate bitmap: %#x", __FUNCTION__, hr);
                return;
            }
        }

        d->dc()->DrawBitmap(intermediate.bitmap(),
                            to_d2d_rect_f(r), state()->opacity,
                            d->interpolationMode());
    }
}

static ComPtr<IDWriteFontFace> fontFaceFromFontEngine(QFontEngine *fe)
{
    ComPtr<IDWriteFontFace> fontFace;

    switch (fe->type()) {
    case QFontEngine::Win:
    {
        QWindowsFontEngine *wfe = static_cast<QWindowsFontEngine *>(fe);
        QSharedPointer<QWindowsFontEngineData> wfed = wfe->fontEngineData();

        HGDIOBJ oldfont = wfe->selectDesignFont();
        HRESULT hr = QWindowsDirect2DContext::instance()->dwriteGdiInterop()->CreateFontFaceFromHdc(wfed->hdc, &fontFace);
        DeleteObject(SelectObject(wfed->hdc, oldfont));
        if (FAILED(hr))
            qWarning("%s: Could not create DirectWrite fontface from HDC: %#x", __FUNCTION__, hr);

    }
        break;

#ifndef QT_NO_DIRECTWRITE

    case QFontEngine::DirectWrite:
    {
        QWindowsFontEngineDirectWrite *wfedw = static_cast<QWindowsFontEngineDirectWrite *>(fe);
        fontFace = wfedw->directWriteFontFace();
    }
        break;

#endif // QT_NO_DIRECTWRITE

    default:
        qWarning("%s: Unknown font engine!", __FUNCTION__);
        break;
    }

    return fontFace;
}

void QWindowsDirect2DPaintEngine::drawStaticTextItem(QStaticTextItem *staticTextItem)
{
    Q_D(QWindowsDirect2DPaintEngine);
    D2D_TAG(D2DDebugDrawStaticTextItemTag);

    if (qpen_style(d->pen.qpen) == Qt::NoPen)
        return;

    if (staticTextItem->numGlyphs == 0)
        return;

    // If we can't support the current configuration with Direct2D, fall back to slow path
    // Most common cases are perspective transform and gradient brush as pen
    if ((state()->transform().isAffine() == false) || d->pen.emulate) {
        QPaintEngineEx::drawStaticTextItem(staticTextItem);
        return;
    }

    ComPtr<IDWriteFontFace> fontFace = fontFaceFromFontEngine(staticTextItem->fontEngine());
    if (!fontFace) {
        qWarning("%s: Could not find font - falling back to slow text rendering path.", __FUNCTION__);
        QPaintEngineEx::drawStaticTextItem(staticTextItem);
        return;
    }

    QVector<UINT16> glyphIndices(staticTextItem->numGlyphs);
    QVector<FLOAT> glyphAdvances(staticTextItem->numGlyphs);
    QVector<DWRITE_GLYPH_OFFSET> glyphOffsets(staticTextItem->numGlyphs);

    // XXX Are we generating a lot of cache misses here?
    for (int i = 0; i < staticTextItem->numGlyphs; i++) {
        glyphIndices[i] = UINT16(staticTextItem->glyphs[i]); // Imperfect conversion here

        // This looks  a little funky because the positions are precalculated
        glyphAdvances[i] = 0;
        glyphOffsets[i].advanceOffset = staticTextItem->glyphPositions[i].x.toReal();
        // Qt and Direct2D seem to disagree on the direction of the ascender offset...
        glyphOffsets[i].ascenderOffset = staticTextItem->glyphPositions[i].y.toReal() * -1;
    }

    drawGlyphRun(D2D1::Point2F(0, 0),
                 fontFace.Get(),
                 staticTextItem->font,
                 staticTextItem->numGlyphs,
                 glyphIndices.constData(),
                 glyphAdvances.constData(),
                 glyphOffsets.constData(),
                 false);
}

void QWindowsDirect2DPaintEngine::drawTextItem(const QPointF &p, const QTextItem &textItem)
{
    Q_D(QWindowsDirect2DPaintEngine);
    D2D_TAG(D2DDebugDrawTextItemTag);

    if (qpen_style(d->pen.qpen) == Qt::NoPen)
        return;

    const QTextItemInt &ti = static_cast<const QTextItemInt &>(textItem);
    if (ti.glyphs.numGlyphs == 0)
        return;

    // If we can't support the current configuration with Direct2D, fall back to slow path
    // Most common cases are perspective transform and gradient brush as pen
    if ((state()->transform().isAffine() == false) || d->pen.emulate) {
        QPaintEngine::drawTextItem(p, textItem);
        return;
    }

    ComPtr<IDWriteFontFace> fontFace = fontFaceFromFontEngine(ti.fontEngine);
    if (!fontFace) {
        qWarning("%s: Could not find font - falling back to slow text rendering path.", __FUNCTION__);
        QPaintEngine::drawTextItem(p, textItem);
        return;
    }

    QVector<UINT16> glyphIndices(ti.glyphs.numGlyphs);
    QVector<FLOAT> glyphAdvances(ti.glyphs.numGlyphs);
    QVector<DWRITE_GLYPH_OFFSET> glyphOffsets(ti.glyphs.numGlyphs);

    // XXX Are we generating a lot of cache misses here?
    for (int i = 0; i < ti.glyphs.numGlyphs; i++) {
        glyphIndices[i] = UINT16(ti.glyphs.glyphs[i]); // Imperfect conversion here
        glyphAdvances[i] = ti.glyphs.effectiveAdvance(i).toReal();
        glyphOffsets[i].advanceOffset = ti.glyphs.offsets[i].x.toReal();

        // XXX Should we negate the y value like for static text items?
        glyphOffsets[i].ascenderOffset = ti.glyphs.offsets[i].y.toReal();
    }

    const bool rtl = (ti.flags & QTextItem::RightToLeft);
    const QPointF offset(rtl ? ti.width.toReal() : 0, 0);

    drawGlyphRun(to_d2d_point_2f(p + offset),
                 fontFace.Get(),
                 ti.font(),
                 ti.glyphs.numGlyphs,
                 glyphIndices.constData(),
                 glyphAdvances.constData(),
                 glyphOffsets.constData(),
                 rtl);
}

// Points (1/72 inches) to Microsoft's Device Independent Pixels (1/96 inches)
inline static Q_DECL_CONSTEXPR FLOAT pointSizeToDIP(qreal pointSize)
{
    return pointSize + (pointSize / qreal(3.0));
}

inline static FLOAT pixelSizeToDIP(int pixelSize)
{
    FLOAT dpiX, dpiY;
    factory()->GetDesktopDpi(&dpiX, &dpiY);

    return FLOAT(pixelSize) / (dpiY / 96.0f);
}

inline static FLOAT fontSizeInDIP(const QFont &font)
{
    // Direct2d wants the font size in DIPs (Device Independent Pixels), each of which is 1/96 inches.
    if (font.pixelSize() == -1) {
        // font size was set as points
        return pointSizeToDIP(font.pointSizeF());
    } else {
        // font size was set as pixels
        return pixelSizeToDIP(font.pixelSize());
    }
}

void QWindowsDirect2DPaintEngine::drawGlyphRun(const D2D1_POINT_2F &pos,
                                               IDWriteFontFace *fontFace,
                                               const QFont &font,
                                               int numGlyphs,
                                               const UINT16 *glyphIndices,
                                               const FLOAT *glyphAdvances,
                                               const DWRITE_GLYPH_OFFSET *glyphOffsets,
                                               bool rtl)
{
    Q_D(QWindowsDirect2DPaintEngine);

    DWRITE_GLYPH_RUN glyphRun = {
        fontFace,               //    IDWriteFontFace           *fontFace;
        fontSizeInDIP(font),    //    FLOAT                     fontEmSize;
        numGlyphs,              //    UINT32                    glyphCount;
        glyphIndices,           //    const UINT16              *glyphIndices;
        glyphAdvances,          //    const FLOAT               *glyphAdvances;
        glyphOffsets,           //    const DWRITE_GLYPH_OFFSET *glyphOffsets;
        FALSE,                  //    BOOL                      isSideways;
        rtl ? 1 : 0             //    UINT32                    bidiLevel;
    };

    const bool antiAlias = bool((state()->renderHints & QPainter::TextAntialiasing)
                                && !(font.styleStrategy() & QFont::NoAntialias));
    d->dc()->SetTextAntialiasMode(antiAlias ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE
                                            : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);

    d->dc()->DrawGlyphRun(pos,
                          &glyphRun,
                          NULL,
                          d->pen.brush.Get(),
                          DWRITE_MEASURING_MODE_GDI_CLASSIC);
}

QT_END_NAMESPACE
