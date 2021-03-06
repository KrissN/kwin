/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2016 Roman Gilg <subdiff@gmail.com>

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
#ifndef KWIN_DRM_OBJECT_PLANE_H
#define KWIN_DRM_OBJECT_PLANE_H

#include "drm_object.h"
// drm
#include <xf86drmMode.h>

namespace KWin
{

class DrmBuffer;

class DrmPlane : public DrmObject
{
public:
    DrmPlane(uint32_t plane_id, int fd);

    virtual ~DrmPlane();

    enum class PropertyIndex {
        Type = 0,
        SrcX,
        SrcY,
        SrcW,
        SrcH,
        CrtcX,
        CrtcY,
        CrtcW,
        CrtcH,
        FbId,
        CrtcId,
        Count
    };

    enum class TypeIndex {
        Primary = 0,
        Cursor,
        Overlay,
        Count
    };
    
    bool init();
    bool initProps();
    TypeIndex type();
    bool isCrtcSupported(uint32_t crtc);
    DrmObject::AtomicReturn atomicReqPlanePopulate(drmModeAtomicReq *req);

    DrmBuffer *current(){
        return m_current;
    }
    DrmBuffer *next(){
        return m_next;
    }
    void setCurrent(DrmBuffer *b){
        m_current = b;
    }
    void setNext(DrmBuffer *b){
        m_next = b;
    }

    QVector<uint32_t> formats(){
        return m_formats;
    }
    void setFormats(uint32_t const *f, int fcount);

    void setPossibleCrtcs(uint32_t value){
        m_possibleCrtcs = value;
    }
    uint32_t possibleCrtcs(){
        return m_possibleCrtcs;
    }

private:
    DrmBuffer *m_current = nullptr;
    DrmBuffer *m_next = nullptr;

    // TODO: See weston drm_output_check_plane_format for future use of these member variables
    QVector<uint32_t> m_formats;        // Possible formats, which can be presented on this plane

    // TODO: when using overlay planes in the future: restrict possible screens / crtcs of planes
    uint32_t m_possibleCrtcs;
};

}

#endif

