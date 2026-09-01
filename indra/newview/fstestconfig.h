/**
 * @file fstestconfig.h
 * @brief Config-file loading for the automated test harness (FSTestHarness).
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

#ifndef FS_TESTCONFIG_H
#define FS_TESTCONFIG_H

#include <string>

#include "stdtypes.h"
#include "llsd.h"

/**
 * Reads the TOML config files the sl-client Rust viewer already uses, so one
 * harness can drive both viewers from the same files:
 *
 *   credentials.toml
 *     default_avatar = "primary"
 *     [avatars.primary]
 *     first = "Avatar"
 *     last  = "Tester"
 *     password = "secret"
 *     login_uri = "http://127.0.0.1:9100/"     # optional
 *     grid = "aditi"                            # optional
 *     mfa_command = "..."                       # optional
 *     mfa_window_guard_secs = 5                 # optional
 *
 *   grid file
 *     login_uri = "http://127.0.0.1:9100/"
 *     gridname  = "Fake Grid"                   # optional
 *     gridnick  = "fakegrid"                    # optional
 *
 * This is deliberately a *strict subset* of TOML rather than a vendored full
 * parser: the schema above is flat and tiny, and vendoring a megabyte header
 * into a fork that is regularly rebased onto upstream Firestorm costs more than
 * it saves. Everything outside the subset -- arrays, inline tables, dotted
 * keys, multi-line strings, floats, dates -- is a hard parse error naming the
 * line, never a silent misread. A credentials file that this parser accepts
 * means the same thing it means to a real TOML parser.
 */
namespace FSTestConfig
{
    /**
     * Parse TOML text into an LLSD map. Tables nest: a "[avatars.primary]"
     * header puts its keys at result["avatars"]["primary"].
     *
     * Values become LLSD String, Integer or Boolean. Returns false and fills
     * @a error with a "line N: ..." message on any syntax we do not accept.
     */
    bool parseTomlSubset(const std::string& text, LLSD& result, std::string& error);

    /** As parseTomlSubset, reading @a path. Missing/unreadable file is an error. */
    bool parseTomlFile(const std::string& path, LLSD& result, std::string& error);

    /** One resolved avatar from a credentials file. */
    struct Avatar
    {
        std::string mKey;           ///< the [avatars.<key>] name
        std::string mFirst;
        std::string mLast;
        std::string mPassword;
        std::string mLoginUri;      ///< may be empty
        std::string mGrid;          ///< may be empty
        std::string mMfaCommand;    ///< may be empty; not yet executed, see .cpp
        S32         mMfaWindowGuardSecs = 5;
    };

    /**
     * Load @a path and pick an avatar, mirroring sl-client's rule:
     * explicit @a wanted_key, else "default_avatar", else the sole avatar,
     * else an error naming the available keys.
     */
    bool loadCredentials(const std::string& path,
                         const std::string& wanted_key,
                         Avatar& avatar,
                         std::string& error);

    /** One resolved grid description. */
    struct Grid
    {
        std::string mLoginUri;      ///< required
        std::string mGridName;      ///< may be empty -> taken from get_grid_info
        std::string mGridNick;      ///< may be empty
        std::string mHelperUri;     ///< may be empty
        std::string mLoginPage;     ///< may be empty
        std::string mSlurlBase;     ///< may be empty
        std::string mPlatform;      ///< may be empty
    };

    /** Load a grid description file. A missing "login_uri" is an error. */
    bool loadGrid(const std::string& path, Grid& grid, std::string& error);

    /**
     * The bare host[:port] a login URI points at -- what LLGridManager uses as
     * a grid's name/key. "http://127.0.0.1:9100/" -> "127.0.0.1:9100".
     */
    std::string gridNameFromLoginUri(const std::string& login_uri);
}

#endif // FS_TESTCONFIG_H
