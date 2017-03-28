/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2007 Rivo Laks <rivolaks@hot.ee>
Copyright (C) 2008 Lucas Murray <lmurray@undefinedfire.com>
Copyright (C) 2017 Krzysztof Nowicki <krissn@op.pl>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/

#include "projector.h"

#include <QAction>
#include <QFile>
#include <kwinglutils.h>
#include <kwinglplatform.h>
#include <KGlobalAccel>
#include <KLocalizedString>
#include <QStandardPaths>
#include <kwinglutils.h>
#include <kwinxrenderutils.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>

#include "projectoreffectadaptor.h"

namespace KWin
{

static Q_CONSTEXPR double maxDeformFactor = 0.4;

static const QList<double> cornersMinValues = {
    0.0, 0.0,
    1.0 - maxDeformFactor, 0.0,
    1.0 - maxDeformFactor, 1.0 - maxDeformFactor,
    0.0, 1.0 - maxDeformFactor
};

static const QList<double> cornersMaxValues = {
    maxDeformFactor, maxDeformFactor,
    1.0, maxDeformFactor,
    1.0, 1.0,
    maxDeformFactor, 1.0
};

static const QList<double> cornersDefaulValues = {
    0.0, 0.0,
    1.0, 0.0,
    1.0, 1.0,
    0.0, 1.0
};

ProjectorEffect::ProjectorEffect()
    : m_cursorVisible(false)
{
    (void) new ProjectorEffectAdaptor(this);

    QDBusConnection dbus = QDBusConnection::sessionBus();
    dbus.registerObject(QStringLiteral("/Effects/Projector"), this);

    connect(effects, &EffectsHandler::screenGeometryChanged, this, &ProjectorEffect::slotScreenGeometryChanged);
    connect(effects, &EffectsHandler::mouseChanged, this, &ProjectorEffect::slotMouseChanged);
    connect(effects, &EffectsHandler::cursorShapeChanged, this, &ProjectorEffect::recreateTexture);
    recreateTexture();
}

ProjectorEffect::~ProjectorEffect()
{
    if (m_cursorVisible) {
        xcb_xfixes_show_cursor(xcbConnection(), x11RootWindow());
    }
}

bool ProjectorEffect::supported()
{
    return effects->compositingType() == OpenGL2Compositing;
}

void ProjectorEffect::prePaintScreen(ScreenPrePaintData &data, int time)
{
    data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;
    effects->prePaintScreen(data, time);
}

void ProjectorEffect::paintScreen(int mask, QRegion region, ScreenPaintData& data)
{
    effects->paintScreen(mask, region, data);

    if (m_cursorTexture && m_cursorVisible) {
        QPoint p = effects->cursorPos();
        for (const ScreenData &screenData : m_screenData) {
            if ((screenData.number != -1) && (screenData.rect.contains(p))) {
                p = translatePoint(p.x() - screenData.rect.left(), p.y() - screenData.rect.top(),
                                   screenData.transMatrix).toPoint();
                p.rx() += screenData.rect.left();
                p.ry() += screenData.rect.top();
            }
        }
        p -= m_cursorHotSpot;
        QRect rect(p.x() + data.xTranslation(), p.y() + data.yTranslation(), m_cursorSize.width(),
                   m_cursorSize.height());

        m_cursorTexture->bind();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture);
        QMatrix4x4 mvp = data.projectionMatrix();
        mvp.translate(rect.x(), rect.y());
        s->setUniform(GLShader::ModelViewProjectionMatrix, mvp);
        m_cursorTexture->render(region, rect);
        ShaderManager::instance()->popShader();
        m_cursorTexture->unbind();
        glDisable(GL_BLEND);
    }
}

void ProjectorEffect::prePaintWindow(EffectWindow *w, WindowPrePaintData &data, int time)
{
    for (const ScreenData &screenData : m_screenData) {
        if (screenData.number != -1) {
            QRect screenRect = screenData.rect;
            screenRect.adjust(0, 0, 1, 1);

            if (screenRect.intersects(w->expandedGeometry())) {
                data.quads = data.quads.makeGrid(100);
                data.quads = data.quads.splitAtX(screenRect.left() - w->x());
                data.quads = data.quads.splitAtX(screenRect.right() - w->x());
                data.quads = data.quads.splitAtY(screenRect.top() - w->y());
                data.quads = data.quads.splitAtY(screenRect.bottom() - w->y());
                data.setTransformed();
            }
        }
    }
    effects->prePaintWindow(w, data, time);
}

