/**
 * @file fstestquiesce.h
 * @brief "Has the scene finished loading?" detector for FSTestHarness.
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

#ifndef FS_TESTQUIESCE_H
#define FS_TESTQUIESCE_H

#include <string>

#include "stdtypes.h"

/**
 * Counts the asset work still in flight, so a screenshot is only taken once
 * the scene has stopped changing for reasons that are not divergences.
 *
 * The counters are the same ones the texture console shows
 * (lltextureview.cpp), plus the mesh repository's LOD queues. A frame is
 * "quiet" when every one of them is zero.
 */
class FSTestQuiescence
{
public:
    /** Per-subsystem outstanding work; all zero means the scene has settled. */
    struct Counts
    {
        S32 mTextureFetch     = 0;  ///< LLTextureFetch::getNumRequests()
        S32 mTextureHttp      = 0;  ///< LLTextureFetch::getNumHTTPRequests()
        S32 mImageDecode      = 0;  ///< LLImageDecodeThread::getPending()
        S32 mCacheReads       = 0;  ///< LLTextureCache::getNumReads()
        S32 mCacheWrites      = 0;  ///< LLTextureCache::getNumWrites()
        S32 mTexturesToCreate = 0;  ///< gTextureList.mCreateTextureList.size()
        S32 mMeshLodPending   = 0;  ///< LLMeshRepository::sLODPending
        S32 mMeshLodActive    = 0;  ///< LLMeshRepository::sLODProcessing

        S32  total() const;
        bool quiet() const { return total() == 0; }

        /** Human-readable "textures=3 meshes=1" for the timeout log line. */
        std::string describe() const;
    };

    /** Sample every counter. Safe before login (subsystems may be null). */
    static Counts sample();

    /**
     * Feed one frame. Returns true once @a hold_frames consecutive quiet
     * frames have been seen. A single busy frame resets the run, so a scene
     * that briefly goes idle mid-load does not read as settled.
     */
    bool update(const Counts& counts, S32 hold_frames);

    /** Consecutive quiet frames seen so far. */
    S32 quietFrames() const { return mQuietFrames; }

    void reset() { mQuietFrames = 0; }

private:
    S32 mQuietFrames = 0;
};

#endif // FS_TESTQUIESCE_H
