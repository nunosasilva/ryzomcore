// NeL - MMORPG Framework <https://wiki.ryzom.dev/>
// Copyright (C) 2026  The Ryzom Core Community
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef NL_STD_VULKAN_H
#define NL_STD_VULKAN_H

#include "nel/misc/types_nl.h"

#include <vector>
#include <string>
#include <map>
#include <memory>

#if defined(NL_OS_UNIX)
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#endif

#include <vulkan/vulkan.h>

#include "nel/misc/debug.h"

namespace NLVULKAN {

/// Pixel format of every NeL texture uploaded or rendered to. Uploads convert
/// to RGBA8, and an offscreen render pass can only be used with a framebuffer
/// whose colour attachment has the same format, so both paths use this.
static const VkFormat NL_VK_TEXTURE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

/// Colour format of the modern sun shadow masks (ITexture::setFloatRenderTarget).
/// One 32-bit float channel holds the light-space depth exactly, so the
/// receiver can compare with a centimetre tolerance instead of the half-metre
/// step an 8-bit mask needs over a ~100 m cascade. Blending is only used with
/// VK_BLEND_OP_MIN, which the format supports on every desktop Vulkan stack
/// and on RADV; the driver probes it and falls back to NL_VK_TEXTURE_FORMAT
/// (and the 8-bit bias) when it is missing.
static const VkFormat NL_VK_SHADOW_MASK_FORMAT = VK_FORMAT_R32_SFLOAT;

/// Round a value up to a multiple of alignment.
inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

} /* namespace NLVULKAN */

#endif /* NL_STD_VULKAN_H */
