/**
 * @file fstestscenedump.cpp
 * @brief Structured scene dump for cross-viewer comparison (FSTestHarness).
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

#include "llviewerprecompiledheaders.h"

#include "fstestscenedump.h"

#include <fstream>
#include <iomanip>

#include <boost/json.hpp>

#include "lldate.h"
#include "llsdjson.h"
#include "llsdutil_math.h"
#include "llversioninfo.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llenvironment.h"
#include "llsettingssky.h"
#include "lltextureentry.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llmotion.h"
#include "llmotioncontroller.h"
#include "llvoavatar.h"
#include "llvovolume.h"
#include "fsgridhandler.h"
#include "pipeline.h"

namespace
{
    // ll_sd_from_vector3 / _vector3d / _quaternion come from llsdutil_math.h,
    // and are what the other listeners already emit -- so a consumer that can
    // read LLAgent.getPosition can read this too.
    LLSD color4(const LLColor4& c)
    {
        LLSD out = LLSD::emptyArray();
        out.append(c.mV[0]);
        out.append(c.mV[1]);
        out.append(c.mV[2]);
        out.append(c.mV[3]);
        return out;
    }

    /** Identity and build of this viewer, so a dump names what produced it. */
    LLSD buildContext()
    {
        LLSD ctx = LLSD::emptyMap();
        ctx["viewer"]  = "firestorm";
        ctx["channel"] = LLVersionInfo::instance().getChannel();
        ctx["version"] = LLVersionInfo::instance().getVersion();
        ctx["time"]    = LLDate::now().toHTTPDateString("%Y-%m-%dT%H:%M:%S");
        ctx["grid"]    = LLGridManager::instance().getGrid();

        if (LLViewerRegion* region = gAgent.getRegion())
        {
            ctx["region_name"] = region->getName();
            ctx["region_id"]   = region->getRegionID();
            // A U64 does not survive LLSD; the handle is identity, so string it.
            ctx["region_handle"] = llformat("%llu",
                                            (unsigned long long)region->getHandle());
            ctx["region_width"]  = region->getWidth();
        }
        return ctx;
    }

    /** Where the camera is and what it can see -- the framing of the shot. */
    LLSD buildCamera()
    {
        LLSD cam = LLSD::emptyMap();
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        if (!camera)
        {
            return cam;
        }

        cam["origin_agent"] = ll_sd_from_vector3(camera->getOrigin());
        cam["at_axis"]      = ll_sd_from_vector3(camera->getAtAxis());
        cam["up_axis"]      = ll_sd_from_vector3(camera->getUpAxis());
        cam["left_axis"]    = ll_sd_from_vector3(camera->getLeftAxis());
        cam["fov_radians"]  = camera->getView();
        cam["aspect"]       = camera->getAspect();
        cam["near_clip"]    = camera->getNear();
        cam["far_clip"]     = camera->getFar();

        cam["origin_global"] = ll_sd_from_vector3d(gAgentCamera.getCameraPositionGlobal());
        cam["focus_global"]  = ll_sd_from_vector3d(gAgentCamera.getFocusGlobal());
        cam["camera_mode"]   = (S32)gAgentCamera.getCameraMode();

        if (LLViewerRegion* region = gAgent.getRegion())
        {
            cam["origin_region"] =
                ll_sd_from_vector3(region->getPosRegionFromGlobal(gAgentCamera.getCameraPositionGlobal()));
            cam["focus_region"] =
                ll_sd_from_vector3(region->getPosRegionFromGlobal(gAgentCamera.getFocusGlobal()));
        }
        return cam;
    }

    /** The atmospheric inputs the WL sky shaders are actually bound with.
     *
     * Two viewers can agree about the sky's name, the sun's direction and every
     * other thing a dump reports, and still draw very different skies -- because
     * what the shader runs on is this uniform block, and a dump that does not
     * carry it cannot say whether a divergence is in the numbers or in the maths.
     * These are the `skyV.glsl` / `cloudsV.glsl` uniforms, read back through the
     * same getters `LLSettingsVOSky::applySpecial` and `applyToUniforms` push.
     */
    LLSD buildSkyParams(const LLSettingsSky::ptr_t& sky)
    {
        LLSD params = LLSD::emptyMap();

        params["sunlight_color"]       = ll_sd_from_vector3(LLVector3(sky->getSunlightColor().mV));
        params["moonlight_color"]      = ll_sd_from_vector3(LLVector3(sky->getMoonlightColor().mV));
        params["ambient_color"]        = ll_sd_from_vector3(LLVector3(LLColor3(sky->getTotalAmbient()).mV));
        params["blue_horizon"]         = ll_sd_from_vector3(LLVector3(sky->getBlueHorizon().mV));
        params["blue_density"]         = ll_sd_from_vector3(LLVector3(sky->getBlueDensity().mV));
        params["haze_horizon"]         = sky->getHazeHorizon();
        params["haze_density"]         = sky->getHazeDensity();
        params["density_multiplier"]   = sky->getDensityMultiplier();
        params["distance_multiplier"]  = sky->getDistanceMultiplier();
        params["max_y"]                = sky->getMaxY();
        params["gamma"]                = sky->getGamma();
        params["glow"]                 = ll_sd_from_vector3(LLVector3(sky->getGlow().mV));
        params["cloud_color"]          = ll_sd_from_vector3(LLVector3(sky->getCloudColor().mV));
        params["cloud_shadow"]         = sky->getCloudShadow();
        params["cloud_scale"]          = sky->getCloudScale();
        params["cloud_variance"]       = sky->getCloudVariance();
        params["sun_up_factor"]        = sky->getIsSunUp() ? 1.0 : 0.0;
        params["sun_moon_glow_factor"] = sky->getSunMoonGlowFactor();
        params["star_brightness"]      = sky->getStarBrightness();
        params["moisture_level"]       = sky->getSkyMoistureLevel();
        params["droplet_radius"]       = sky->getSkyDropletRadius();
        params["ice_level"]            = sky->getSkyIceLevel();

        // The "fake HDR" scale the sky colour is multiplied by after
        // linearisation (`softenLight`), and the branch that decides it
        // (`LLSettingsVOSky::applySpecial`): a legacy / classic-mode sky is 1.0,
        // an EEP sky that authors a probe ambiance is sqrt(gamma) * 2.
        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
        static LLCachedControl<F32> auto_adjust_hdr_scale(gSavedSettings, "RenderSkyAutoAdjustHDRScale", 2.f);
        const bool classic_mode = sky->canAutoAdjust() && !should_auto_adjust();
        F32 sky_hdr_scale = 1.f;
        if (sky->getReflectionProbeAmbiance() != 0.f)
        {
            sky_hdr_scale = sqrtf(sky->getGamma()) * 2.f;
        }
        else if (sky->canAutoAdjust() && should_auto_adjust())
        {
            sky_hdr_scale = auto_adjust_hdr_scale();
        }
        params["sky_hdr_scale"]              = sky_hdr_scale;
        params["reflection_probe_ambiance"]  = sky->getReflectionProbeAmbiance();
        params["classic_mode"]               = classic_mode;

        return params;
    }

    /** The lighting the frame was rendered under. */
    LLSD buildEnvironment()
    {
        LLSD env = LLSD::emptyMap();
        LLEnvironment& environment = LLEnvironment::instance();

        env["selected"] = (S32)environment.getSelectedEnvironment();

        LLSettingsSky::ptr_t sky = environment.getCurrentSky();
        if (sky)
        {
            env["sun_direction"]  = ll_sd_from_vector3(sky->getSunDirection());
            env["moon_direction"] = ll_sd_from_vector3(sky->getMoonDirection());
            env["sun_rotation"]   = ll_sd_from_quaternion(sky->getSunRotation());
            env["sky_name"]       = sky->getName();
            env["sky_params"]     = buildSkyParams(sky);
        }
        if (LLSettingsWater::ptr_t water = environment.getCurrentWater())
        {
            env["water_name"] = water->getName();
        }
        return env;
    }

    /** Per-face appearance -- where "the texture is wrong" actually lives. */
    LLSD buildFaces(LLViewerObject* obj)
    {
        LLSD faces = LLSD::emptyArray();
        const U8 num_tes = obj->getNumTEs();
        for (U8 i = 0; i < num_tes; ++i)
        {
            const LLTextureEntry* te = obj->getTE(i);
            if (!te)
            {
                continue;
            }
            LLSD face = LLSD::emptyMap();
            face["index"]      = (S32)i;
            face["texture"]    = te->getID();
            face["color"]      = color4(te->getColor());
            face["scale_s"]    = te->getScaleS();
            face["scale_t"]    = te->getScaleT();
            face["offset_s"]   = te->getOffsetS();
            face["offset_t"]   = te->getOffsetT();
            face["rotation"]   = te->getRotation();
            face["bump"]       = (S32)te->getBumpmap();
            face["shiny"]      = (S32)te->getShiny();
            face["fullbright"] = (S32)te->getFullbright();
            face["glow"]       = te->getGlow();

            // Legacy Blinn-Phong and PBR materials are separate ids; a
            // divergence is usually one arriving and the other not.
            if (!te->getMaterialID().isNull())
            {
                face["material_id"] = te->getMaterialID().asString();
            }
            if (te->getGLTFMaterial())
            {
                face["gltf_material"] = te->getGLTFRenderMaterial() ? true : false;
            }
            faces.append(face);
        }
        return faces;
    }

    /** Every object the viewer holds for the agent's region. */
    LLSD buildObjects()
    {
        LLSD objects = LLSD::emptyArray();
        LLViewerRegion* agent_region = gAgent.getRegion();

        const S32 count = gObjectList.getNumObjects();
        for (S32 i = 0; i < count; ++i)
        {
            LLViewerObject* obj = gObjectList.getObject(i);
            if (!obj || obj->isDead())
            {
                continue;
            }
            // Only the agent's own region, so a dump is comparable between two
            // viewers that may have drawn different neighbours into cache.
            if (agent_region && obj->getRegion() != agent_region)
            {
                continue;
            }
            if (obj->isAvatar())
            {
                continue; // reported separately, with appearance state
            }

            LLSD entry = LLSD::emptyMap();
            entry["id"]       = obj->getID();
            entry["local_id"] = (S32)obj->getLocalID();
            entry["pcode"]    = obj->getPCodeString();
            entry["position"] = ll_sd_from_vector3(obj->getPositionRegion());
            entry["rotation"] = ll_sd_from_quaternion(obj->getRotationRegion());
            entry["scale"]    = ll_sd_from_vector3(obj->getScale());
            entry["num_faces"] = (S32)obj->getNumTEs();
            entry["faces"]    = buildFaces(obj);

            if (LLVOVolume* volume = dynamic_cast<LLVOVolume*>(obj))
            {
                entry["is_mesh"] = volume->isMesh();
                if (volume->isMesh())
                {
                    entry["mesh_id"] = volume->getMeshID();
                }
                entry["is_sculpt"] = volume->isSculpted();
                entry["lod"]       = volume->getLOD();
                entry["is_flexible"] = volume->isFlexible();
                entry["is_light"]  = volume->getIsLight();
            }

            entry["visible"] = obj->mDrawable.notNull()
                            && !obj->mDrawable->isState(LLDrawable::FORCE_INVISIBLE);
            objects.append(entry);
        }
        return objects;
    }

    /** Where a motion's clock has reached inside the motion itself: wrapped by
        its duration when it loops, clamped to it when it plays once, and absent
        when the duration is unknown. This is the "which frame" of an animation,
        and the number two viewers can be compared on -- `time` runs from
        whenever each of them started playing. */
    bool loopTime(F32 time, F32 duration, bool looping, F32& out)
    {
        if (duration <= 0.f)
        {
            return false;
        }
        const F32 elapsed = llmax(time, 0.f);
        out = looping ? fmodf(elapsed, duration) : llmin(elapsed, duration);
        return true;
    }

    /** What one avatar is playing, in the order the viewer applies it.

        mActiveMotions is front-inserted on activation, so this list runs most
        recently started first -- which is half of what decides who owns a joint
        (priority is the other half), and why the order is reported rather than
        sorted into something tidier. */
    LLSD buildAnimations(LLVOAvatar* avatar)
    {
        LLSD animations = LLSD::emptyArray();
        LLMotionController& controller = avatar->getMotionController();
        const F32 now = controller.getAnimTime();
        for (LLMotion* motion : controller.getActiveMotions())
        {
            if (!motion)
            {
                continue;
            }
            LLSD entry = LLSD::emptyMap();
            entry["id"] = motion->getID();
            const F32 time = now - motion->getActivationTimestamp();
            entry["time"] = time;
            const F32 duration = motion->getDuration();
            const bool looping = motion->getLoop();
            if (duration > 0.f)
            {
                entry["duration"] = duration;
                F32 wrapped = 0.f;
                if (loopTime(time, duration, looping, wrapped))
                {
                    entry["loop_time"] = wrapped;
                }
            }
            entry["looping"] = looping;
            entry["priority"] = (S32)motion->getPriority();
            entry["stopping"] = motion->isStopped();
            // The simulator numbers what it asked for; a motion the viewer
            // started itself carries no number of the simulator's.
            LLVOAvatar::AnimIterator found = avatar->mPlayingAnimations.find(motion->getID());
            if (found != avatar->mPlayingAnimations.end())
            {
                entry["sequence"] = found->second;
            }
            animations.append(entry);
        }
        return animations;
    }

    /** Avatars, with the appearance state that explains a grey or bald one. */
    LLSD buildAvatars()
    {
        LLSD avatars = LLSD::emptyArray();
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead())
            {
                continue;
            }
            LLSD entry = LLSD::emptyMap();
            entry["id"]            = avatar->getID();
            entry["is_self"]       = avatar->isSelf();
            // An animesh object is an LLControlAvatar: it rides the avatar
            // skeleton and so turns up in LLCharacter::sInstances, but it has
            // no agent behind it. Counting one as an avatar overstates how many
            // residents are present and invites a comparison to read a rezzed
            // animesh as a person, so say which it is rather than dropping it --
            // whether the animesh rezzed at all is worth comparing too.
            entry["is_control_avatar"] = avatar->isControlAvatar();
            entry["position"]      = ll_sd_from_vector3(avatar->getPositionRegion());
            entry["rotation"]      = ll_sd_from_quaternion(avatar->getRotationRegion());
            entry["animations"] = buildAnimations(avatar);
            entry["is_fully_loaded"] = avatar->isFullyLoaded();
            entry["visual_complexity"] = (S32)avatar->getVisualComplexity();
            avatars.append(entry);
        }
        return avatars;
    }

    /** The render settings that decide what the frame could contain at all. */
    LLSD buildRender()
    {
        LLSD render = LLSD::emptyMap();
        render["draw_distance"]  = gSavedSettings.getF32("RenderFarClip");
        render["quality_level"]  = (S32)gSavedSettings.getU32("RenderQualityPerformance");
        render["shadow_detail"]  = gSavedSettings.getS32("RenderShadowDetail");
        render["reflection_probes"] = gSavedSettings.getBOOL("RenderReflectionsEnabled");
        render["reflection_detail"] = gSavedSettings.getS32("RenderReflectionProbeDetail");
        render["max_texture_res"]   = (S32)gSavedSettings.getU32("RenderMaxTextureResolution");
        render["mesh_lod_boost"]    = gSavedSettings.getF32("RenderVolumeLODFactor");
        render["visible_drawables"] = (S32)gPipeline.mNumVisibleFaces;
        return render;
    }
}

namespace FSTestSceneDump
{

LLSD build()
{
    LLSD dump = LLSD::emptyMap();
    dump["schema_version"] = SCHEMA_VERSION;
    dump["context"]        = buildContext();
    dump["camera"]         = buildCamera();
    dump["environment"]    = buildEnvironment();
    dump["render"]         = buildRender();
    dump["objects"]        = buildObjects();
    dump["avatars"]        = buildAvatars();
    return dump;
}

bool writeJson(const LLSD& dump, const std::string& path, std::string& error)
{
    error.clear();

    std::ofstream out(path.c_str());
    if (!out)
    {
        error = "cannot write '" + path + "'";
        return false;
    }
    out << LlsdToJson(dump);
    if (!out)
    {
        error = "write failed for '" + path + "'";
        return false;
    }
    return true;
}

} // namespace FSTestSceneDump
