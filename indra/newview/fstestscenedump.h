/**
 * @file fstestscenedump.h
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

#ifndef FS_TESTSCENEDUMP_H
#define FS_TESTSCENEDUMP_H

#include <string>

#include "stdtypes.h"
#include "llsd.h"

/**
 * Builds a machine-readable description of what the viewer currently believes
 * it is drawing, written next to the screenshots as JSON.
 *
 * Pixels tell you two viewers disagree; this tells you *why*. A prim in the
 * wrong place, a texture that resolved to a different asset id, a mesh stuck
 * at a coarser LOD and a material that never arrived all look like "the image
 * differs" and are four different bugs.
 *
 * The schema is the contract between this viewer and the Rust one, so it is
 * versioned: bump SCHEMA_VERSION on any incompatible change, and keep the
 * sl-client side in step (roadmap item viewer-scene-dump).
 */
namespace FSTestSceneDump
{
    /// Bump on any incompatible schema change.
    static const S32 SCHEMA_VERSION = 1;

    /**
     * Snapshot the current scene. Safe to call any time after login; fields
     * whose subsystems are absent are simply omitted rather than faked.
     */
    LLSD build();

    /**
     * Serialise @a dump as pretty-printed JSON to @a path.
     * Returns false and fills @a error on any I/O failure.
     */
    bool writeJson(const LLSD& dump, const std::string& path, std::string& error);
}

#endif // FS_TESTSCENEDUMP_H
