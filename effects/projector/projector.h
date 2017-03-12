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

#ifndef KWIN_PROJECTOR_H
#define KWIN_PROJECTOR_H

#include <QGenericMatrix>
#include <kwineffects.h>

namespace KWin
{

class GLTexture;

/**
 * Transforms an output to correct for projector misalignment
 **/
class ProjectorEffect
    : public Effect
{
    Q_OBJECT
public:
    ProjectorEffect();
    ~ProjectorEffect();

    virtual void drawWindow(EffectWindow* w, int mask, QRegion region,
                            WindowPaintData& data) Q_DECL_OVERRIDE;
    virtual void prePaintScreen(ScreenPrePaintData &data, int time) Q_DECL_OVERRIDE;
    virtual void prePaintWindow(EffectWindow *w, WindowPrePaintData &data, int time) Q_DECL_OVERRIDE;
    virtual void paintScreen(int mask, QRegion region, ScreenPaintData& data) Q_DECL_OVERRIDE;
    virtual bool isActive() const Q_DECL_OVERRIDE;

    static bool supported();

private Q_SLOTS:
    void slotScreenGeometryChanged(const QSize &size);
    void slotMouseChanged(const QPoint& pos, const QPoint& old,
                          Qt::MouseButtons buttons, Qt::MouseButtons oldbuttons,
                          Qt::KeyboardModifiers modifiers, Qt::KeyboardModifiers oldmodifiers);
private:
    QPointF translatePoint(float x, float y, const QMatrix3x3 &mat/*const QRect &screenRect*/) const;
    static QMatrix3x3 calculateTransform(const QPolygonF &quad, const QRect &screenRect);
    void recreateTexture();

    struct ScreenData {
        QString id;
        int number;
        QPolygonF quad;
        QMatrix3x3 transMatrix;
        QRect rect;
    };

    QVector<ScreenData> m_screenData;
    QRegion m_transformedRegion;
    QScopedPointer<GLTexture> m_cursorTexture;
    QSize m_cursorSize;
    QPoint m_cursorHotSpot;
    QPoint m_cursorPos;
    bool m_cursorVisible;
};

} // namespace

#endif
