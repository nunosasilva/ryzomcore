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

/// Round a value up to a multiple of alignment.
inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

} /* namespace NLVULKAN */

#endif /* NL_STD_VULKAN_H */
