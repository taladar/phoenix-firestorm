/**
 * @file fstestharness.h
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

#ifndef FS_TESTHARNESS_H
#define FS_TESTHARNESS_H

#include <string>

#include "stdtypes.h"
#include "llsd.h"
#include "llsingleton.h"
#include "lltimer.h"
#include "v3math.h"

#include "fstestquiesce.h"

/**
 * Drives the viewer through login -> settle -> capture -> logout with no user
 * present, so an external harness can run this viewer and the Rust sl-client
 * viewer against the same (fake) grid and compare what they drew.
 *
 * Inert unless switched on: with none of the --credentials / --screenshot-dir
 * / --camera-* options (or their SL_VIEWER_* environment equivalents) given,
 * initFromCommandLine() leaves the harness inactive and tick() returns
 * immediately, so an ordinary interactive session is unaffected.
 *
 * The option and environment-variable names deliberately mirror sl-client's,
 * so one env block can drive both viewers.
 *
 * Note this is emphatically *not* --noninteractive: that mode skips rendering
 * entirely (llviewerdisplay.cpp) and so can never produce a screenshot.
 */
class FSTestHarness : public LLSingleton<FSTestHarness>
{
    LLSINGLETON(FSTestHarness);

public:
    /**
     * Read the config files and options and, if any are present, arm the
     * harness. Call from LLAppViewer::initConfiguration() *after* clp.notify(),
     * because that is when command-line values reach gSavedSettings.
     */
    void initFromCommandLine();

    /** True once initFromCommandLine() found something to do. */
    bool isActive() const { return mActive; }

    /** Advance the state machine. Call once per frame from the main loop. */
    void tick();

private:
    enum EState
    {
        STATE_INACTIVE = 0, ///< not armed; tick() does nothing
        STATE_WAIT_LOGIN,   ///< waiting for LLStartUp to reach STATE_STARTED
        STATE_SETTLE,       ///< in-world, waiting for the scene to stop loading
        STATE_CAPTURE,      ///< writing frames
        STATE_SHUTDOWN,     ///< logout requested, waiting for it to complete
        STATE_HOLD,         ///< scene set up, camera held, viewer left running
        STATE_DONE          ///< nothing further to do
    };

    /**
     * True when an unattended run was asked for -- i.e. one that ends by
     * itself. Camera and environment flags alone only set the scene up.
     */
    bool captureRequested() const;

    void tickWaitLogin();
    void tickSettle();
    void tickCapture();
    void tickShutdown();

    /// Put the camera exactly where it was asked to be. Re-applied per frame.
    /// Closes the floaters login restored and blocks new ones for the run.
    void closeFloaters();
    void applyCamera();
    /// Pin the sun, so a run does not drift between its first and last frame.
    void applyEnvironment();
    /**
     * Give the window an app-id distinct from the production viewer's, so a
     * compositor rule can route harness runs to their own workspace. Defaults
     * to "firestorm-test"; SL_VIEWER_APP_ID overrides.
     */
    void applyWindowIdentity();
    /// Force the settings two viewers must agree on to be comparable at all.
    void applyDeterminismSettings();
    /**
     * Set a setting for this run only. Most of the controls the harness
     * overrides are Persist=1, so a plain setBOOL() would write them into the
     * user's settings.xml on exit; this uses the same non-persisting path
     * --set does. A control that does not exist is skipped, not an error.
     *
     * Pass log_value=false for anything secret: the credential triple would
     * otherwise land in the log in clear text.
     */
    static void forceSetting(const char* name, const LLSD& value, bool log_value = true);
    /**
     * Resize the window to the capture size -- but only for a run that captures
     * the UI.
     *
     * The captured frame's size does not depend on this: captureFrame passes
     * the pinned size to saveSnapshot. But the reference snapshot path cannot
     * render the UI at a size other than the window's ("Scaling of the UI is
     * currently *not* supported", llviewerwindow.cpp), so with show_ui it
     * clamps the requested size to the window and then *scales* the grab down
     * to it. A 4K window captured at 1080p therefore yields a half-size UI,
     * which is not what the other viewer -- whose UI lays out at the capture
     * size -- will have drawn. Asking the window to be the capture size is the
     * only lever there is; when the window manager declines, the run says so.
     *
     * A world-only run does not resize at all: the snapshot renders into its
     * own scratch target, so shrinking the window would cost the operator a
     * watchable run for nothing.
     */
    void applyWindowSize();

