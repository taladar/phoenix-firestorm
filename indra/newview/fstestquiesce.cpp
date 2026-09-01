/**
 * @file fstestquiesce.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fstestquiesce.h"

#include <sstream>

#include "llappviewer.h"
#include "llimageworker.h"
#include "llmeshrepository.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llviewertexturelist.h"

S32 FSTestQuiescence::Counts::total() const
{
    return mTextureFetch + mTextureHttp + mImageDecode
         + mCacheReads + mCacheWrites + mTexturesToCreate
         + mMeshLodPending + mMeshLodActive;
}

std::string FSTestQuiescence::Counts::describe() const
{
    std::ostringstream out;
    out << "fetch=" << mTextureFetch
        << " http=" << mTextureHttp
        << " decode=" << mImageDecode
        << " cacheR=" << mCacheReads
        << " cacheW=" << mCacheWrites
        << " create=" << mTexturesToCreate
        << " meshPending=" << mMeshLodPending
        << " meshActive=" << mMeshLodActive;
    return out.str();
}

FSTestQuiescence::Counts FSTestQuiescence::sample()
{
    Counts c;

    // Every subsystem below can legitimately be absent before login, so each
    // is guarded rather than assumed.
    if (LLTextureFetch* fetch = LLAppViewer::getTextureFetch())
    {
        c.mTextureFetch = fetch->getNumRequests();
        c.mTextureHttp  = fetch->getNumHTTPRequests();
    }
    if (LLImageDecodeThread* decode = LLAppViewer::getImageDecodeThread())
    {
        c.mImageDecode = static_cast<S32>(decode->getPending());
    }
    if (LLTextureCache* cache = LLAppViewer::getTextureCache())
    {
        c.mCacheReads  = cache->getNumReads();
        c.mCacheWrites = cache->getNumWrites();
    }

    c.mTexturesToCreate = static_cast<S32>(gTextureList.mCreateTextureList.size());

    c.mMeshLodPending = static_cast<S32>(LLMeshRepository::sLODPending);
    c.mMeshLodActive  = static_cast<S32>(LLMeshRepository::sLODProcessing);

    return c;
}

bool FSTestQuiescence::update(const Counts& counts, S32 hold_frames)
{
    if (counts.quiet())
    {
        ++mQuietFrames;
    }
    else
    {
        // One busy frame breaks the run: a scene that dips to idle between two
        // bursts of fetches has not settled.
        mQuietFrames = 0;
    }
    return mQuietFrames >= hold_frames;
}