void ProjectorEffect::drawWindow(EffectWindow* w, int mask, QRegion region, WindowPaintData& data)
{
    for (const ScreenData &screenData : m_screenData) {
        if (screenData.number != -1) {
            QRect screenRect = screenData.rect;
            screenRect.adjust(0, 0, 1, 1);
            WindowQuadList newQuads;
            Q_FOREACH (WindowQuad quad, data.quads) {
                if (quad.left() + w->x() >= screenRect.left() && quad.top() + w->y() >= screenRect.top() &&
                    quad.right() + w->x() <= screenRect.right() && quad.bottom() + w->y() <= screenRect.bottom()) {
                    for (int i = 0; i < 4; i++) {
                        const QPointF point = translatePoint(quad[i].x() + w->x() - screenRect.left(),
                                                             quad[i].y() + w->y() - screenRect.top(),
                                                             screenData.transMatrix);
                        quad[i].setX(point.x() - w->x() + screenRect.left());
                        quad[i].setY(point.y() - w->y() + screenRect.top());
                    }
                }
                newQuads.append(quad);
            }
            data.quads = newQuads;
        }
    }
    effects->drawWindow(w, mask, region, data);
}

QPointF ProjectorEffect::translatePoint(float x, float y, const QMatrix3x3 &mat) const
{
    QGenericMatrix<1, 3, float> pointHomo;
    pointHomo(0, 0) = x;
    pointHomo(1, 0) = y;
    pointHomo(2, 0) = 1.0;
    QGenericMatrix<1, 3, float> pointTrans = mat * pointHomo;
    return QPoint(pointTrans(0, 0) / pointTrans(2, 0), pointTrans(1, 0) / pointTrans(2, 0));
}

QMatrix3x3 ProjectorEffect::calculateTransform(const QPolygonF &quad, const QRect &screenRect)
{
    /* Algorithm based on implementation found in keystone.5c, which is part of xrandr release. */
#ifndef NDEBUG
    if (quad.length() != 4)
        qFatal("Transform quad must have four edges!");
#endif
    QMatrix3x3 result;
    /* The quad stored in configuration is stored in relative form, where each coordinate is a
     * value between 0 and 1 inclusive. */
    QPolygonF q = quad;
    for (QPointF &point : q) {
        point.rx() *= screenRect.width();
        point.ry() *= screenRect.height();
    }
    result(0, 2) = q[0].x();
    result(1, 2) = q[0].y();
    result(2, 2) = 1.0;

    const float a = ((q[2].x() - q[3].x()) * (q[1].y() - q[2].y())
            - (q[2].y() - q[3].y()) * (q[1].x() - q[2].x())) * screenRect.height();
    const float b = (q[2].x() - q[1].x() - q[3].x() + q[0].x()) * (q[1].y() - q[2].y())
            - (q[2].y() - q[1].y() - q[3].y() + q[0].y()) * (q[1].x() - q[2].x());

    result(2, 1) = -b / a;

    result(2, 0) = (q[1].x() != q[2].x()) ?
        (result(2, 1) * (q[2].x() - q[3].x()) * screenRect.height()
                + q[2].x() - q[1].x() - q[3].x() + q[0].x())
                / ((q[1].x() - q[2].x()) * screenRect.width()) :
        (result(2, 1) * (q[2].y() - q[3].y()) * screenRect.height()
                + q[2].y() - q[1].y() - q[3].y() + q[0].y())
                / ((q[1].y() - q[2].y()) * screenRect.width());

    result(0, 0) = result(2, 0) * q[1].x() + (q[1].x() - q[0].x()) / screenRect.width();
    result(1, 0) = result(2, 0) * q[1].y() + (q[1].y() - q[0].y()) / screenRect.width();

    result(0, 1) = result(2, 1) * q[3].x() + (q[3].x() - q[0].x()) / screenRect.height();
    result(1, 1) = result(2, 1) * q[3].y() + (q[3].y() - q[0].y()) / screenRect.height();

    return result;
}

bool ProjectorEffect::isActive() const
{
    return !m_transformedRegion.isEmpty();
}

