/**
 * @file fstestharness.cpp
 * @brief Unattended capture mode, for cross-viewer comparison runs.
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

#include "fstestharness.h"

#include <cstdlib>
#include <sstream>

#include "lldir.h"
#include "llsdserialize.h"
#include "llsdjson.h"
#include "llstring.h"

#include "fsgridhandler.h"
#include "fstestconfig.h"
#include "fstestscenedump.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llenvironment.h"
#include "llsettingsdaycycle.h"
#include "llsettingssky.h"
#include "llsettingsvo.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
// llviewerwindow.h only forward-declares LLWindow, and this file calls through
// it to ask the window system for a size (and to read back what it gave us).
#include "llwindow.h"
#include "llsnapshotmodel.h"
#include "llfloaterreg.h"
#include "llfloater.h"
#include "pipeline.h"

namespace
{
    /// How long to wait for a clean logout before escalating.
    const F32 LOGOUT_GRACE_SECS = 15.f;
    /// Match sl-client: region up this long before a capture may be taken.
    const F32 MIN_SETTLE_SECS = 5.f;
    /// Match sl-client: consecutive quiet frames required.
    const S32 QUIET_HOLD_FRAMES = 30;

    std::string envString(const char* name)
    {
        auto value = LLStringUtil::getoptenv(name);
        return value ? *value : std::string();
    }

    bool envF32(const char* name, F32& out)
    {
        const std::string raw = envString(name);
        if (raw.empty())
        {
            return false;
        }
        try
        {
            out = (F32)std::stod(raw);
            return true;
        }
        catch (const std::exception&)
        {
            LL_WARNS("FSTestHarness") << name << ": not a number: '" << raw
                                      << "' -- ignored" << LL_ENDL;
            return false;
        }
    }

    bool envS32(const char* name, S32& out)
    {
        const std::string raw = envString(name);
        if (raw.empty())
        {
            return false;
        }
        try
        {
            out = (S32)std::stol(raw);
            return true;
        }
        catch (const std::exception&)
        {
            LL_WARNS("FSTestHarness") << name << ": not a number: '" << raw
                                      << "' -- ignored" << LL_ENDL;
            return false;
        }
    }

    /**
     * A boolean environment variable, with sl-client's own falsey set so that
     * one env block means the same thing to both viewers: unset, empty, "0",
     * "false", "no" and "off" are false; anything else is true.
     */
    bool envBool(const char* name, bool& out)
    {
        std::string raw = envString(name);
        if (raw.empty())
        {
            return false;
        }
        LLStringUtil::toLower(raw);
        out = !(raw == "0" || raw == "false" || raw == "no" || raw == "off");
        return true;
    }

    /**
     * A setting, else an environment variable. Command line wins, because it
     * is the more specific statement of intent.
     */
    std::string settingOrEnv(const char* setting, const char* env_name)
    {
        const std::string from_setting = gSavedSettings.getString(setting);
        if (!from_setting.empty())
        {
            return from_setting;
        }
        return envString(env_name);
    }

    /**
     * One skin as skins.xml describes it: the folder its files load from, the
     * name the viewer shows for it, and the same pair for each of its themes.
     */
    struct SkinEntry
    {
        std::string mFolder;
        std::string mName;
        /// folder, name -- in skins.xml order, so the first is the default.
        std::vector<std::pair<std::string, std::string>> mThemes;
    };

    /**
     * Read skins.xml -- the same catalogue the preferences skin panel reads,
     * from the same place. Returns false with @a error set when it cannot be
     * read or holds no skins.
     */
    bool loadSkinCatalogue(std::vector<SkinEntry>& out, std::string& error)
    {
        const std::string path =
            gDirUtilp->getSkinBaseDir() + gDirUtilp->getDirDelimiter() + "skins.xml";
        llifstream file(path.c_str(), std::ios::binary);
        if (!file.is_open())
        {
            error = "cannot read " + path;
            return false;
        }

        LLSD skins;
        if (LLSDSerialize::fromXMLDocument(skins, file) == LLSDParser::PARSE_FAILURE ||
            !skins.isArray())
        {
            error = path + " is not an array of skins";
            return false;
        }

        for (LLSD::array_const_iterator it = skins.beginArray(); it != skins.endArray(); ++it)
        {
            SkinEntry entry;
            entry.mFolder = (*it)["folder"].asString();
            entry.mName = (*it)["name"].asString();
            const LLSD& themes = (*it)["themes"];
            if (themes.isArray())
            {
                for (LLSD::array_const_iterator t = themes.beginArray(); t != themes.endArray(); ++t)
                {
                    entry.mThemes.emplace_back((*t)["folder"].asString(),
                                               (*t)["name"].asString());
                }
            }
            out.push_back(entry);
        }

        if (out.empty())
        {
            error = path + " lists no skins";
            return false;
        }
        return true;
    }

    bool sameName(const std::string& a, const std::string& b)
    {
        return LLStringUtil::compareInsensitive(a, b) == 0;
    }

    /// Every skin's name and folder, for an error message that leaves the
    /// operator able to fix the typo without opening skins.xml.
    std::string skinChoices(const std::vector<SkinEntry>& skins)
    {
        std::ostringstream out;
        for (const SkinEntry& skin : skins)
        {
            if (out.tellp() > 0)
            {
                out << ", ";
            }
            out << skin.mName << " (" << skin.mFolder << ')';
        }
        return out.str();
    }

    /// As skinChoices, for one skin's themes. A theme whose folder is empty --
    /// every skin's first -- is shown by name alone, since that is the only
    /// way to ask for it.
    std::string themeChoices(const SkinEntry& skin)
    {
        std::ostringstream out;
        for (const auto& theme : skin.mThemes)
        {
            if (out.tellp() > 0)
            {
                out << ", ";
            }
            out << theme.second;
            if (!theme.first.empty())
            {
                out << " (" << theme.first << ')';
            }
        }
        return out.str();
    }

    /// Parse "x,y,z" as sl-client does, in region-local SL (Z-up) metres.
    bool parseVec3(const std::string& raw, LLVector3& out)
    {
        F32 v[3] = { 0.f, 0.f, 0.f };
        size_t start = 0;
        for (S32 i = 0; i < 3; ++i)
        {
            const size_t comma = raw.find(',', start);
            const std::string piece =
                (i == 2) ? raw.substr(start) : raw.substr(start, comma - start);
            if (i < 2 && comma == std::string::npos)
            {
                return false;
            }
            try
            {
                v[i] = (F32)std::stod(piece);
            }
            catch (const std::exception&)
            {
                return false;
            }
            start = comma + 1;
        }
        out.setVec(v[0], v[1], v[2]);
        return true;
    }
}

