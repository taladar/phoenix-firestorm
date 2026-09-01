/**
 * @file llenvironmentlistener.h
 * @brief LLEventAPI for LLEnvironment -- scripted time of day.
 *
 * $LicenseInfo:firstyear=2025&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef LL_LLENVIRONMENTLISTENER_H
#define LL_LLENVIRONMENTLISTENER_H

#include "lleventapi.h"

class LLSD;

/**
 * Exposes deterministic environment control over the LLSD event API, which is
 * what the --leap plugin channel and the test harness both need.
 *
 * Everything here applies to ENV_LOCAL (the viewer-side personal layer) with
 * TRANSITION_INSTANT, so the change is visible on the very next frame rather
 * than eased in over several seconds -- an eased transition means two viewers
 * photographed at the same moment are lit differently for no interesting
 * reason.
 */
class LLEnvironmentListener : public LLEventAPI
{
public:
    LLEnvironmentListener();

private:
    /// ["position"] 0..1 along the day cycle; replies ["ok"].
    void setDayPosition(const LLSD& event) const;
    /// ["asset_id"] one of LLEnvironment's KNOWN_SKY_* ids; replies ["ok"].
    void setFixedSky(const LLSD& event) const;
    /// ["azimuth"], ["elevation"] in degrees; replies ["ok"].
    void setSunAzimuthElevation(const LLSD& event) const;
    /// Drop the local override and go back to region/parcel environment.
    void clearLocal(const LLSD& event) const;
    /// Replies with the current sun/moon direction and selected layer.
    void getEnvironment(const LLSD& event) const;
};

#endif /* ! defined(LL_LLENVIRONMENTLISTENER_H) */