    /// Write one frame; returns false if the snapshot failed.
    bool captureFrame(S32 index);
    /// Write the structured scene dump, if one was asked for.
    void writeSceneDump();
    /**
     * Write <screenshot-dir>/harness-status.json. The driving harness reads
     * this rather than an exit code, because the viewer's shutdown path does
     * not carry a status out reliably.
     */
    void writeStatus() const;

    /// Begin a clean logout. @a reason is recorded in the status file.
    void finish(bool ok, const std::string& reason);

    // --- configuration -------------------------------------------------
    bool        mActive = false;
    std::string mScreenshotDir;
    std::string mSceneDumpPath;

    bool        mHaveCameraPos = false;
    LLVector3   mCameraPos;
    bool        mHaveCameraLookAt = false;
    LLVector3   mCameraLookAt;

    bool        mHaveDayPosition = false;
    F32         mDayPosition = 0.f;

    /// The pixel grid every captured frame is rendered at, independent of
    /// whatever size the window ends up being (see captureFrame). Defaults to
    /// 1080p rather than to "whatever the window is": a capture harness whose
    /// resolution is decided by the window manager produces frames that cannot
    /// be compared with the other viewer's, or even with each other.
    /// 1080p. Big enough that fine detail -- texture banding, a mesh LOD
    /// swap, an alpha-sorting seam -- survives into the frame, which is the
    /// whole point of comparing the images at all.
    static constexpr S32 DEFAULT_CAPTURE_WIDTH  = 1920;
    static constexpr S32 DEFAULT_CAPTURE_HEIGHT = 1080;

    S32         mCaptureWidth = DEFAULT_CAPTURE_WIDTH;   ///< SL_VIEWER_CAPTURE_SIZE
    S32         mCaptureHeight = DEFAULT_CAPTURE_HEIGHT;

    /**
     * Which layers of the composited frame the capture holds, each an
     * independent switch and each independent of the capture size --
     * SL_VIEWER_CAPTURE_UI / _HUD / _GIZMOS, mirroring sl-client's
     * --capture-ui / --capture-hud / --capture-gizmos.
     *
     * All three default to off, so a frame holds the world alone: that is the
     * comparison a renderer cross-check is after, and two viewers' interfaces
     * are not the same interface. They are separate switches rather than one
     * "chrome" switch because the questions are separate -- *does the other
     * viewer draw this HUD the same way* is asked with the HUD in the frame and
     * the UI out of it.
     */
    bool        mCaptureUi = false;
    bool        mCaptureHud = false;
    bool        mCaptureGizmos = false;

    F32         mSettleTimeout = 25.f;  ///< SL_VIEWER_SCREENSHOT_DELAY
    F32         mFrameInterval = 0.5f;  ///< SL_VIEWER_SCREENSHOT_INTERVAL
    S32         mFrameCount = 30;       ///< SL_VIEWER_SCREENSHOT_FRAMES
    F32         mLoginTimeout = 180.f;  ///< SL_VIEWER_LOGIN_TIMEOUT

    // --- run state -----------------------------------------------------
    EState      mState = STATE_INACTIVE;
    LLTimer     mStateTimer;      ///< time in the current state
    LLTimer     mFrameTimer;      ///< time since the last captured frame
    S32         mFramesWritten = 0;
    bool        mEnvironmentApplied = false;
    bool        mCameraApplied = false;
    bool        mLoggedOutOk = true;
    std::string mResultReason = "not started";
    bool        mResultOk = false;
    FSTestQuiescence mQuiescence;
};

#endif // FS_TESTHARNESS_H