FSTestHarness::FSTestHarness()
{
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool FSTestHarness::initFromCommandLine()
{
    // The screenshot directory first, before anything that can be refused:
    // it is where a refusal is reported, and a driver reading an empty
    // directory can only say the run never happened.
    mScreenshotDir = settingOrEnv("FSTestScreenshotDir", "SL_VIEWER_SCREENSHOT_DIR");
    if (!mScreenshotDir.empty())
    {
        mActive = true;
    }

    // --- credentials -------------------------------------------------------
    const std::string cred_path =
        settingOrEnv("FSTestCredentialsFile", "SL_VIEWER_CREDENTIALS");
    const std::string avatar_key = gSavedSettings.getString("FSTestAvatarKey");

    std::string login_uri_from_creds;
    if (!cred_path.empty())
    {
        FSTestConfig::Avatar avatar;
        std::string error;
        if (!FSTestConfig::loadCredentials(cred_path, avatar_key, avatar, error))
        {
            // Refuse to start rather than fall through to the login panel and
            // sit there forever in an unattended run.
            return refuse("--credentials: " + error);
        }

        if (!avatar.mMfaCommand.empty())
        {
            // sl-client runs this through sh -c for a TOTP token. Firestorm's
            // MFA flow is a different shape (a token prompt during login), so
            // rather than half-wire it, say so plainly.
            LL_WARNS("FSTestHarness")
                << "credentials avatar '" << avatar.mKey << "' sets mfa_command,"
                << " which this viewer does not yet use; login will fail if the"
                << " account requires MFA" << LL_ENDL;
        }

        LLSD login_info = LLSD::emptyArray();
        login_info.append(avatar.mFirst);
        login_info.append(avatar.mLast);
        login_info.append(avatar.mPassword);
        // This is exactly what --login sets; LLLoginHandler turns it into a
        // credential and flips AutoLogin on. Non-persisting, as the command
        // line parser also does (llcommandlineparser.cpp:612) -- the control
        // is Persist=1, so a plain set would write the test password into the
        // user's settings.xml in clear text.
        forceSetting("UserLoginInfoCmdLine", login_info, /*log_value*/ false);

        login_uri_from_creds = avatar.mLoginUri;
        if (login_uri_from_creds.empty() && !avatar.mGrid.empty())
        {
            forceSetting("CmdLineGridChoice", avatar.mGrid);
        }

        LL_INFOS("FSTestHarness") << "credentials: avatar '" << avatar.mKey
                                  << "' = " << avatar.mFirst << ' ' << avatar.mLast
                                  << LL_ENDL;
        mActive = true;
    }

    // --- grid --------------------------------------------------------------
    const std::string grid_path =
        settingOrEnv("FSTestGridFile", "SL_VIEWER_GRID_FILE");
    std::string login_uri = login_uri_from_creds;
    if (!grid_path.empty())
    {
        FSTestConfig::Grid grid;
        std::string error;
        if (!FSTestConfig::loadGrid(grid_path, grid, error))
        {
            return refuse("--gridfile: " + error);
        }
        // A grid file is the more specific statement, so it wins over the
        // avatar's own login_uri.
        login_uri = grid.mLoginUri;
        mActive = true;
    }

    if (!login_uri.empty())
    {
        // NOTE: --loginuri / CmdLineLoginURI is dead code in this build (the
        // OpenSim grid manager never reads it), so route through
        // CmdLineGridChoice, which fsgridhandler *does* honour: an unknown
        // name is treated as a host and resolved via GET /get_grid_info.
        const std::string grid_name = FSTestConfig::gridNameFromLoginUri(login_uri);
        gSavedSettings.setString("CmdLineGridChoice", grid_name);
        LL_INFOS("FSTestHarness") << "grid: " << grid_name
                                  << " (from " << login_uri << ')' << LL_ENDL;
    }

    // --- capture -----------------------------------------------------------
    // (The screenshot directory was read at the top.)
    mSceneDumpPath = settingOrEnv("FSTestSceneDump", "SL_VIEWER_SCENE_DUMP");
    if (!mSceneDumpPath.empty())
    {
        mActive = true;
    }

    const std::string cam_pos = settingOrEnv("FSTestCameraPosition", "SL_VIEWER_CAMERA_POSITION");
    if (!cam_pos.empty())
    {
        if (parseVec3(cam_pos, mCameraPos))
        {
            mHaveCameraPos = true;
            mActive = true;
        }
        else
        {
            return refuse("--camera-position: expected 'x,y,z', got '" + cam_pos + "'");
        }
    }
    const std::string cam_look = settingOrEnv("FSTestCameraLookAt", "SL_VIEWER_CAMERA_LOOK_AT");
    if (!cam_look.empty())
    {
        if (parseVec3(cam_look, mCameraLookAt))
        {
            mHaveCameraLookAt = true;
            mActive = true;
        }
        else
        {
            return refuse("--camera-look-at: expected 'x,y,z', got '" + cam_look + "'");
        }
    }

    F32 day_position = 0.f;
    if (envF32("SL_VIEWER_SKY_DAY_POSITION", day_position))
    {
        mDayPosition = llclamp(day_position, 0.f, 1.f);
        mHaveDayPosition = true;
        mActive = true;
    }

    // Capture size: "WxH". Both viewers must render the same pixel grid or the
    // images are not comparable at all, so this has a real default
    // (DEFAULT_CAPTURE_WIDTH x ..._HEIGHT) rather than falling back to the
    // window's size -- see mCaptureWidth.
    const std::string capture_size = envString("SL_VIEWER_CAPTURE_SIZE");
    if (!capture_size.empty())
    {
        // Strict, as sl-client's parser is: both numbers, all of each, and
        // nothing else. A value with no 'x' at all used to keep the default
        // size and carry on, so a typo ran at a size nobody asked for.
        bool parsed = false;
        const size_t x = capture_size.find_first_of("xX");
        if (x != std::string::npos)
        {
            const std::string width = capture_size.substr(0, x);
            const std::string height = capture_size.substr(x + 1);
            try
            {
                size_t width_end = 0;
                size_t height_end = 0;
                const long w = std::stol(width, &width_end);
                const long h = std::stol(height, &height_end);
                parsed = width_end == width.size() && height_end == height.size()
                    && w > 0 && h > 0;
                if (parsed)
                {
                    mCaptureWidth  = (S32)w;
                    mCaptureHeight = (S32)h;
                    mActive = true;
                }
            }
            catch (const std::exception&)
            {
                parsed = false;
            }
        }
        if (!parsed)
        {
            return refuse("SL_VIEWER_CAPTURE_SIZE: expected 'WxH', got '" + capture_size + "'");
        }
    }
    if (mCaptureWidth % 4 != 0)
    {
        // rawSnapshot pads the width up to a multiple of four before it scales
        // the grab ("image_width += (image_width * 3) % 4", a BMP row-alignment
        // hack that runs whatever the format), so this frame would come out
        // wider than sl-client's, which is exact. Say so here rather than
        // leaving it for whoever tries to diff the pair.
        LL_WARNS("FSTestHarness") << "a capture width of " << mCaptureWidth
                                  << " is not a multiple of 4; the snapshot path rounds it up, so"
                                     " these frames will not match sl-client's" << LL_ENDL;
    }

    // Which layers of the composited frame the capture holds. Independent of
    // each other and of the size; all off by default (world only).
    if (envBool("SL_VIEWER_CAPTURE_UI", mCaptureUi) && mCaptureUi)
    {
        mActive = true;
    }
    if (envBool("SL_VIEWER_CAPTURE_HUD", mCaptureHud) && mCaptureHud)
    {
        mActive = true;
    }
    if (envBool("SL_VIEWER_CAPTURE_GIZMOS", mCaptureGizmos) && mCaptureGizmos)
    {
        mActive = true;
    }
    // Not a layer of the frame, but read beside them: whether the run may make
    // sound. Off by default. Unlike the layers it does not activate the harness
    // on its own -- sound in an ordinary session is not the harness's business.
    envBool("SL_VIEWER_CAPTURE_AUDIO", mCaptureAudio);

    // The lens, in degrees, so a run states its framing rather than resting on
    // two viewers' defaults agreeing. They do agree -- both default to
    // DEFAULT_FIELD_OF_VIEW (60 degrees) -- but sl-client's used to be 45, and
    // the first cross-check that put both cameras at one pose framed five prims
    // of a fixture row here and three there before anyone noticed the lens.
    F32 fov_degrees = 0.f;
    if (envF32("SL_VIEWER_CAPTURE_FOV", fov_degrees) && fov_degrees > 0.f)
    {
        mHaveFieldOfView = true;
        mFieldOfView = fov_degrees * DEG_TO_RAD;
        LL_INFOS("FSTestHarness") << "field of view pinned to " << fov_degrees
                                  << " degrees" << LL_ENDL;
    }

    // The interface scale, pinned so a chrome pair states the scale both
    // interfaces are drawn at rather than resting on two defaults agreeing.
    // Refused outside sl-client's UiScale range (0.75 to 2), which lies inside
    // this viewer's own: a pin one side would clamp is a pair at two scales.
    const std::string ui_scale = envString("SL_VIEWER_CAPTURE_UI_SCALE");
    if (!ui_scale.empty())
    {
        bool parsed = false;
        try
        {
            size_t end = 0;
            mUiScale = std::stof(ui_scale, &end);
            parsed = end == ui_scale.size() && mUiScale >= 0.75f && mUiScale <= 2.f;
        }
        catch (const std::exception&)
        {
            parsed = false;
        }
        if (!parsed)
        {
            return refuse("SL_VIEWER_CAPTURE_UI_SCALE: expected a number from 0.75 to 2, got '"
                          + ui_scale + "'");
        }
        mHaveUiScale = true;
        LL_INFOS("FSTestHarness") << "UI scale pinned to " << mUiScale << LL_ENDL;
    }

    envF32("SL_VIEWER_SCREENSHOT_DELAY", mSettleTimeout);
    envF32("SL_VIEWER_SCREENSHOT_INTERVAL", mFrameInterval);
    envS32("SL_VIEWER_SCREENSHOT_FRAMES", mFrameCount);
    envF32("SL_VIEWER_LOGIN_TIMEOUT", mLoginTimeout);

    // The skin. This viewer's own --skin has already put its argument in
    // SkinCurrent by now (the command line reaches gSavedSettings at
    // clp.notify(), just above the call to us), so applySkin() falls back to
    // that and only the environment variable is read here.
    //
    // Naming a skin does *not* arm the harness: which skin the viewer wears is
    // a presentation choice, not a run mode, and arming would turn an ordinary
    // session into an unattended one -- closing its floaters and forcing the
    // determinism settings on it. So this sits before the !mActive return
    // rather than after it, and applySkin() decides what to do in each case.
    mSkin = envString("SL_VIEWER_SKIN");
    mSkinTheme = settingOrEnv("FSTestSkinTheme", "SL_VIEWER_THEME");
    if (!applySkin())
    {
        return false;
    }

    if (!mActive)
    {
        return true;
    }

    if (!mScreenshotDir.empty())
    {
        LLFile::mkdir(mScreenshotDir);
    }

    applyWindowIdentity();
    applyDeterminismSettings();
    if (mHaveUiScale)
    {
        // Before the window exists, which is when it reads UIScaleFactor; both
        // are Persist=1, so forced for this run only. The first-run reset is
        // switched off because a fresh user directory is a first run, and on a
        // display whose system UI size is not 1 it would put UIScaleFactor
        // back to 1 over the pin.
        forceSetting("ResetUIScaleOnFirstRun", LLSD(false));
        forceSetting("UIScaleFactor", mUiScale);
    }

    mState = STATE_WAIT_LOGIN;
    mResultReason = "login not completed";
    mStateTimer.reset();

    LL_INFOS("FSTestHarness")
        << "armed: screenshots='" << mScreenshotDir
        << "' frames=" << mFrameCount
        << " interval=" << mFrameInterval
        << "s settle-timeout=" << mSettleTimeout
        << "s login-timeout=" << mLoginTimeout << 's' << LL_ENDL;
    return true;
}

bool FSTestHarness::refuse(const std::string& reason)
{
    LL_WARNS("FSTestHarness") << "refusing the run: " << reason << LL_ENDL;

    mState = STATE_DONE;
    mResultOk = false;
    mResultReason = reason;

    // A refusal can come before the options the status reports on were read.
    // Read them here, so that a UI run or a pinned sun gets its block saying
    // the run never reached that point. Without the block, the driver takes
    // the silence for a viewer too old to write it.
    envBool("SL_VIEWER_CAPTURE_UI", mCaptureUi);
    F32 day_position = 0.f;
    if (!mHaveDayPosition && envF32("SL_VIEWER_SKY_DAY_POSITION", day_position))
    {
        mDayPosition = llclamp(day_position, 0.f, 1.f);
        mHaveDayPosition = true;
    }
    if (mCaptureUi && mWindowSizeDetail.empty())
    {
        // Otherwise the status would carry writeStatus()'s default, "the
        // window was the size the run asked for", about a window that never
        // existed.
        mWindowSizeDetail = "the run was refused before the window was made";
    }
    if (!mScreenshotDir.empty())
    {
        LLFile::mkdir(mScreenshotDir);
        writeStatus();
    }
    return false;
}

void FSTestHarness::forceSetting(const char* name, const LLSD& value, bool log_value)
{
    LLControlVariable* control = gSavedSettings.getControl(name);
    if (!control)
    {
        // Not every control exists in every build flavour; a missing one is
        // simply not applicable here, not an error.
        LL_DEBUGS("FSTestHarness") << "no such setting '" << name
                                   << "'; skipped" << LL_ENDL;
        return;
    }
    // The `false` is the whole point: most of these are Persist=1, so a plain
    // gSavedSettings.setBOOL() would write them into the user's settings.xml
    // on exit and permanently disable their voice, notifications or RLV. This
    // is the same non-persisting path --set uses (tempSetControl).
    control->setValue(value, false);

    if (log_value)
    {
        LL_INFOS("FSTestHarness") << "  forced " << name << " = " << value << LL_ENDL;
    }
    else
    {
        LL_INFOS("FSTestHarness") << "  forced " << name << " = <not logged>" << LL_ENDL;
    }
}

void FSTestHarness::applyWindowIdentity()
{
    // Give a harness run a window identity of its own, so a compositor can
    // route it somewhere other than the production viewer -- a niri/sway
    // window rule keying on app-id, for instance. Without this, every viewer
    // on the machine shares the app-id SDL derives from argv[0] and a rule
    // cannot tell a throwaway test window from the session someone is using.
    //
    // SDL reads all three of these at video init, which is in initWindow(),
    // well after initConfiguration() where we are now. Passing overwrite=0
    // leaves an explicitly-exported value alone, so the caller stays in
    // charge; SL_VIEWER_APP_ID picks the name without having to know which of
    // the three SDL will consult on a given display server.
    std::string app_id = envString("SL_VIEWER_APP_ID");
    if (app_id.empty())
    {
        app_id = "firestorm-test";
    }

    // X11/XWayland uses the first, native Wayland the second; SDL_APP_NAME is
    // the fallback both consult and is also what shows up in volume applets
    // and screensaver-inhibitor lists.
    setenv("SDL_VIDEO_X11_WMCLASS", app_id.c_str(), 0);
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", app_id.c_str(), 0);
    setenv("SDL_APP_NAME", app_id.c_str(), 0);

    LL_INFOS("FSTestHarness") << "window app-id: " << app_id
                              << " (override with SL_VIEWER_APP_ID)" << LL_ENDL;
}

bool FSTestHarness::applySkin()
{
    // This viewer's own --skin lands in SkinCurrent, where the user
    // directory's saved choice also lives -- but not at the same level. The
    // command line applies map-to with saved_value=false
    // (llcommandlineparser.cpp), and an unsaved value is pushed *above* the
    // saved one on the control's stack, where it cannot affect
    // getSaveValue(). So the two differing is exactly "something overrode this
    // for this run", which is the signal there is otherwise no way to read.
    const LLPointer<LLControlVariable> skin_control = gSavedSettings.getControl("SkinCurrent");
    const bool skin_overridden =
        skin_control.notNull() &&
        skin_control->getValue().asString() != skin_control->getSaveValue().asString();

    // A skin was named if any of the three routes named one.
    const bool skin_named = !mSkin.empty() || skin_overridden;
    const bool asked = skin_named || !mSkinTheme.empty();
    if (!asked && !mActive)
    {
        // Nobody named a skin and there is no run to keep coherent. Leave the
        // viewer's own settings entirely alone -- this is every ordinary
        // interactive session.
        return true;
    }

    // An armed run with no skin named still comes through here, to reconcile
    // the three pairs against whatever SkinCurrent holds -- which covers both
    // --skin (folder set, readable names left saying something else) and a
    // user directory whose pairs have drifted apart. A run that exists to
    // compare skins must not be the thing that discovers the mismatch.
    const std::string current_skin = gSavedSettings.getString("SkinCurrent");
    const std::string current_theme = gSavedSettings.getString("SkinCurrentTheme");
    const std::string wanted_skin = mSkin.empty() ? current_skin : mSkin;

    std::vector<SkinEntry> skins;
    std::string error;
    if (!loadSkinCatalogue(skins, error))
    {
        if (asked)
        {
            return refuse("cannot honour the requested skin: " + error);
        }
        LL_WARNS("FSTestHarness") << "skin catalogue: " << error
                                  << "; leaving the skin settings as they are" << LL_ENDL;
        return true;
    }

    const SkinEntry* skin = nullptr;
    for (const SkinEntry& candidate : skins)
    {
        // Either spelling: the folder files load from, or the name the viewer
        // shows. A caller who read the name off the preferences panel should
        // not have to learn the folder it corresponds to.
        if (sameName(candidate.mFolder, wanted_skin) || sameName(candidate.mName, wanted_skin))
        {
            skin = &candidate;
            break;
        }
    }
    if (!skin)
    {
        if (asked)
        {
            // Refuse to start. Falling through would dress the run in the
            // default skin while the operator believes they asked for another
            // one -- the silent wrong answer this option exists to prevent.
            return refuse("no such skin '" + wanted_skin + "'; available: " + skinChoices(skins));
        }
        LL_WARNS("FSTestHarness") << "the current skin '" << wanted_skin
                                  << "' is not listed in skins.xml; leaving the skin"
                                     " settings as they are" << LL_ENDL;
        return true;
    }

    const std::pair<std::string, std::string>* theme = nullptr;
    if (!mSkinTheme.empty())
    {
        for (const auto& candidate : skin->mThemes)
        {
            if (sameName(candidate.first, mSkinTheme) || sameName(candidate.second, mSkinTheme))
            {
                theme = &candidate;
                break;
            }
        }
        if (!theme)
        {
            return refuse("skin '" + skin->mName + "' has no theme '" + mSkinTheme
                          + "'; available: " + themeChoices(*skin));
        }
    }
    else if (skin_named)
    {
        // A skin named without a theme takes its own first one, which is the
        // default skins.xml lists it under. Keeping the *outgoing* skin's
        // theme folder instead would point the theme search at a
        // skins/<new>/themes/<old> that does not exist, and would leave the
        // readable name naming a theme from a skin no longer worn.
        if (!skin->mThemes.empty())
        {
            theme = &skin->mThemes.front();
        }
    }
    else
    {
        // Reconciling, not changing: keep the theme already chosen, and only
        // work out what it is called. A folder that is not one of this skin's
        // themes has already drifted, so fall back to the skin's first.
        for (const auto& candidate : skin->mThemes)
        {
            if (candidate.first == current_theme)
            {
                theme = &candidate;
                break;
            }
        }
        if (!theme && !skin->mThemes.empty())
        {
            theme = &skin->mThemes.front();
        }
    }

    const std::string theme_folder = theme ? theme->first : std::string();
    const std::string theme_name = theme ? theme->second : std::string();

    LL_INFOS("FSTestHarness") << "skin: " << skin->mName << " / "
                              << (theme_name.empty() ? "(no theme)" : theme_name)
                              << " -- folders " << skin->mFolder << " / "
                              << (theme_folder.empty() ? "(base)" : theme_folder) << LL_ENDL;

    // All four together, and non-persisting: every one of them is Persist=1,
    // so a plain set would repaint the operator's own viewer on exit. The
    // folders decide which files load; the readable names decide which code
    // paths run, because llstartup copies them into the FSInternalSkin* pair
    // that fscommon and llviewermenu test. LLAppViewer::initConfiguration
    // reads SkinCurrent a little further down the same function that called
    // us, so this is in time for the skin the run actually loads.
    forceSetting("SkinCurrent", skin->mFolder);
    forceSetting("SkinCurrentTheme", theme_folder);
    forceSetting("FSSkinCurrentReadableName", skin->mName);
    forceSetting("FSSkinCurrentThemeReadableName", theme_name);

    return true;
}

void FSTestHarness::applyDeterminismSettings()
{
    // Every value here is one that would otherwise differ between two runs or
    // two viewers for reasons that have nothing to do with rendering. Each is
    // logged, so a comparison run is self-describing.
    LL_INFOS("FSTestHarness") << "applying determinism settings:" << LL_ENDL;

    // Camera must not drift or ease towards its target.
    forceSetting("CameraPositionSmoothing", 0.f);

    // The login panel must never appear in an unattended run.
    forceSetting("AutoLogin", true);
    // RLVa forces the login panel back on (llstartup.cpp:1227), which silently
    // breaks autologin. That guard is not a safety check on autologin: RLVa can
    // only latch on before STATE_LOGIN_CLEANUP (RlvHandler::canEnable), so the
    // login screen is the last place the setting can still be toggled without a
    // restart, and forcing the panel keeps that toggle reachable. A harness run
    // does not use RLVa, so declining it costs nothing -- but note this is the
    // one override here that changes viewer *behaviour* rather than
    // presentation. Comparing RLV-restricted rendering would mean dropping it,
    // and then autologin is not available either.
    forceSetting("RestrainedLove", false);
    // Do not persist test passwords into the credential store.
    forceSetting("FSLoginDontSavePassword", true);
    // Exit instead of falling back to the login panel when login fails.
    forceSetting("QuitOnLoginActivated", true);
    // Do not block startup on the phoenixviewer.com grid list; a fake grid run
    // should touch nothing but the fake grid.
    forceSetting("GridListDownload", false);

    // Nothing may pop up over the scene.
    forceSetting("IgnoreAllNotifications", true);

    // Snapshots hold the layers this run asked for. captureFrame passes
    // show_ui / show_hud to saveSnapshot directly; these are the same choice
    // stated in the settings the rest of the snapshot machinery reads, so the
    // two cannot disagree.
    forceSetting("RenderUIInSnapshot", mCaptureUi);
    forceSetting("RenderHUDInSnapshot", mCaptureHud);
    // Never the balance: it is account state, not rendering, and it differs
    // between the two viewers' test accounts for reasons no comparison cares
    // about.
    forceSetting("RenderBalanceInSnapshot", false);
    forceSetting("HighResSnapshot", false);

    // The editor overlays that draw into the *world* pass rather than the UI
    // layer -- selection silhouettes, highlights and beacons -- are this
    // viewer's answer to sl-client's gizmo overlay, and survive show_ui =
    // false. Forced off unless the run asked for them, so a stray selection or
    // a saved beacon setting cannot put a coloured line through one viewer's
    // frames and not the other's. When they *are* asked for, nothing is forced:
    // the run wants what the viewer would draw.
    if (!mCaptureGizmos)
    {
        forceSetting("RenderHighlightSelections", false);
        forceSetting("renderhighlights", false);
        forceSetting("renderbeacons", false);
        forceSetting("scriptsbeacon", false);
        forceSetting("scripttouchbeacon", false);
        forceSetting("physicalbeacon", false);
        forceSetting("soundsbeacon", false);
        forceSetting("particlesbeacon", false);
        LLPipeline::sRenderHighlight = false;
    }

    // Audio and voice are pure noise in a screenshot run, and voice in
    // particular spawns a helper process per instance.
    forceSetting("EnableVoiceChat", false);
    // A fixture scene's sound sources loop for the whole run, through the
    // speakers of whoever is sitting at the machine. Muted unless the run asked
    // for sound -- non-persistently, like every setting here, so the operator's
    // own mute state survives the run.
    if (!mCaptureAudio)
    {
        forceSetting("MuteAudio", true);
    }

    // The avatar must hold still, and in particular must not pose itself from
    // where the *camera* happens to be. These three are the ways it otherwise
    // moves without anyone asking it to:
    //
    // - The look-at animation turns the agent's own head -- and with it the
    //   neck and shoulders -- towards the camera's focus. With the camera
    //   forced somewhere by --camera-position/--camera-look-at, that reads in
    //   the frame as a crouched, craned avatar, and a comparison then measures
    //   this viewer's head-tracking against the other viewer's camera rather
    //   than anything either of them rendered.
    // - The typing animation would start if anything put text in the chat bar.
    // - Going AFK swaps the whole body into the away pose. The shipped default
    //   is already 0 (never), but a run must not depend on the operator's
    //   saved value for whether its avatar is standing or slumped.
    forceSetting("DisableLookAtAnimation", true);
    forceSetting("PlayTypingAnim", false);
    forceSetting("AFKTimeout", 0);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void FSTestHarness::tick()
{
    if (!mActive)
    {
        return;
    }

    switch (mState)
    {
        case STATE_WAIT_LOGIN: tickWaitLogin(); break;
        case STATE_SETTLE:     tickSettle();    break;
        case STATE_CAPTURE:    tickCapture();   break;
        case STATE_SHUTDOWN:   tickShutdown();  break;
        case STATE_HOLD:
            // Keep the requested pose asserted; the agent camera pulls back
            // towards the avatar the moment we stop.
            applyCamera();
            break;
        case STATE_INACTIVE:
        case STATE_DONE:
        default:
            break;
    }
}

bool FSTestHarness::captureRequested() const
{
    return !mScreenshotDir.empty() || !mSceneDumpPath.empty();
}

void FSTestHarness::tickWaitLogin()
{
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        // Only an unattended capture run gives up on its own. When a person is
        // driving, a slow login is theirs to wait out or abandon.
        if (captureRequested() && mStateTimer.getElapsedTimeF32() > mLoginTimeout)
        {
            finish(false, "login timed out in startup state "
                          + LLStartUp::getStartupStateString());
        }
        return;
    }

    LL_INFOS("FSTestHarness") << "in world after "
                              << mStateTimer.getElapsedTimeF32() << "s" << LL_ENDL;

    applyWindowSize();
    closeFloaters();

    mState = STATE_SETTLE;
    mResultReason = "scene never settled";
    mStateTimer.reset();
    mQuiescence.reset();
}

void FSTestHarness::tickSettle()
{
    // The environment and camera are applied as soon as we are in world, so
    // the scene settles under the same lighting it will be photographed in.
    applyEnvironment();
    applyCamera();

    const F32 elapsed = mStateTimer.getElapsedTimeF32();
    const FSTestQuiescence::Counts counts = FSTestQuiescence::sample();
    const bool quiet_enough = mQuiescence.update(counts, QUIET_HOLD_FRAMES);

    // Both conditions, as sl-client does: a scene that has not been in world
    // for MIN_SETTLE_SECS has not had time to ask for its content yet, so an
    // early all-zero reading is not "settled", it is "has not started".
    if (elapsed >= MIN_SETTLE_SECS && quiet_enough)
    {
        LL_INFOS("FSTestHarness") << "scene settled after " << elapsed << "s"
                                  << LL_ENDL;
    }
    else if (elapsed >= mSettleTimeout)
    {
        // Capture anyway, but say what was still outstanding -- a frame taken
        // mid-load is worth having as long as nobody mistakes it for settled.
        LL_WARNS("FSTestHarness")
            << "scene did not settle within " << mSettleTimeout
            << "s; capturing anyway with outstanding work: "
            << counts.describe() << LL_ENDL;
    }
    else
    {
        return;
    }

    if (!captureRequested())
    {
        // Camera and environment flags on their own are a "set the scene up
        // and leave it alone" request -- e.g. positioning the camera by hand
        // before looking at something. Quitting here would be a nasty
        // surprise; only an explicit --screenshot-dir / --scene-dump asks for
        // an unattended run that ends by itself.
        LL_INFOS("FSTestHarness")
            << "no --screenshot-dir or --scene-dump given; scene set up, "
            << "leaving the viewer running" << LL_ENDL;
        mState = STATE_HOLD;
        return;
    }

    // Before the first frame, and only now: the resize asked for at the top of
    // the settle is a round trip, and this is the first moment its answer is
    // certain to have arrived.
    verifyWindowSize();

    mState = STATE_CAPTURE;
    mResultReason = "capture incomplete";
    mStateTimer.reset();
    mFrameTimer.reset();
}

void FSTestHarness::tickCapture()
{
    // The first frame goes out as soon as the scene settled; waiting a full
    // interval first would only let it drift away from the settled state.
    if (mFramesWritten > 0 && mFrameTimer.getElapsedTimeF32() < mFrameInterval)
    {
        return;
    }
    mFrameTimer.reset();

    // The agent camera drifts back towards the avatar, so the pose is
    // re-asserted for every frame rather than set once.
    applyCamera();

    if (!captureFrame(mFramesWritten))
    {
        finish(false, "snapshot failed at frame "
                      + std::to_string(mFramesWritten));
        return;
    }
    ++mFramesWritten;

    if (mFramesWritten >= mFrameCount)
    {
        writeSceneDump();
        finish(true, "complete");
    }
}

void FSTestHarness::tickShutdown()
{
    if (mStateTimer.getElapsedTimeF32() < LOGOUT_GRACE_SECS)
    {
        return;
    }

    // requestQuit() should have sent a LogoutRequest and be waiting for the
    // reply. If we are still here, escalate -- but to fastQuit(), which still
    // tells the simulator we are going, not to forceQuit(), which does not.
    // A session the sim thinks is still logged in makes the *next* run fail
    // for reasons that have nothing to do with what we are comparing.
    LL_WARNS("FSTestHarness")
        << "logout did not complete within " << LOGOUT_GRACE_SECS
        << "s; sending a final logout and exiting" << LL_ENDL;

    mState = STATE_DONE;
    LLAppViewer::instance()->fastQuit(mResultOk ? 0 : 1);
}

void FSTestHarness::finish(bool ok, const std::string& reason)
{
    mResultOk = ok;
    mResultReason = reason;
    // The frames exist; they are just not frames of the scene that was asked
    // for. Reporting that as a success invites a person to compare two viewers'
    // skies when neither of them was told which sky to draw.
    if (ok && mHaveDayPosition && !mDayPositionHonoured)
    {
        std::ostringstream why;
        why << "the run asked for day position " << mDayPosition << " and did not get it: "
            << (mDayPositionDetail.empty()
                    ? std::string("the environment was never applied")
                    : mDayPositionDetail);
        mResultOk = false;
        mResultReason = why.str();
    }
    // Same shape, same reason: the frames exist and are not of what was asked
    // for. A UI capture from a window of the wrong size is the window's UI in a
    // buffer of another size -- letterboxed, or the same grab stitched across
    // it -- and reporting that as a success invites a person to compare two
    // viewers' interfaces when one of them was never drawn at the size in the
    // file.
    if (mResultOk && mCaptureUi && !mWindowSizeHonoured)
    {
        mResultOk = false;
        mResultReason = mWindowSizeDetail.empty()
            ? std::string("the window was never checked against the capture size")
            : mWindowSizeDetail;
    }
    writeStatus();

    if (ok)
    {
        LL_INFOS("FSTestHarness") << "run complete: " << reason
                                  << " (" << mFramesWritten << " frames)" << LL_ENDL;
    }
    else
    {
        LL_WARNS("FSTestHarness") << "run failed: " << reason
                                  << " (" << mFramesWritten << " frames)" << LL_ENDL;
    }

    mState = STATE_SHUTDOWN;
    mStateTimer.reset();

    // The kind, gentle quit: sends LogoutRequest and waits for LogoutReply.
    LLAppViewer::instance()->requestQuit();
}

// ---------------------------------------------------------------------------
// Scene control
// ---------------------------------------------------------------------------

void FSTestHarness::applyWindowSize()
{
    if (mCaptureWidth <= 0 || mCaptureHeight <= 0 || !gViewerWindow)
    {
        return;
    }
    if (!mCaptureUi)
    {
        // A world-only capture renders into its own scratch target at the
        // pinned size, so the window's size does not enter the frame at all.
        // Shrinking it would only make the run harder to watch.
        return;
    }

    LLWindow* window = gViewerWindow->getWindow();
    if (!window)
    {
        return;
    }

    // ASK THE WINDOW SYSTEM, not LLViewerWindow.
    //
    // LLViewerWindow::reshape() is the *inbound* notification -- what the
    // window system calls when the window has already changed -- and it only
    // updates mWindowRectRaw and re-lays the UI. It never touches mWindow, so
    // calling it here did not resize anything: it told the viewer's internals a
    // size the actual window did not have. The UI then laid itself out for a
    // window that did not exist while the snapshot grabbed the one that did,
    // which is how a run came back with 1024x738 of content sitting in a
    // 1920x1080 frame, and the same grab stitched twice by the second frame.
    //
    // LLWindow::setSize is the request. It may still be refused -- a tiling
    // Wayland compositor sizes its own windows, and an X11 client reaching it
    // through Xwayland has no way to insist -- which is what verifyWindowSize()
    // is for.
    if (!window->setSize(LLCoordWindow(mCaptureWidth, mCaptureHeight)))
    {
        LL_WARNS("FSTestHarness") << "the window system refused a resize to "
                                  << mCaptureWidth << 'x' << mCaptureHeight << LL_ENDL;
    }
    LL_INFOS("FSTestHarness") << "asked the window system for "
                              << mCaptureWidth << 'x' << mCaptureHeight
                              << "; checking what it gave us before the first frame" << LL_ENDL;
}

void FSTestHarness::verifyWindowSize()
{
    if (!mCaptureUi || mWindowSizeChecked)
    {
        // Only a UI capture depends on the window's size; a world-only frame is
        // rendered into a scratch target of its own.
        return;
    }
    mWindowSizeChecked = true;

    LLWindow* window = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
    if (!window)
    {
        return;
    }

    // Deliberately asked *here* rather than straight after setSize(): a resize
    // is a round trip through the window system, and the answer arrives on a
    // later frame. The settle period is several seconds of them, so by the
    // first captured frame the window is whatever it is going to be.
    LLCoordWindow actual;
    if (!window->getSize(&actual))
    {
        mWindowSizeDetail = "the window system would not say how big the window is";
        return;
    }

    if (actual.mX == mCaptureWidth && actual.mY == mCaptureHeight)
    {
        mWindowSizeHonoured = true;
        LL_INFOS("FSTestHarness") << "window is " << actual.mX << 'x' << actual.mY
                                  << ", as asked" << LL_ENDL;
        return;
    }

    // Not a degraded comparison -- not a comparison. The reference's snapshot
    // path cannot draw the UI at any size but the window's, so a frame captured
    // at a size the window does not have holds the window's UI in a buffer of
    // another size: letterboxed, or the same grab stitched across it. Either
    // way it cannot be compared with sl-client's, which lays its UI out at the
    // capture size. Carried out through harness-status.json, because a
    // directory of plausible-looking PNGs is indistinguishable from a good run
    // by looking at it.
    std::ostringstream why;
    why << "the window is " << actual.mX << 'x' << actual.mY << " and the run asked for "
        << mCaptureWidth << 'x' << mCaptureHeight
        << "; the window manager sizes its own windows. A UI capture from a window of"
           " another size is not comparable -- float this window (its app-id is the"
           " SL_VIEWER_APP_ID one) or capture at the window's size.";
    mWindowSizeDetail = why.str();
    LL_WARNS("FSTestHarness") << mWindowSizeDetail << LL_ENDL;
}

void FSTestHarness::closeFloaters()
{
    // Firestorm restores its docked floaters on login -- Conversations
    // (floater_im_box) among them -- and they sit over the 3D view.
    //
    // The captured frames do not contain them: captureFrame passes
    // show_ui = false, so the snapshot re-renders the world with no UI at
    // all. This is therefore not about the comparison output. It is about
    // (a) being able to watch a run and see the scene rather than a stack of
    // panels, and (b) the floaters that *do* reach into the 3D render --
    // the build tools draw selection outlines and beacons into the world,
    // not into the UI layer, so those would survive show_ui = false and land
    // in the frames.
    //
    // Close what login opened, then stop anything else opening for the rest
    // of the run: a notification or an inventory offer arriving mid-capture
    // would otherwise pop a floater between two frames of one sequence.
    if (gFloaterView)
    {
        gFloaterView->closeAllChildren(/*app_quitting*/ false);
    }
    LLFloaterReg::hideVisibleInstances();
    LLFloaterReg::blockShowFloaters(true);

    LL_INFOS("FSTestHarness") << "closed open floaters and blocked new ones"
                              << LL_ENDL;
}

void FSTestHarness::applyCamera()
{
    if (!mHaveCameraPos)
    {
        return;
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    const LLVector3d pos_global = region->getPosGlobalFromRegion(mCameraPos);
    const LLVector3d focus_global = mHaveCameraLookAt
        ? region->getPosGlobalFromRegion(mCameraLookAt)
        : gAgent.getPositionGlobal();

    if (!mCameraApplied)
    {
        // A camera further from the agent than the draw distance is silently
        // refused, so make sure the draw distance covers the shot.
        const F32 needed = (F32)(pos_global - gAgent.getPositionGlobal()).magVec();
        const F32 far_clip = gSavedSettings.getF32("RenderFarClip");
        if (needed > far_clip)
        {
            const F32 widened = llmin(needed * 1.1f, 512.f);
            LL_INFOS("FSTestHarness")
                << "camera is " << needed << "m from the avatar but RenderFarClip is "
                << far_clip << "m; raising it to " << widened << "m" << LL_ENDL;
            // Non-persisting: RenderFarClip is Persist=1 and this must not
            // outlive the run.
            forceSetting("RenderFarClip", widened);
        }
        gAgentCamera.unlockView();
        gAgentCamera.setAnimationDuration(0.f);
        mCameraApplied = true;
    }

    // Detach from the avatar and place the camera outright. Without
    // stopCameraAnimation() this lerps in over up to a second, which would
    // make the first frames of a run differ from the last for no good reason.
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(pos_global, focus_global, LLUUID::null);
    gAgentCamera.stopCameraAnimation();

    // The lens, re-asserted with the pose: setDefaultFOV clamps to the
    // aspect-dependent bounds itself (LLCamera::getMinView / getMaxView), and
    // the setting is forced non-persistently because CameraAngle is Persist=1
    // and a harness run must not edit the user's preferences.
    if (mHaveFieldOfView)
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        if (camera && fabsf(camera->getDefaultFOV() - mFieldOfView) > 1e-4f)
        {
            forceSetting("CameraAngle", mFieldOfView);
            camera->setDefaultFOV(mFieldOfView);
        }
    }
}

void FSTestHarness::applyEnvironment()
{
    if (!mHaveDayPosition || mEnvironmentApplied)
    {
        return;
    }

    LLEnvironment& env = LLEnvironment::instance();

    LLSettingsDay::ptr_t day = env.getEnvironmentDay(LLEnvironment::ENV_REGION);
    if (!day)
    {
        day = env.getEnvironmentDay(LLEnvironment::ENV_LOCAL);
    }
    if (!day)
    {
        // The region's day cycle may not have arrived yet; try again next frame
        // rather than pinning the sky to a default that is not this region's.
        // A cycle that never arrives is caught by the settle timeout, and
        // reported below as a pin that could not be honoured.
        return;
    }

    // TRACK_GROUND_LEVEL (1) is the ground-level sky track; 0 is water.
    const LLSettingsDay::CycleTrack_t& track =
        day->getCycleTrackConst(LLSettingsDay::TRACK_GROUND_LEVEL);

    // A position is a point *along* the track, not a name for one of its
    // keyframes. getSkyAtKeyframe() answers the second question -- it looks the
    // position up in the track's map and finds nothing unless a frame sits at
    // exactly that key -- so it silently declined every position but the ones a
    // cycle happens to be keyed at, which for a one-keyframe cycle is 0.0 and
    // nothing else. Blending the bounding pair is what a running viewer does
    // every frame anyway, and is what the RLV @setenv_daytime path already does
    // with this same blender.
    if (track.size() < 2)
    {
        // One frame (or none) renders the same sky at every position, so there
        // is no sun to choose here and nothing this viewer can do about it: the
        // cycle is the region's. Say so, loudly and in the status file -- a
        // capture whose lighting was not the lighting that was asked for is not
        // a capture of the requested scene, and it looks exactly like one.
        std::ostringstream detail;
        detail << "the region's day cycle schedules " << track.size()
               << " sky frame(s), so every day position renders the same sky";
        mDayPositionDetail = detail.str();
        mDayPositionHonoured = false;
        mEnvironmentApplied = true;
        LL_WARNS("FSTestHarness") << "day position " << mDayPosition
                                  << " not honoured: " << mDayPositionDetail << LL_ENDL;
        return;
    }

    LLSettingsSky::ptr_t sky = LLSettingsVOSky::buildDefaultSky();
    auto blender = std::make_shared<LLTrackBlenderLoopingManual>(
        sky, day, LLSettingsDay::TRACK_GROUND_LEVEL);
    blender->setPosition(mDayPosition);

    // A fixed sky, not a running cycle: the sun must not move between the
    // first captured frame and the last.
    env.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT, true);

    std::ostringstream detail;
    detail << "blended the region's day cycle (" << track.size()
           << " sky keyframes) at " << mDayPosition;
    mDayPositionDetail = detail.str();
    mDayPositionHonoured = true;
    mEnvironmentApplied = true;
    LL_INFOS("FSTestHarness") << "sky pinned at day position " << mDayPosition
                              << ": " << mDayPositionDetail << LL_ENDL;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

bool FSTestHarness::captureFrame(S32 index)
{
    if (mScreenshotDir.empty())
    {
        return true; // nothing asked for; not an error
    }
    if (!gViewerWindow)
    {
        return false;
    }

    // frame_000.png ... matching sl-client's naming, so one comparison tool
    // reads both viewers' output without special-casing either.
    const std::string filename =
        gDirUtilp->add(mScreenshotDir, llformat("frame_%03d.png", index));

    // The pinned capture size, NOT the window's. The two are deliberately
    // independent: applyWindowSize's reshape() is only a *request* to the window
    // manager, and a tiling compositor answers it with a configure event
    // carrying its own size -- which arrives through SDL as an ordinary resize
    // and lands in LLViewerWindow::reshape, overwriting ours. Observed mid-run,
    // between frame_000 and frame_001 of one capture sequence, when the window
    // lost focus. Sizing the snapshot from the window therefore means the window
    // manager picks the resolution, and can change it partway through a
    // sequence. saveSnapshot takes an explicit size and honours it -- with the
    // one caveat that a UI capture is clamped to the window and scaled to fit
    // (llviewerwindow.cpp: "Scaling of the UI is currently *not* supported"),
    // which is why applyWindowSize asks for a matching window in that case and
    // warns when it does not get one.
    const S32 width  = mCaptureWidth;
    const S32 height = mCaptureHeight;

    // Explicit PNG: saveSnapshot defaults to BMP regardless of the extension.
    // The layers are this run's independent choices; the balance never, being
    // account state rather than rendering.
    const bool ok = gViewerWindow->saveSnapshot(filename, width, height,
                                                /*show_ui*/   mCaptureUi,
                                                /*show_hud*/  mCaptureHud,
                                                /*do_rebuild*/false,
                                                /*show_balance*/ false,
                                                LLSnapshotModel::SNAPSHOT_TYPE_COLOR,
                                                LLSnapshotModel::SNAPSHOT_FORMAT_PNG);
    if (!ok)
    {
        LL_WARNS("FSTestHarness") << "snapshot failed: " << filename << LL_ENDL;
    }
    return ok;
}

void FSTestHarness::writeSceneDump()
{
    std::string path = mSceneDumpPath;
    if (path.empty())
    {
        if (mScreenshotDir.empty())
        {
            return;
        }
        path = gDirUtilp->add(mScreenshotDir, "scene.json");
    }

    const LLSD dump = FSTestSceneDump::build();
    std::string error;
    if (!FSTestSceneDump::writeJson(dump, path, error))
    {
        LL_WARNS("FSTestHarness") << "scene dump: " << error << LL_ENDL;
        return;
    }
    LL_INFOS("FSTestHarness") << "scene dump written to " << path << LL_ENDL;
}

void FSTestHarness::writeStatus() const
{
    if (mScreenshotDir.empty())
    {
        return;
    }

    // The driving harness reads this instead of an exit status: the viewer's
    // shutdown path does not carry one out reliably, and "did the run happen"
    // must be distinguishable from "the viewers differ".
    LLSD status = LLSD::emptyMap();
    status["ok"]             = mResultOk;
    status["reason"]         = mResultReason;
    status["frames_written"] = mFramesWritten;
    status["frames_expected"] = mFrameCount;
    status["viewer"]         = "firestorm";
    // Only when a sun was pinned -- and then always, including when it could
    // not be honoured, which is the case the key exists for. sl-client writes
    // the same three sub-keys.
    if (mHaveDayPosition)
    {
        LLSD pinned = LLSD::emptyMap();
        pinned["requested"] = mDayPosition;
        pinned["honoured"]  = mDayPositionHonoured;
        pinned["detail"]    = mDayPositionDetail.empty()
            ? std::string("the run ended before the environment was applied")
            : mDayPositionDetail;
        status["day_position"] = pinned;
    }
    // Only for a UI capture, and then always -- including when the window
    // declined, which is the case the key exists for.
    if (mCaptureUi)
    {
        LLSD window = LLSD::emptyMap();
        window["requested"] = llformat("%dx%d", mCaptureWidth, mCaptureHeight);
        window["honoured"]  = mWindowSizeHonoured;
        window["detail"]    = mWindowSizeDetail.empty()
            ? std::string("the window was the size the run asked for")
            : mWindowSizeDetail;
        status["window_size"] = window;
    }

    const std::string path = gDirUtilp->add(mScreenshotDir, "harness-status.json");
    std::string error;
    if (!FSTestSceneDump::writeJson(status, path, error))
    {
        LL_WARNS("FSTestHarness") << "status file: " << error << LL_ENDL;
    }
}
