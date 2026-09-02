/**
 * @file avatarSkinV.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */


in vec4 weight;

uniform vec4 matrixPalette[45];

mat4 getSkinnedTransform()
{
    // The palette holds each joint's 4x3 transform as three rows, stored plane
    // by plane: row 0 of every joint, then row 1 of every joint, then row 2. So
    // joint j lives at [j], [j + JOINTS] and [j + 2*JOINTS], and the stride is
    // the joint count -- derived from the array rather than written out as 15
    // and 30, which silently had to agree with both the size declared above and
    // LL_CHARACTER_MAX_JOINTS_PER_MESH on the C++ side.
    const int JOINTS = matrixPalette.length() / 3;

    mat4 ret;
    int i = int(floor(weight.x));
    float x = fract(weight.x);

    // A vertex blends between its joint and the *next* one along the chain, but
    // a joint at the end of a chain has no next: the base meshes bind those
    // vertices rigidly, with a blend of exactly zero. The partner is read
    // anyway, and for the last joint of a full palette that read runs off the
    // end of the array -- and mix() evaluates its operand whatever the weight,
    // so a NaN or Inf read back from out of bounds survives being multiplied by
    // zero. That is what emptied the 388 vertices bound to mWristRight, the
    // last entry of the upper body's full 15-joint palette: the avatar's right
    // hand vanished while the identically weighted left hand, whose partner
    // read lands in bounds, rendered normally. Clamping is exact -- it can only
    // alter a blend that is already weighted zero.
    int j = min(i + 1, JOINTS - 1);

    ret[0] = mix(matrixPalette[i             ], matrixPalette[j             ], x);
    ret[1] = mix(matrixPalette[i +     JOINTS], matrixPalette[j +     JOINTS], x);
    ret[2] = mix(matrixPalette[i + 2 * JOINTS], matrixPalette[j + 2 * JOINTS], x);
    ret[3] = vec4(0,0,0,1);

    return ret;

#ifdef IS_AMD_CARD
    // If it's AMD make sure the GLSL compiler sees the arrays referenced once by static index. Otherwise it seems to optimise the storage awawy which leads to unfun crashes and artifacts.
    vec4 dummy1 = matrixPalette[0];
    vec4 dummy2 = matrixPalette[44];
#endif
}
