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

namespace KWin
{

ProjectorEffect::ProjectorEffect()
{
    connect(effects, &EffectsHandler::screenGeometryChanged, this, &ProjectorEffect::slotScreenGeometryChanged);
}

ProjectorEffect::~ProjectorEffect()
{
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
    return true;
}

void ProjectorEffect::slotScreenGeometryChanged(const QSize &size)
{
    QVector<QString> screens;

    Q_UNUSED(size);

    for (int i = 0; i < effects->numScreens(); i++) {
        screens.append(effects->screenName(i));
    }

    for (ScreenData &screenData : m_screenData) {
        screenData.number = screens.indexOf(screenData.id);
        if (screenData.number >= 0) {
            screenData.rect = effects->clientArea(ScreenArea, screenData.number, effects->currentDesktop());
            screenData.transMatrix = calculateTransform(screenData.quad, screenData.rect);
        } else {
        }
    }
}

} // namespace