void ProjectorEffect::slotScreenGeometryChanged(const QSize &size)
{
    QVector<QString> screens;

    Q_UNUSED(size);

    for (int i = 0; i < effects->numScreens(); i++) {
        screens.append(effects->screenName(i));
    }

    QRegion transformedRegion;
    for (ScreenData &screenData : m_screenData) {
        screenData.number = screens.indexOf(screenData.id);
        if (screenData.number >= 0) {
            screenData.rect = effects->clientArea(ScreenArea, screenData.number, effects->currentDesktop());
            transformedRegion += screenData.rect;
            screenData.transMatrix = calculateTransform(screenData.quad, screenData.rect);
        } else {
        }
    }
    m_transformedRegion = transformedRegion;

    effects->addRepaintFull();
}

void ProjectorEffect::recreateTexture()
{
    effects->makeOpenGLContextCurrent();
    // load the cursor-theme image from the Xcursor-library
    xcb_xfixes_get_cursor_image_cookie_t keks = xcb_xfixes_get_cursor_image_unchecked(xcbConnection());
    xcb_xfixes_get_cursor_image_reply_t *ximg = xcb_xfixes_get_cursor_image_reply(xcbConnection(), keks, 0);
    if (ximg) {
        // turn the XcursorImage into a QImage that will be used to create the GLTexture/XRenderPicture.
        m_cursorSize = QSize(ximg->width, ximg->height);
        m_cursorHotSpot = QPoint(ximg->xhot, ximg->yhot);
        uint32_t *bits = xcb_xfixes_get_cursor_image_cursor_image(ximg);
        QImage img((uchar*)bits, m_cursorSize.width(), m_cursorSize.height(),
                   QImage::Format_ARGB32_Premultiplied);
        m_cursorTexture.reset(new GLTexture(img));
        m_cursorTexture->setFilter(GL_LINEAR);
        free(ximg);
    }
    else {
        m_cursorTexture.reset();
    }
}

void ProjectorEffect::slotMouseChanged(const QPoint& pos, const QPoint& old,
                                       Qt::MouseButtons buttons, Qt::MouseButtons oldbuttons,
                                       Qt::KeyboardModifiers modifiers,
                                       Qt::KeyboardModifiers oldmodifiers)
{
    Q_UNUSED(buttons)
    Q_UNUSED(oldbuttons)
    Q_UNUSED(modifiers)
    Q_UNUSED(oldmodifiers)

    m_cursorPos = pos;
    const QPoint p = pos - m_cursorHotSpot;
    const QRect rect(p.x(), p.y(), m_cursorSize.width(), m_cursorSize.height());
    const bool cursorVisible = m_transformedRegion.contains(rect);
    if ((m_cursorVisible && (pos != old)) || (cursorVisible != m_cursorVisible)) {
        effects->addRepaintFull();
    }
    if (cursorVisible != m_cursorVisible) {
        (cursorVisible ? xcb_xfixes_hide_cursor : xcb_xfixes_show_cursor)(xcbConnection(), x11RootWindow());
    }
    m_cursorVisible = cursorVisible;
}

bool ProjectorEffect::setScreenTranslation(const QString &screen, const QList<double> &corners)
{
    if (corners.length() != 8) {
        qCWarning(KWINEFFECTS) << "Expected array of 8 corner coordinates, found" << corners.length() << "items";
        return false;
    }

    auto screenDataIt = m_screenData.begin();
    for (; screenDataIt != m_screenData.end() && screenDataIt->id != screen; screenDataIt++) {};

    if (corners == cornersDefaulValues) {
        if (screenDataIt != m_screenData.end()) {
            m_screenData.erase(screenDataIt);
        }
    } else {
        auto minIt = cornersMinValues.begin();
        auto maxIt = cornersMaxValues.begin();
        for (const float &val : corners) {
            if (val < *minIt || val > *maxIt) {
                qCWarning(KWINEFFECTS) << "Allowed values for corner coordinate "
                        << minIt - cornersMinValues.begin() << " are between " << *minIt << " and "
                        << *maxIt << " inclusive";
                return false;
            }
            minIt++;
            maxIt++;
        }

        if (screenDataIt == m_screenData.end()) {
            ScreenData screenData;
            m_screenData.prepend(screenData);
            screenDataIt = m_screenData.begin();
        }
        screenDataIt->id = screen;
        auto it = corners.begin();
        screenDataIt->quad.clear();
        for (int i = 0; i < 4; i++) {
            float x = *it++;
            float y = *it++;
            screenDataIt->quad.append(QPointF(x, y));
        }
    }

    slotScreenGeometryChanged(QSize());

    return true;
}

} // namespace
