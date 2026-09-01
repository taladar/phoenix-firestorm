/**
 * @file llenvironmentlistener.cpp
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

// Precompiled header
#include "llviewerprecompiledheaders.h"
// associated header
#include "llenvironmentlistener.h"
// other Linden headers
#include "llenvironment.h"
#include "llevents.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "llsettingssky.h"

LLEnvironmentListener::LLEnvironmentListener():
    LLEventAPI("LLEnvironment",
               "Control the viewer-side environment (time of day, sun position)")
{
    add("setDayPosition",
        "Pin the local environment to [\"position\"] (0..1) along the day cycle.\n"
        "Applied as a fixed sky, so it does not drift while the viewer runs.\n"
        "Post on [\"reply\"] an event containing [\"ok\"] and, on failure, [\"error\"]",
        &LLEnvironmentListener::setDayPosition,
        llsd::map("position", LLSD(), "reply", LLSD()));

    add("setFixedSky",
        "Set the local environment to the settings asset [\"asset_id\"].\n"
        "Post on [\"reply\"] an event containing [\"ok\"]",
        &LLEnvironmentListener::setFixedSky,
        llsd::map("asset_id", LLSD(), "reply", LLSD()));

    add("setSunAzimuthElevation",
        "Point the sun at [\"azimuth\"] / [\"elevation\"], both in degrees.\n"
        "Post on [\"reply\"] an event containing [\"ok\"] and, on failure, [\"error\"]",
        &LLEnvironmentListener::setSunAzimuthElevation,
        llsd::map("azimuth", LLSD(), "elevation", LLSD(), "reply", LLSD()));

    add("clearLocal",
        "Drop the local environment override, reverting to parcel/region.",
        &LLEnvironmentListener::clearLocal);

    add("getEnvironment",
        "Reply with [\"selected\"], [\"sun_direction\"], [\"moon_direction\"].",
        &LLEnvironmentListener::getEnvironment,
        llsd::map("reply", LLSD()));
}

void LLEnvironmentListener::setDayPosition(const LLSD& event) const
{
    LLEnvironment& env = LLEnvironment::instance();

    F32 position = (F32)event["position"].asReal();
    position = llclamp(position, 0.f, 1.f);

    // Prefer the region's own cycle so "noon" means what it means on this
    // region, and fall back to whatever the local layer already resolves to.
    LLSettingsDay::ptr_t day = env.getEnvironmentDay(LLEnvironment::ENV_REGION);
    if (!day)
    {
        day = env.getEnvironmentDay(LLEnvironment::ENV_LOCAL);
    }
    if (!day)
    {
        sendReply(llsd::map("ok", false,
                            "error", "no day cycle available yet"), event);
        return;
    }

    // TRACK_GROUND_LEVEL (1) is the ground-level sky track; track 0 is water.
    LLSettingsSky::ptr_t sky =
        day->getSkyAtKeyframe(position, LLSettingsDay::TRACK_GROUND_LEVEL);
    if (!sky)
    {
        sendReply(llsd::map("ok", false,
                            "error", "day cycle has no sky at that position"), event);
        return;
    }

    // A *fixed* sky rather than a running cycle: a capture run must not drift
    // between its first frame and its last.
    env.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT, true);

    sendReply(llsd::map("ok", true, "position", position), event);
}

void LLEnvironmentListener::setFixedSky(const LLSD& event) const
{
    LLEnvironment& env = LLEnvironment::instance();

    const LLUUID asset_id = event["asset_id"].asUUID();
    if (asset_id.isNull())
    {
        sendReply(llsd::map("ok", false, "error", "asset_id is null"), event);
        return;
    }

    env.setEnvironment(LLEnvironment::ENV_LOCAL, asset_id, LLEnvironment::TRANSITION_INSTANT);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT, true);

    sendReply(llsd::map("ok", true), event);
}

void LLEnvironmentListener::setSunAzimuthElevation(const LLSD& event) const
{
    LLEnvironment& env = LLEnvironment::instance();

    LLSettingsSky::ptr_t current = env.getCurrentSky();
    if (!current)
    {
        sendReply(llsd::map("ok", false, "error", "no sky available yet"), event);
        return;
    }
    // Clone, so we edit the local override rather than the region's settings.
    LLSettingsSky::ptr_t sky = current->buildClone();

    F32 azimuth   = (F32)event["azimuth"].asReal()   * DEG_TO_RAD;
    F32 elevation = (F32)event["elevation"].asReal() * DEG_TO_RAD;
    if (is_approx_zero(elevation))
    {
        // Exactly zero elevation degenerates the rotation; nudge it, as the
        // environment-adjust floater does.
        elevation = F_APPROXIMATELY_ZERO;
    }

    LLQuaternion quat;
    quat.setAngleAxis(-elevation, 0, 1, 0);
    LLQuaternion az_quat;
    az_quat.setAngleAxis(F_TWO_PI - azimuth, 0, 0, 1);
    quat *= az_quat;

    sky->setSunRotation(quat);
    sky->update();

    env.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT, true);

    sendReply(llsd::map("ok", true), event);
}

void LLEnvironmentListener::clearLocal(const LLSD&) const
{
    LLEnvironment& env = LLEnvironment::instance();
    env.clearEnvironment(LLEnvironment::ENV_LOCAL);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT, true);
}

void LLEnvironmentListener::getEnvironment(const LLSD& event) const
{
    LLEnvironment& env = LLEnvironment::instance();

    LLSD reply = llsd::map("selected", (S32)env.getSelectedEnvironment());
    if (LLSettingsSky::ptr_t sky = env.getCurrentSky())
    {
        reply["sun_direction"]  = ll_sd_from_vector3(sky->getSunDirection());
        reply["moon_direction"] = ll_sd_from_vector3(sky->getMoonDirection());
        reply["sky_name"]       = sky->getName();
    }
    sendReply(reply, event);
}
