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

/**
 * Texture handling for the Vulkan driver.
 *
 * Textures are converted to RGBA8 on the CPU and uploaded to device-local
 * images through a staging buffer. Compressed (DXTC) textures are decompressed
 * on upload for now; a native BC path can be added later without touching the
 * rest of the driver.
 */

#include "stdvulkan.h"
#include "driver_vulkan.h"

#include "nel/3d/texture_cube.h"

#include <chrono>

using namespace std;
using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

/// Monotonic nanoseconds for the NLVK_STATS upload counters.
static uint64 uploadNowNs()
{
	return (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

VkBuffer CDriverVulkan::createStagingBuffer(VkDeviceSize size, VkDeviceMemory *memory, void **mapped,
	VkBufferUsageFlags usage)
{
	VkBufferCreateInfo bci;
	memset(&bci, 0, sizeof(bci));
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = usage;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(_Device, &bci, NULL, &buffer) != VK_SUCCESS)
		return VK_NULL_HANDLE;

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(_Device, buffer, &req);

	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(_PhysicalDevice, &memProps);
	uint32 typeIndex = 0;
	bool found = false;
	for (uint32 i = 0; i < memProps.memoryTypeCount; ++i)
	{
		if ((req.memoryTypeBits & (1u << i)) &&
			(memProps.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		{
			typeIndex = i;
			found = true;
			break;
		}
	}
	if (!found)
	{
		vkDestroyBuffer(_Device, buffer, NULL);
		return VK_NULL_HANDLE;
	}

	VkMemoryAllocateInfo mai;
	memset(&mai, 0, sizeof(mai));
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = typeIndex;
	if (vkAllocateMemory(_Device, &mai, NULL, memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(_Device, buffer, NULL);
		return VK_NULL_HANDLE;
	}
	vkBindBufferMemory(_Device, buffer, *memory, 0);
	if (mapped)
		vkMapMemory(_Device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
	return buffer;
}

void CDriverVulkan::destroyTexture(CTextureVulkan &tex)
{
	if (!_Device)
		return;
	if (tex.Image == VK_NULL_HANDLE && tex.Memory == VK_NULL_HANDLE &&
		tex.View == VK_NULL_HANDLE && tex.Sampler == VK_NULL_HANDLE &&
		tex.Framebuffer == VK_NULL_HANDLE)
		return;
	// Commands that reference this texture may still be recording or in flight,
	// so retire it; beginFrame() frees it once no in-flight frame can use it.
	_PendingTextureDestroy.push_back(std::make_pair(_SwapBufferCounter, tex));
	tex = CTextureVulkan();
}

VkFormat CDriverVulkan::compressedTextureFormat(CBitmap::TType format) const
{
	// All BC formats are sampleable when textureCompressionBC is supported.
	// DXT1 loses its punch-through alpha if uploaded as BC1_RGB, so keep the
	// RGBA variant: Ryzom uses DXT1 for foliage and other masked textures.
	if (!_SupportsBC)
		return VK_FORMAT_UNDEFINED;
	switch (format)
	{
	case CBitmap::DXTC1:
	case CBitmap::DXTC1Alpha:	return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case CBitmap::DXTC3:		return VK_FORMAT_BC2_UNORM_BLOCK;
	case CBitmap::DXTC5:		return VK_FORMAT_BC3_UNORM_BLOCK;
	default:					return VK_FORMAT_UNDEFINED;
	}
}

void CDriverVulkan::releaseTextureDrvInfo(CTextureDrvInfosVulkan &info)
{
	_TextureUsed.erase(&info);
	if (_AllocatedTextureMemory >= info.TextureMemory)
		_AllocatedTextureMemory -= info.TextureMemory;
	else
		_AllocatedTextureMemory = 0;
	info.TextureMemory = 0;
	destroyTexture(info.Texture);
}

CTextureDrvInfosVulkan::~CTextureDrvInfosVulkan()
{
	// _Driver is always valid for the textures this driver created: IDriver's
	// release() destroys every CTextureDrvShare (and therefore every entry)
	// before the Vulkan device goes away.
	if (_Driver)
		_Driver->releaseTextureDrvInfo(*this);
}

/// Create the image, memory, view and sampler of a texture. Used for both
/// uploaded textures and render targets: the only difference is the usage
/// flags (render targets add COLOR_ATTACHMENT) and the format (compressed
/// textures keep their BC format, everything else is RGBA8).
bool CDriverVulkan::createTextureImage(uint32 width, uint32 height, uint mipCount,
	VkImageUsageFlags usage, VkFormat format, CTextureVulkan &out)
{
	VkImageCreateInfo ici;
	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.extent.width = width;
	ici.extent.height = height;
	ici.extent.depth = 1;
	ici.mipLevels = mipCount;
	ici.arrayLayers = 1;
	ici.format = format;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.usage = usage;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateImage(_Device, &ici, NULL, &out.Image) != VK_SUCCESS)
		return false;

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(_Device, out.Image, &req);
	out.MemorySize = req.size;
	VkMemoryAllocateInfo mai;
	memset(&mai, 0, sizeof(mai));
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(_Device, &mai, NULL, &out.Memory) != VK_SUCCESS)
		return false;
	if (vkBindImageMemory(_Device, out.Image, out.Memory, 0) != VK_SUCCESS)
		return false;

	VkImageViewCreateInfo vci;
	memset(&vci, 0, sizeof(vci));
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = out.Image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vci.subresourceRange.levelCount = mipCount;
	vci.subresourceRange.layerCount = 1;
	if (vkCreateImageView(_Device, &vci, NULL, &out.View) != VK_SUCCESS)
		return false;

	VkSamplerCreateInfo sci;
	memset(&sci, 0, sizeof(sci));
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = mipCount > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = (float)mipCount;
	vkCreateSampler(_Device, &sci, NULL, &out.Sampler);

	out.Width = width;
	out.Height = height;
	out.MipLevels = mipCount;
	out.Layers = 1;
	out.Usage = usage;
	out.Format = format;
	out.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

VkImageView CDriverVulkan::getWhiteTexture()
{
	if (_WhiteTexture.Image == VK_NULL_HANDLE)
	{
		uint8 white[4] = { 255, 255, 255, 255 };
		VkBuffer staging = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		void *mapped = NULL;
		staging = createStagingBuffer(4, &stagingMemory, &mapped);
		if (staging == VK_NULL_HANDLE)
			return VK_NULL_HANDLE;
		memcpy(mapped, white, 4);

		VkImageCreateInfo ici;
		memset(&ici, 0, sizeof(ici));
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.extent.width = 1;
		ici.extent.height = 1;
		ici.extent.depth = 1;
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.format = NL_VK_TEXTURE_FORMAT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateImage(_Device, &ici, NULL, &_WhiteTexture.Image) != VK_SUCCESS)
			return VK_NULL_HANDLE;

		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(_Device, _WhiteTexture.Image, &req);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		vkAllocateMemory(_Device, &mai, NULL, &_WhiteTexture.Memory);
		vkBindImageMemory(_Device, _WhiteTexture.Image, _WhiteTexture.Memory, 0);

		VkImageViewCreateInfo vci;
		memset(&vci, 0, sizeof(vci));
		vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vci.image = _WhiteTexture.Image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = NL_VK_TEXTURE_FORMAT;
		vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vci.subresourceRange.levelCount = 1;
		vci.subresourceRange.layerCount = 1;
		vkCreateImageView(_Device, &vci, NULL, &_WhiteTexture.View);

		VkSamplerCreateInfo sci;
		memset(&sci, 0, sizeof(sci));
		sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sci.magFilter = VK_FILTER_NEAREST;
		sci.minFilter = VK_FILTER_NEAREST;
		sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		vkCreateSampler(_Device, &sci, NULL, &_WhiteTexture.Sampler);

		// Upload + transition in one submit.
		vkDeviceWaitIdle(_Device);
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkCommandBufferAllocateInfo cbi;
		memset(&cbi, 0, sizeof(cbi));
		cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbi.commandPool = _CommandPool;
		cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbi.commandBufferCount = 1;
		vkAllocateCommandBuffers(_Device, &cbi, &cmd);
		VkCommandBufferBeginInfo bi;
		memset(&bi, 0, sizeof(bi));
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);
		VkImageMemoryBarrier barrier;
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = _WhiteTexture.Image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		VkBufferImageCopy region;
		memset(&region, 0, sizeof(region));
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = 1;
		region.imageExtent.height = 1;
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(cmd, staging, _WhiteTexture.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		vkEndCommandBuffer(cmd);
		VkSubmitInfo si;
		memset(&si, 0, sizeof(si));
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		vkQueueSubmit(_GraphicsQueue, 1, &si, VK_NULL_HANDLE);
		vkQueueWaitIdle(_GraphicsQueue);
		vkFreeCommandBuffers(_Device, _CommandPool, 1, &cmd);
		vkDestroyBuffer(_Device, staging, NULL);
		vkFreeMemory(_Device, stagingMemory, NULL);
	}
	return _WhiteTexture.View;
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

/// Bytes of one 4x4 block of a BC format, or 0 for other formats.
static uint32 compressedBlockBytes(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:	return 8;
	case VK_FORMAT_BC2_UNORM_BLOCK:
	case VK_FORMAT_BC3_UNORM_BLOCK:		return 16;
	default:							return 0;
	}
}

/// Bytes of the BC3 image of a mip level (4x4 blocks, 16 bytes each).
static uint32 bc3DataSize(uint32 width, uint32 height)
{
	return ((width + 3) / 4) * ((height + 3) / 4) * 16;
}

/// Pack an 8-bit RGB triple into a 565 endpoint.
static uint16 toRGB565(uint32 r, uint32 g, uint32 b)
{
	return (uint16)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

/// Encode one 4x4 RGBA block as BC3 (DXT5): an 8-byte interpolation alpha
/// block followed by an 8-byte colour block. Edge blocks replicate the last
/// row/column, which is what the GPU reads back for a partial block.
///
/// The endpoints are the per-block bounding box (min/max per channel), the
/// classic simple encoder. It is not the highest-quality DXT encoder, but it
/// is deterministic, symmetric and good enough for the ForceDXTC request:
/// the client asks for a memory reduction, not for offline DDS quality.
static void compressBlockBC3(const uint8 *src, uint32 srcStride, uint32 srcWidth, uint32 srcHeight,
	uint32 blockX, uint32 blockY, uint8 *dst)
{
	uint8 block[16 * 4];
	for (uint y = 0; y < 4; ++y)
	{
		const uint32 sy = std::min(blockY + y, srcHeight - 1);
		const uint8 *row = src + (size_t)sy * srcStride;
		for (uint x = 0; x < 4; ++x)
		{
			const uint32 sx = std::min(blockX + x, srcWidth - 1);
			memcpy(block + (y * 4 + x) * 4, row + (size_t)sx * 4, 4);
		}
	}

	// --- Alpha: 8-value palette between the block minimum and maximum ---
	uint32 minA = 255, maxA = 0;
	for (uint i = 0; i < 16; ++i)
	{
		const uint32 a = block[i * 4 + 3];
		minA = std::min(minA, a);
		maxA = std::max(maxA, a);
	}
	uint8 alpha[8];
	alpha[0] = (uint8)maxA;
	alpha[1] = (uint8)minA;
	if (maxA != minA)
		for (uint i = 2; i < 8; ++i)
			alpha[i] = (uint8)(((8 - i) * maxA + (i - 1) * minA) / 7);
	else
		for (uint i = 2; i < 8; ++i)
			alpha[i] = (uint8)maxA;

	dst[0] = alpha[0];
	dst[1] = alpha[1];
	uint64 alphaBits = 0;
	for (uint i = 0; i < 16; ++i)
	{
		const uint8 a = block[i * 4 + 3];
		uint32 best = 0;
		uint32 bestDist = 0xFFFFFFFFu;
		for (uint p = 0; p < 8; ++p)
		{
			const uint32 d = (uint32)abs((int)a - (int)alpha[p]);
			if (d < bestDist) { bestDist = d; best = p; }
		}
		alphaBits |= (uint64)best << (i * 3);
	}
	for (uint i = 0; i < 6; ++i)
		dst[2 + i] = (uint8)(alphaBits >> (i * 8));

	// --- Colour: bounding box endpoints in 565, 4-value mode ---
	uint32 minR = 255, minG = 255, minB = 255;
	uint32 maxR = 0, maxG = 0, maxB = 0;
	for (uint i = 0; i < 16; ++i)
	{
		const uint8 *p = block + i * 4;
		minR = std::min(minR, (uint32)p[0]); maxR = std::max(maxR, (uint32)p[0]);
		minG = std::min(minG, (uint32)p[1]); maxG = std::max(maxG, (uint32)p[1]);
		minB = std::min(minB, (uint32)p[2]); maxB = std::max(maxB, (uint32)p[2]);
	}
	uint16 c0 = toRGB565(maxR, maxG, maxB);
	uint16 c1 = toRGB565(minR, minG, minB);
	if (c0 < c1)
		std::swap(c0, c1); // 4-colour mode requires c0 > c1

	// Expand the endpoints back to 8 bits, then the two interpolated colours.
	uint32 pal[4][3];
	const uint32 r0 = ((c0 >> 11) & 0x1F) * 255 / 31, g0 = ((c0 >> 5) & 0x3F) * 255 / 63, b0 = (c0 & 0x1F) * 255 / 31;
	const uint32 r1 = ((c1 >> 11) & 0x1F) * 255 / 31, g1 = ((c1 >> 5) & 0x3F) * 255 / 63, b1 = (c1 & 0x1F) * 255 / 31;
	pal[0][0] = r0; pal[0][1] = g0; pal[0][2] = b0;
	pal[1][0] = r1; pal[1][1] = g1; pal[1][2] = b1;
	pal[2][0] = (2 * r0 + r1) / 3; pal[2][1] = (2 * g0 + g1) / 3; pal[2][2] = (2 * b0 + b1) / 3;
	pal[3][0] = (r0 + 2 * r1) / 3; pal[3][1] = (g0 + 2 * g1) / 3; pal[3][2] = (b0 + 2 * b1) / 3;

	dst[8] = (uint8)c0;
	dst[9] = (uint8)(c0 >> 8);
	dst[10] = (uint8)c1;
	dst[11] = (uint8)(c1 >> 8);
	uint32 colorBits = 0;
	for (uint i = 0; i < 16; ++i)
	{
		const uint8 *p = block + i * 4;
		uint32 best = 0;
		uint32 bestDist = 0xFFFFFFFFu;
		for (uint q = 0; q < 4; ++q)
		{
			const int dr = (int)p[0] - (int)pal[q][0];
			const int dg = (int)p[1] - (int)pal[q][1];
			const int db = (int)p[2] - (int)pal[q][2];
			const uint32 d = (uint32)(dr * dr + dg * dg + db * db);
			if (d < bestDist) { bestDist = d; best = q; }
		}
		colorBits |= best << (i * 2);
	}
	dst[12] = (uint8)colorBits;
	dst[13] = (uint8)(colorBits >> 8);
	dst[14] = (uint8)(colorBits >> 16);
	dst[15] = (uint8)(colorBits >> 24);
}

/// Compress a whole RGBA mip level to BC3.
static void compressRGBAtoBC3(const uint8 *src, uint32 width, uint32 height, uint8 *dst)
{
	const uint32 blocksX = (width + 3) / 4;
	const uint32 blocksY = (height + 3) / 4;
	for (uint by = 0; by < blocksY; ++by)
	{
		for (uint bx = 0; bx < blocksX; ++bx)
		{
			compressBlockBC3(src, width * 4, width, height, bx * 4, by * 4,
				dst + ((size_t)by * blocksX + bx) * 16);
		}
	}
}

/// Bytes per texel of an uncompressed CBitmap format, or 0 for a format that
/// has to be converted before it can be read (the compressed ones).
static uint sourcePixelSize(CBitmap::TType format)
{
	switch (format)
	{
	case CBitmap::RGBA:				return 4;
	case CBitmap::AlphaLuminance:	return 2;
	case CBitmap::Alpha:
	case CBitmap::Luminance:		return 1;
	default:						return 0;
	}
}

/// Make sure the texture bitmap is in a format the upload can read directly.
///
/// Compressed textures are decompressed in place once (they are file data and
/// never receive partial updates after that). The uncompressed formats are
/// read where they are and the texture is NOT modified: in particular
/// CTextureFont owns an Alpha atlas that it keeps writing one coverage byte
/// per texel to between uploads, and rewriting that bitmap as RGBA here (as an
/// unconditional convertToType() used to do) corrupts the whole glyph atlas.
static bool prepareTexturePixels(ITexture &tex)
{
	if (sourcePixelSize(tex.getPixelFormat()) != 0)
		return true;
	return tex.convertToType(CBitmap::RGBA);
}

/// Convert `count` texels of one bitmap row to RGBA8.
static void convertRowToRGBA(const uint8 *src, uint count, CBitmap::TType format, uint8 *dst)
{
	switch (format)
	{
	case CBitmap::RGBA:
		memcpy(dst, src, (size_t)count * 4);
		break;
	case CBitmap::Alpha:
		// Coverage mask: the colour comes from the material / vertex colour.
		for (uint i = 0; i < count; ++i)
		{
			dst[i * 4 + 0] = 255;
			dst[i * 4 + 1] = 255;
			dst[i * 4 + 2] = 255;
			dst[i * 4 + 3] = src[i];
		}
		break;
	case CBitmap::Luminance:
		for (uint i = 0; i < count; ++i)
		{
			dst[i * 4 + 0] = src[i];
			dst[i * 4 + 1] = src[i];
			dst[i * 4 + 2] = src[i];
			dst[i * 4 + 3] = 255;
		}
		break;
	case CBitmap::AlphaLuminance:
		for (uint i = 0; i < count; ++i)
		{
			dst[i * 4 + 0] = src[i * 2 + 0];
			dst[i * 4 + 1] = src[i * 2 + 0];
			dst[i * 4 + 2] = src[i * 2 + 0];
			dst[i * 4 + 3] = src[i * 2 + 1];
		}
		break;
	default:
		nlassert(sourcePixelSize(format) != 0);
		break;
	}
}

/// Copy a rect of one mip level to `dst` as tightly packed RGBA8.
static void copyTextureRect(ITexture &tex, uint mipLevel, const NLMISC::CRect &rect, uint8 *dst)
{
	const CBitmap::TType format = tex.getPixelFormat();
	const uint bytesPerPixel = sourcePixelSize(format);
	const uint srcWidth = tex.getWidth(mipLevel);
	const uint8 *src = (const uint8 *)tex.getPixels(mipLevel).getPtr();
	for (sint y = 0; y < rect.Height; ++y)
	{
		const uint8 *row = src + ((size_t)(rect.Y + y) * srcWidth + rect.X) * bytesPerPixel;
		convertRowToRGBA(row, rect.Width, format, dst + (size_t)y * rect.Width * 4);
	}
}

/// Clip a client rect to a mip level. Returns false when nothing is left.
static bool clampTextureRect(NLMISC::CRect &rect, uint width, uint height)
{
	sint x0 = rect.X, y0 = rect.Y;
	sint x1 = rect.X + rect.Width, y1 = rect.Y + rect.Height;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (sint)width) x1 = (sint)width;
	if (y1 > (sint)height) y1 = (sint)height;
	if (x1 <= x0 || y1 <= y0)
		return false;
	rect.X = x0;
	rect.Y = y0;
	rect.Width = x1 - x0;
	rect.Height = y1 - y0;
	return true;
}

bool CDriverVulkan::uploadTextureVulkan(ITexture &tex, CTextureVulkan &out, bool upload)
{
	destroyTexture(out);

	// A render target is drawn into, not uploaded: give it an image the
	// render-target path can attach and skip pixel generation entirely.
	const bool renderTarget = tex.getRenderTarget();
	if (!renderTarget)
		tex.generate();

	uint width = tex.getWidth();
	uint height = tex.getHeight();
	if (width == 0 || height == 0)
		return false;

	// Compressed textures stay compressed all the way to the GPU when the
	// device can sample BC images: the bitmap is not expanded to RGBA on the
	// CPU, which used to multiply the client's texture memory by 4 to 8 (DXT1
	// is 4 bits per texel, RGBA is 32) and quadrupled the upload bandwidth.
	// An explicit upload format means the engine wants that representation
	// (alpha-only fonts, luminance masks, forced RGBA), so only the automatic
	// case keeps the BC data.
	VkFormat format = VK_FORMAT_UNDEFINED;
	if (!renderTarget && tex.getUploadFormat() == ITexture::Auto)
		format = compressedTextureFormat(tex.getPixelFormat());
	bool compressed = (format != VK_FORMAT_UNDEFINED);

	// ForceDXTCCompression: the client asks for runtime DXT compression of
	// uncompressed RGBA textures (client.cfg ForceDXTC). The engine has no
	// RGBA->DXT compressor (CBitmap::convertToDXTC5 only rewrites DXT1 data),
	// so the driver encodes BC3 here when the device can sample BC images.
	bool compressFromRGBA = false;
	if (!renderTarget && !compressed && _ForceDXTCCompression && _SupportsBC &&
		tex.getUploadFormat() == ITexture::Auto && tex.getPixelFormat() == CBitmap::RGBA &&
		tex.allowDegradation())
	{
		format = VK_FORMAT_BC3_UNORM_BLOCK;
		compressed = true;
		compressFromRGBA = true;
	}

	uint mipCount = 1;
	if (!renderTarget && tex.mipMapOn())
	{
		if (compressed && !compressFromRGBA)
		{
			// DDS files ship their mip chain; CBitmap::buildMipMaps() only
			// works on RGBA data and would decompress the whole texture.
			mipCount = max(1u, (uint)tex.getMipMapCount());
		}
		else
		{
			tex.buildMipMaps();
			mipCount = max(1u, (uint)tex.getMipMapCount());
		}
	}
	if (!compressed)
		format = NL_VK_TEXTURE_FORMAT;

	// TRANSFER_SRC is always on so any texture can be the source of
	// stretchRect()'s vkCmdBlitImage (the engine also stretches plain textures).
	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (renderTarget)
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (!createTextureImage(width, height, mipCount, usage, format, out))
		return false;

	out.HasAlpha = true;
	out.Compressed = compressed;
	out.CompressFromRGBA = compressFromRGBA;
	out.SourceFormat = tex.getPixelFormat();
	if (compressFromRGBA)
		nlinfo("Vulkan: ForceDXTC: compressing RGBA texture %ux%u (%u mips) to BC3", width, height, mipCount);

	if (renderTarget)
		return true;

	if (compressed)
		nldebug("Vulkan: compressed texture %ux%u %u mips format=%d uploaded as BC",
			width, height, mipCount, (int)out.SourceFormat);

	if (!upload)
	{
		// No pixels yet (a render target or a texture the caller streams in
		// with uploadTexture()/touchRect()). The image view covers every mip
		// level and the sampler may read any of them, while a region upload
		// only ever transitions the level it writes, so put the whole image in
		// the shader-read layout now. The contents are undefined either way.
		STextureCopy copy;
		copy.Image = out.Image;
		copy.OldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		copy.BaseLevel = 0;
		copy.LevelCount = mipCount;
		if (!recordTextureCopy(copy))
			return false;
		out.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		out.Uploaded = true;
		return true;
	}

	return uploadTexturePixels(tex, out);
}

// ---------------------------------------------------------------------------
// Upload staging ring
// ---------------------------------------------------------------------------

bool CDriverVulkan::createStagingRing(uint32 size)
{
	VkBufferCreateInfo bci;
	memset(&bci, 0, sizeof(bci));
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(_Device, &bci, NULL, &buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(_Device, buffer, &req);
	VkMemoryAllocateInfo mai;
	memset(&mai, 0, sizeof(mai));
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (vkAllocateMemory(_Device, &mai, NULL, &memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(_Device, buffer, NULL);
		return false;
	}
	vkBindBufferMemory(_Device, buffer, memory, 0);
	void *mapped = NULL;
	if (vkMapMemory(_Device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		vkDestroyBuffer(_Device, buffer, NULL);
		vkFreeMemory(_Device, memory, NULL);
		return false;
	}

	// Copies already recorded reference the old buffer: retire it at a frame
	// boundary like every other ring.
	if (_StagingRing)
		deferDestroyBuffer(_StagingRing, _StagingRingMemory);
	_StagingRing = buffer;
	_StagingRingMemory = memory;
	_StagingRingMapped = mapped;
	_StagingRingSize = size;
	_StagingRingHalf = size / 2;
	_StagingRingFrameBase = _FrameIndex * _StagingRingHalf;
	_StagingRingOffset = _StagingRingFrameBase;
	_StagingRingIdleFrames = 0;
	return true;
}

bool CDriverVulkan::growStagingRing(uint32 required)
{
	// One copy cannot be split over two buffers, so the ring must have a half
	// that fits it. Very large textures get a dedicated staging buffer instead
	// (see allocTextureStaging()), which keeps the ring small.
	uint32 newSize = _StagingRingSize ? _StagingRingSize : NL_VK_STAGING_RING_INITIAL;
	// Always double: the ring grew because this frame's slice did not fit, not
	// necessarily because one copy is bigger than the current half.
	while (newSize / 2 < required || newSize <= _StagingRingSize)
	{
		if (newSize >= NL_VK_STAGING_RING_MAX)
			return false;
		newSize *= 2;
	}
	if (!createStagingRing(newSize))
		return false;
	nlinfo("Vulkan: texture staging ring grown to %u bytes (%u bytes needed)", newSize, required);
	return true;
}

void CDriverVulkan::shrinkStagingRing()
{
	// The loading phase can grow the ring to tens of MiB; in game the traffic
	// is a few KiB per frame. Rebuild it at its initial size once it is idle
	// (see swapBuffers()), so the memory comes back. The old buffer is retired
	// with the frame that may still read it, like any other ring growth.
	if (_StagingRingSize <= NL_VK_STAGING_RING_INITIAL)
		return;
	if (createStagingRing(NL_VK_STAGING_RING_INITIAL))
		nlinfo("Vulkan: texture staging ring shrunk to %u bytes (idle)", NL_VK_STAGING_RING_INITIAL);
}

bool CDriverVulkan::allocTextureStaging(VkDeviceSize size, STextureStaging &out)
{
	out.Buffer = VK_NULL_HANDLE;
	out.Dedicated = VK_NULL_HANDLE;
	out.Offset = 0;
	out.Mapped = NULL;
	if (size == 0 || size > 0xFFFFFFFFu)
		return false;

	// BC blocks are at most 16 bytes and RGBA texels 4, so 16-byte alignment
	// satisfies every bufferOffset requirement of vkCmdCopyBufferToImage.
	const VkDeviceSize alignment = 16;
	// This frame slot's half of the ring. Uploads can also arrive between two
	// frames, after _FrameIndex moved to the next slot but before beginFrame()
	// updated _StagingRingFrameBase; always use this slot's base, not the
	// member, or the first such upload would grow the ring for no reason.
	const uint32 frameBase = _StagingRingHalf ? _FrameIndex * _StagingRingHalf : 0;
	if (_StagingRingHalf &&
		(_StagingRingOffset < frameBase || _StagingRingOffset >= frameBase + _StagingRingHalf))
		_StagingRingOffset = frameBase;
	VkDeviceSize offset = ((VkDeviceSize)_StagingRingOffset + alignment - 1) & ~(alignment - 1);
	if (_StagingRing == VK_NULL_HANDLE || offset + size > (VkDeviceSize)frameBase + _StagingRingHalf)
	{
		// Grow so that this frame's whole upload set fits: the copies already
		// recorded still occupy their slices, so only the space after them
		// helps. Passing just `size` doubles the ring once per copy instead.
		const uint64 needed64 = (offset - frameBase) + size;
		const uint32 needed = needed64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32)needed64;
		if (!growStagingRing(needed))
		{
			// Too large for the ring: one dedicated allocation, retired with the
			// frame that reads it.
			void *mapped = NULL;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			out.Buffer = createStagingBuffer(size, &memory, &mapped);
			if (out.Buffer == VK_NULL_HANDLE)
				return false;
			out.Dedicated = memory;
			out.Mapped = mapped;
			return true;
		}
		offset = ((VkDeviceSize)_StagingRingOffset + alignment - 1) & ~(alignment - 1);
	}
	out.Buffer = _StagingRing;
	out.Offset = offset;
	out.Mapped = (uint8 *)_StagingRingMapped + offset;
	_StagingRingOffset = (uint32)(offset + size);
	return true;
}

// ---------------------------------------------------------------------------
// Upload recording
// ---------------------------------------------------------------------------

bool CDriverVulkan::beginUploadBatch()
{
	// Called by beginFrame() after the frame fence proved that the submission
	// which used this command buffer last has finished.
	_UploadRecording = false;
	_UploadCopyCount = 0;
	VkCommandBufferBeginInfo bi;
	memset(&bi, 0, sizeof(bi));
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(_UploadCommandBuffers[_FrameIndex], &bi) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkBeginCommandBuffer(upload) failed");
		return false;
	}
	_UploadRecording = true;

	// Copies the engine asked for between the previous frame and this one.
	for (size_t i = 0; i < _PendingTextureCopies.size(); ++i)
		recordTextureCopy(_PendingTextureCopies[i]);
	_PendingTextureCopies.clear();
	return true;
}

bool CDriverVulkan::submitUploadBatch()
{
	if (!_UploadRecording)
		return false;
	_UploadRecording = false;
	if (vkEndCommandBuffer(_UploadCommandBuffers[_FrameIndex]) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkEndCommandBuffer(upload) failed");
		return false;
	}
	return _UploadCopyCount > 0;
}

bool CDriverVulkan::recordTextureCopy(const STextureCopy &copy)
{
	if (copy.Image == VK_NULL_HANDLE)
		return false;
	if (!_UploadRecording)
	{
		// No frame is being recorded, so the upload command buffer still belongs
		// to a submission that may be executing; keep the copy for beginFrame().
		_PendingTextureCopies.push_back(copy);
		return true;
	}

	VkCommandBuffer cmd = _UploadCommandBuffers[_FrameIndex];
	VkImageMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = copy.OldLayout;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = copy.Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = copy.BaseLevel;
	barrier.subresourceRange.levelCount = copy.LevelCount;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = copy.OldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0
		: (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	// The source scope covers the previous frame's sampling of this image (and a
	// previous frame's copy of it): its commands were submitted to this queue
	// earlier, and submission order plus this barrier is what orders them, with
	// no CPU wait. Vertex, fragment and transfer are the stages that can touch a
	// texture; ALL_COMMANDS is not needed and would flush more caches than the
	// copy depends on.
	vkCmdPipelineBarrier(cmd,
		copy.OldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
			: VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	if (copy.RegionCount)
	{
		vkCmdCopyBufferToImage(cmd, copy.Staging, copy.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			copy.RegionCount, copy.Regions);
	}

	// Back to the layout every sampled descriptor declares. The frame's draws
	// read the image after this copy, in a later command buffer of the same
	// submission.
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier);
	++_StatsTextureCopies;
	_StatsUploadBytes += (uint64)copy.StagingBytes;
	++_UploadCopyCount;
	return true;
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

bool CDriverVulkan::uploadTexturePixels(ITexture &tex, CTextureVulkan &out)
{
	if (out.Image == VK_NULL_HANDLE)
		return false;

	const uint32 mipCount = out.MipLevels ? out.MipLevels : 1;

	// Compressed textures are copied straight from the CBitmap: its mip chain
	// is already DXT data, so nothing is converted or generated on the CPU.
	// ForceDXTC textures are compressed from their RGBA mips in the loop below.
	if (!out.Compressed && !prepareTexturePixels(tex))
		return false;

	// Build the staging data and the copy regions for all mip levels.
	VkDeviceSize totalSize = 0;
	for (uint i = 0; i < mipCount; ++i)
	{
		if (out.CompressFromRGBA)
			totalSize += bc3DataSize(tex.getWidth(i), tex.getHeight(i));
		else if (out.Compressed)
			totalSize += tex.getPixels(i).size();
		else
			totalSize += (VkDeviceSize)tex.getWidth(i) * tex.getHeight(i) * 4;
	}
	if (totalSize == 0)
		return false;

	STextureStaging staging;
	if (!allocTextureStaging(totalSize, staging))
		return false;

	STextureCopy copy;
	copy.Image = out.Image;
	copy.OldLayout = out.Layout;
	copy.BaseLevel = 0;
	copy.LevelCount = mipCount;
	copy.Staging = staging.Buffer;
	copy.StagingBytes = totalSize;
	VkDeviceSize offset = 0;
	for (uint i = 0; i < mipCount && copy.RegionCount < MaxTextureCopyRegions; ++i)
	{
		uint w = tex.getWidth(i);
		uint h = tex.getHeight(i);
		VkDeviceSize size;
		if (out.CompressFromRGBA)
		{
			size = bc3DataSize(w, h);
			compressRGBAtoBC3((const uint8 *)tex.getPixels(i).getPtr(), w, h, (uint8 *)staging.Mapped + offset);
		}
		else if (out.Compressed)
		{
			size = tex.getPixels(i).size();
			memcpy((uint8 *)staging.Mapped + offset, tex.getPixels(i).getPtr(), (size_t)size);
		}
		else
		{
			size = (VkDeviceSize)w * h * 4;
			copyTextureRect(tex, i, CRect(0, 0, w, h), (uint8 *)staging.Mapped + offset);
		}
		VkBufferImageCopy &region = copy.Regions[copy.RegionCount];
		memset(&region, 0, sizeof(region));
		region.bufferOffset = staging.Offset + offset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = i;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = w;
		region.imageExtent.height = h;
		region.imageExtent.depth = 1;
		offset += size;
		++copy.RegionCount;
	}

	const bool recorded = recordTextureCopy(copy);
	// A dedicated staging buffer is read by the copy command of this frame.
	if (staging.Dedicated != VK_NULL_HANDLE)
		deferDestroyBuffer(staging.Buffer, staging.Dedicated);
	if (!recorded)
		return false;

	out.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	out.Uploaded = true;
	return true;
}

bool CDriverVulkan::uploadTextureRegion(ITexture &tex, CTextureVulkan &out, const NLMISC::CRect &rect, uint mipLevel)
{
	if (out.Image == VK_NULL_HANDLE || tex.getWidth(mipLevel) == 0 || tex.getHeight(mipLevel) == 0)
		return false;

	// A ForceDXTC texture is RGBA on the CPU and BC3 on the GPU: a rectangle
	// would have to be recompressed in place, so just re-upload every mip.
	if (out.CompressFromRGBA)
		return uploadTexturePixels(tex, out);

	STextureCopy copy;
	copy.Image = out.Image;
	copy.OldLayout = out.Layout;
	copy.BaseLevel = mipLevel;
	copy.LevelCount = 1;

	if (out.Compressed)
	{
		// Compressed textures are updated in whole 4x4 blocks. This is the path
		// CAsyncTextureManager uses to stream a DDS texture in a few block rows at
		// a time, so it must not decompress anything.
		const uint32 blockBytes = compressedBlockBytes(out.Format);
		if (blockBytes == 0 || mipLevel >= tex.getMipMapCount() || mipLevel >= out.MipLevels)
			return false;
		const uint32 blocksX = (tex.getWidth(mipLevel) + 3) / 4;
		const uint32 blocksY = (tex.getHeight(mipLevel) + 3) / 4;
		// The engine streams full-width rows in multiples of 4 lines, but clamp
		// to the mip so a caller with an arbitrary rect still lands in bounds.
		sint bx0 = std::max(0, rect.X) / 4;
		sint by0 = std::max(0, rect.Y) / 4;
		sint bx1 = std::min<sint>(blocksX, (std::max(0, rect.X) + rect.Width + 3) / 4);
		sint by1 = std::min<sint>(blocksY, (std::max(0, rect.Y) + rect.Height + 3) / 4);
		if (bx1 <= bx0 || by1 <= by0)
			return true;
		const uint32 rowBytes = (uint32)(bx1 - bx0) * blockBytes;
		const uint32 srcRowStride = blocksX * blockBytes;
		const uint8 *src = (const uint8 *)tex.getPixels(mipLevel).getPtr();
		if (!src)
			return false;

		STextureStaging staging;
		if (!allocTextureStaging((VkDeviceSize)rowBytes * (by1 - by0), staging))
			return false;
		for (sint by = by0; by < by1; ++by)
			memcpy((uint8 *)staging.Mapped + (by - by0) * rowBytes,
				src + by * srcRowStride + bx0 * blockBytes, rowBytes);

		VkBufferImageCopy &region = copy.Regions[0];
		memset(&region, 0, sizeof(region));
		region.bufferOffset = staging.Offset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mipLevel;
		region.imageSubresource.layerCount = 1;
		region.imageOffset.x = bx0 * 4;
		region.imageOffset.y = by0 * 4;
		region.imageExtent.width = std::min((uint32)(bx1 - bx0) * 4, tex.getWidth(mipLevel));
		region.imageExtent.height = std::min((uint32)(by1 - by0) * 4, tex.getHeight(mipLevel));
		region.imageExtent.depth = 1;
		copy.Staging = staging.Buffer;
		copy.StagingBytes = (VkDeviceSize)rowBytes * (by1 - by0);
		copy.RegionCount = 1;

		const bool recorded = recordTextureCopy(copy);
		if (staging.Dedicated != VK_NULL_HANDLE)
			deferDestroyBuffer(staging.Buffer, staging.Dedicated);
		if (!recorded)
			return false;
		out.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		out.Uploaded = true;
		return true;
	}

	CRect clipped = rect;
	if (!clampTextureRect(clipped, tex.getWidth(mipLevel), tex.getHeight(mipLevel)))
		return true; // fully outside the mip: nothing to update

	// See the note in prepareTexturePixels(): keep Alpha/Luminance textures as
	// they are, so CTextureFont can keep drawing into its atlas.
	if (!prepareTexturePixels(tex))
		return false;

	STextureStaging staging;
	if (!allocTextureStaging((VkDeviceSize)clipped.Width * clipped.Height * 4, staging))
		return false;
	copyTextureRect(tex, mipLevel, clipped, (uint8 *)staging.Mapped);

	// Unlike a full upload this must preserve the rest of the image, so the old
	// layout is the one the image is actually in (UNDEFINED on first touch).
	VkBufferImageCopy &region = copy.Regions[0];
	memset(&region, 0, sizeof(region));
	region.bufferOffset = staging.Offset;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = mipLevel;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x = clipped.X;
	region.imageOffset.y = clipped.Y;
	region.imageExtent.width = clipped.Width;
	region.imageExtent.height = clipped.Height;
	region.imageExtent.depth = 1;
	copy.Staging = staging.Buffer;
	copy.StagingBytes = (VkDeviceSize)clipped.Width * clipped.Height * 4;
	copy.RegionCount = 1;

	const bool recorded = recordTextureCopy(copy);
	if (staging.Dedicated != VK_NULL_HANDLE)
		deferDestroyBuffer(staging.Buffer, staging.Dedicated);
	if (!recorded)
		return false;
	out.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	out.Uploaded = true;
	return true;
}

// ---------------------------------------------------------------------------
// Driver texture infos
// ---------------------------------------------------------------------------

void CDriverVulkan::setTextureImage(CTextureDrvInfosVulkan &info, CTextureVulkan &created)
{
	if (_AllocatedTextureMemory >= info.TextureMemory)
		_AllocatedTextureMemory -= info.TextureMemory;
	else
		_AllocatedTextureMemory = 0;
	destroyTexture(info.Texture);
	info.Texture = created;
	info.TextureMemory = (uint32)created.MemorySize;
	_AllocatedTextureMemory += info.TextureMemory;
	created = CTextureVulkan();
}

CTextureDrvInfosVulkan *CDriverVulkan::getTextureDrvInfos(ITexture &tex, bool *created)
{
	if (created)
		*created = false;

	// First use by the engine: create the per-texture share object, and get it
	// registered so that IDriver::release() tears it down with the driver.
	if (!tex.TextureDrvShare)
	{
		ItTexDrvSharePtrList it = _TexDrvShares.insert(_TexDrvShares.end(), (NL3D::CTextureDrvShare *)NULL);
		*it = tex.TextureDrvShare = new CTextureDrvShare(this, it, &tex);
	}

	// Shared textures: one image per share name, whatever the number of
	// ITexture instances that point at it. This is what keeps the client from
	// uploading the same .dds once per mesh that uses it.
	if (tex.supportSharing())
	{
		string name;
		getTextureShareName(tex, name);

		// The destructor of a replaced infos removes its map entry and takes
		// _SyncTexDrvInfos, so the previous entry must die outside the lock.
		NLMISC::CSmartPtr<ITextureDrvInfos> maybeDeleted;
		CTextureDrvInfosVulkan *info = NULL;
		{
			CSynchronized<TTexDrvInfoPtrMap>::CAccessor access(&_SyncTexDrvInfos);
			TTexDrvInfoPtrMap &map = access.value();
			ItTexDrvInfoPtrMap it = map.find(name);
			if (it == map.end())
			{
				it = map.insert(make_pair(name, (ITextureDrvInfos *)NULL)).first;
				info = new CTextureDrvInfosVulkan(this, it, this);
				it->second = info;
				if (created)
					*created = true;
			}
			else
			{
				info = static_cast<CTextureDrvInfosVulkan *>((ITextureDrvInfos *)it->second);
			}
			maybeDeleted = tex.TextureDrvShare->DrvTexture;
			tex.TextureDrvShare->DrvTexture = info;
		}
		return info;
	}

	if (!tex.TextureDrvShare->DrvTexture)
	{
		// Unshared texture: the infos dies with the ITexture. Passing a NULL
		// driver keeps it out of the share-name map.
		tex.TextureDrvShare->DrvTexture = new CTextureDrvInfosVulkan(NULL, ItTexDrvInfoPtrMap(), this);
		if (created)
			*created = true;
	}
	return static_cast<CTextureDrvInfosVulkan *>((ITextureDrvInfos *)tex.TextureDrvShare->DrvTexture);
}

CTextureDrvInfosVulkan *CDriverVulkan::findTextureDrvInfos(const ITexture &tex) const
{
	if (!tex.TextureDrvShare || !tex.TextureDrvShare->DrvTexture)
		return NULL;
	return static_cast<CTextureDrvInfosVulkan *>((ITextureDrvInfos *)tex.TextureDrvShare->DrvTexture);
}

CTextureVulkan *CDriverVulkan::getTextureVulkan(ITexture &tex)
{
	CTextureDrvInfosVulkan *info = getTextureDrvInfos(tex);
	if (info->Texture.Image == VK_NULL_HANDLE)
	{
		CTextureVulkan created;
		if (!uploadTextureVulkan(tex, created, true))
			return NULL;
		setTextureImage(*info, created);
	}
	return &info->Texture;
}

bool CDriverVulkan::isTextureExist(const ITexture &tex)
{
	const CTextureDrvInfosVulkan *info = findTextureDrvInfos(tex);
	return info != NULL && info->Texture.Image != VK_NULL_HANDLE;
}

bool CDriverVulkan::setupTexture(ITexture &tex)
{
	bool allUploaded = false;
	return setupTextureEx(tex, true, allUploaded, false);
}

bool CDriverVulkan::setupTextureEx(ITexture &tex, bool bUpload, bool &bAllUploaded, bool bMustRecreateSharedTexture)
{
	bAllUploaded = false;

	bool createdInfo = false;
	CTextureDrvInfosVulkan *info = getTextureDrvInfos(tex, &createdInfo);
	CTextureVulkan &vk = info->Texture;

	// Render targets are never uploaded from the CPU: their contents come from
	// rendering. setRenderTarget() touches the texture on every bind, so this
	// early-out is what keeps the rendered image from being overwritten.
	if (tex.getRenderTarget())
	{
		if (vk.Image != VK_NULL_HANDLE && vk.Width == tex.getWidth() && vk.Height == tex.getHeight())
		{
			tex.clearTouched();
			bAllUploaded = true;
			return true;
		}
		CTextureVulkan created;
		if (!uploadTextureVulkan(tex, created, false))
			return false;
		setTextureImage(*info, created);
		tex.clearTouched();
		bAllUploaded = true;
		return true;
	}

	// A shared texture found through its share name already holds the pixels
	// another ITexture uploaded. Loading this instance's bitmap would be pure
	// overhead (and its size is not even known before generate()), so leave the
	// image alone unless the caller explicitly wants it recreated.
	if (!createdInfo && tex.supportSharing() && !bMustRecreateSharedTexture)
	{
		tex.clearTouched();
		bAllUploaded = true;
		return true;
	}

	if (vk.Image != VK_NULL_HANDLE && !bMustRecreateSharedTexture && !tex.touched())
		return true;

	if (vk.Image != VK_NULL_HANDLE && !bMustRecreateSharedTexture &&
		vk.Width == tex.getWidth() && vk.Height == tex.getHeight())
	{
		// Same size: update the pixels in place so that nothing is destroyed
		// while commands reference it (animated textures hit this path a lot).
		if (bUpload)
		{
			tex.generate();

			// A texture that was fully invalidated, and any mipmapped texture
			// (touching it rebuilds the mip chain from the whole bitmap), needs
			// a complete upload. Otherwise only send the touched rectangles:
			// that is the hot path of the font atlas, which adds one glyph tile
			// at a time and must not have older tiles overwritten with stale
			// copies.
			if (tex.isAllInvalidated() || tex.mipMapOn())
			{
				if (!uploadTexturePixels(tex, vk))
					return false;
			}
			else
			{
				const list<CRect> &rects = tex._ListInvalidRect;
				for (list<CRect>::const_iterator r = rects.begin(); r != rects.end(); ++r)
				{
					if (!uploadTextureRegion(tex, vk, *r, 0))
						return false;
				}
			}
		}
		tex.clearTouched();
		bAllUploaded = bUpload;
		return true;
	}

	CTextureVulkan created;
	if (!uploadTextureVulkan(tex, created, bUpload))
		return false;
	setTextureImage(*info, created);
	tex.clearTouched();
	bAllUploaded = bUpload;
	return true;
}

bool CDriverVulkan::uploadTexture(ITexture &tex, CRect &rect, uint8 nNumMipMap)
{
	CTextureDrvInfosVulkan *info = getTextureDrvInfos(tex);

	// Create the image without pixels if this is the first use: the caller is
	// streaming one rectangle of a mip level in (shape bank / async texture
	// manager), so uploading the whole bitmap here would read regions that have
	// not been generated yet.
	if (info->Texture.Image == VK_NULL_HANDLE ||
		info->Texture.Width != tex.getWidth() || info->Texture.Height != tex.getHeight())
	{
		CTextureVulkan created;
		if (!uploadTextureVulkan(tex, created, false))
			return false;
		setTextureImage(*info, created);
	}

	return uploadTextureRegion(tex, info->Texture, rect, nNumMipMap);
}

bool CDriverVulkan::uploadTextureCube(ITexture &tex, CRect &rect, uint8 nNumMipMap, uint8 nNumFace)
{
	return setupTexture(tex);
}

uintptr_t CDriverVulkan::getTextureHandle(const ITexture &tex)
{
	const CTextureDrvInfosVulkan *info = findTextureDrvInfos(tex);
	if (!info)
		return 0;
	return (uintptr_t)info->Texture.Image;
}

void CDriverVulkan::swapTextureHandle(ITexture &tex0, ITexture &tex1)
{
	// Both textures must have driver data before their contents are swapped.
	bool allUploaded = false;
	setupTextureEx(tex0, true, allUploaded, false);
	setupTextureEx(tex1, true, allUploaded, false);
	CTextureDrvInfosVulkan *a = getTextureDrvInfos(tex0);
	CTextureDrvInfosVulkan *b = getTextureDrvInfos(tex1);
	swap(a->Texture, b->Texture);
	swap(a->TextureMemory, b->TextureMemory);
}

} /* namespace NLVULKAN */
