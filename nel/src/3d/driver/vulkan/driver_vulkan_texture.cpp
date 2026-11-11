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

using namespace std;
using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static VkBuffer createStagingBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size,
	VkDeviceMemory *memory, void **mapped)
{
	VkBufferCreateInfo bci;
	memset(&bci, 0, sizeof(bci));
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(device, &bci, NULL, &buffer) != VK_SUCCESS)
		return VK_NULL_HANDLE;

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, buffer, &req);

	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
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
		vkDestroyBuffer(device, buffer, NULL);
		return VK_NULL_HANDLE;
	}

	VkMemoryAllocateInfo mai;
	memset(&mai, 0, sizeof(mai));
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = typeIndex;
	if (vkAllocateMemory(device, &mai, NULL, memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(device, buffer, NULL);
		return VK_NULL_HANDLE;
	}
	vkBindBufferMemory(device, buffer, *memory, 0);
	if (mapped)
		vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
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

/// Create the image, memory, view and sampler of a texture. Used for both
/// uploaded textures and render targets: the only difference is the usage
/// flags (render targets add COLOR_ATTACHMENT).
bool CDriverVulkan::createTextureImage(uint32 width, uint32 height, uint mipCount,
	VkImageUsageFlags usage, CTextureVulkan &out)
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
	ici.format = NL_VK_TEXTURE_FORMAT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.usage = usage;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateImage(_Device, &ici, NULL, &out.Image) != VK_SUCCESS)
		return false;

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(_Device, out.Image, &req);
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
	vci.format = NL_VK_TEXTURE_FORMAT;
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
	out.Layers = 1;
	out.Usage = usage;
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
		staging = createStagingBuffer(_Device, _PhysicalDevice, 4, &stagingMemory, &mapped);
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

	uint mipCount = 1;
	if (!renderTarget && tex.mipMapOn())
	{
		tex.buildMipMaps();
		mipCount = max(1u, (uint)tex.getMipMapCount());
	}

	// TRANSFER_SRC is always on so any texture can be the source of
	// stretchRect()'s vkCmdBlitImage (the engine also stretches plain textures).
	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (renderTarget)
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (!createTextureImage(width, height, mipCount, usage, out))
		return false;

	out.HasAlpha = true;
	out.SourceFormat = tex.getPixelFormat();

	if (renderTarget)
		return true;

	if (out.SourceFormat == CBitmap::DXTC1 || out.SourceFormat == CBitmap::DXTC1Alpha ||
		out.SourceFormat == CBitmap::DXTC3 || out.SourceFormat == CBitmap::DXTC5)
		nldebug("Vulkan: compressed texture %ux%u format=%d (%s) decompressed on upload",
			width, height, (int)out.SourceFormat, typeid(tex).name());

	if (!upload)
	{
		// No pixels yet (a render target or a texture the caller streams in
		// with uploadTexture()/touchRect()). The image view covers every mip
		// level and the sampler may read any of them, while a region upload
		// only ever transitions the level it writes, so put the whole image in
		// the shader-read layout now. The contents are undefined either way.
		bool batched = false;
		VkCommandBuffer cmd = beginTextureCopy(out.Image, true, batched);
		if (cmd == VK_NULL_HANDLE)
			return false;
		VkImageMemoryBarrier barrier;
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = out.Image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = mipCount;
		barrier.subresourceRange.layerCount = 1;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			0, 0, NULL, 0, NULL, 1, &barrier);
		out.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		out.Uploaded = true;
		return endTextureCopy(cmd, batched, out.Image, VK_NULL_HANDLE, VK_NULL_HANDLE);
	}

	return uploadTexturePixels(tex, out, true);
}

void CDriverVulkan::freeUploadStaging()
{
	for (uint i = 0; i < _UploadStaging.size(); ++i)
	{
		vkDestroyBuffer(_Device, _UploadStaging[i].first, NULL);
		vkFreeMemory(_Device, _UploadStaging[i].second, NULL);
	}
	_UploadStaging.clear();
}

bool CDriverVulkan::beginUploadBatch()
{
	// The previous batch may still be executing: wait for its fence (a tiny
	// copy-only submission, usually already done) before reusing the command
	// buffer and freeing its staging buffers.
	if (_UploadSubmitted)
	{
		vkWaitForFences(_Device, 1, &_UploadFence, VK_TRUE, UINT64_MAX);
		_UploadSubmitted = false;
		freeUploadStaging();
	}
	if (_UploadRecording)
		return true;

	_UploadBatchImages.clear();
	VkCommandBufferBeginInfo bi;
	memset(&bi, 0, sizeof(bi));
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(_UploadCommandBuffer, &bi) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkBeginCommandBuffer(upload) failed");
		return false;
	}
	_UploadRecording = true;
	return true;
}

bool CDriverVulkan::submitUploadBatch()
{
	if (!_UploadRecording)
		return false;
	_UploadRecording = false;
	if (vkEndCommandBuffer(_UploadCommandBuffer) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkEndCommandBuffer(upload) failed");
		return false;
	}

	vkResetFences(_Device, 1, &_UploadFence);
	VkSubmitInfo si;
	memset(&si, 0, sizeof(si));
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &_UploadDone;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &_UploadCommandBuffer;
	if (vkQueueSubmit(_GraphicsQueue, 1, &si, _UploadFence) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkQueueSubmit(upload) failed");
		return false;
	}
	_UploadSubmitted = true;
	// The batch is closed: from now on the images it holds are read-only to the
	// frame path until the fence signals (see isImageInUploadBatch()).
	_UploadBatchImages.clear();
	return true;
}

bool CDriverVulkan::isImageInUploadBatch(VkImage image) const
{
	for (size_t i = 0; i < _UploadBatchImages.size(); ++i)
		if (_UploadBatchImages[i] == image)
			return true;
	return false;
}

VkCommandBuffer CDriverVulkan::beginTextureCopy(VkImage image, bool firstTime, bool &deferred)
{
	// First-time uploads target a brand-new image that no in-flight frame can
	// reference, so they are recorded into the shared upload batch and
	// submitted once for the whole frame. An in-place update of an image is
	// synchronous instead: a frame that is still executing may be sampling the
	// image we are about to overwrite. The exception is an image that already
	// has a copy in this frame's batch: it must join the batch too, so that the
	// layout transitions execute in recording order.
	deferred = firstTime || isImageInUploadBatch(image);
	if (deferred)
		return beginUploadBatch() ? _UploadCommandBuffer : VK_NULL_HANDLE;

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
	return cmd;
}

bool CDriverVulkan::endTextureCopy(VkCommandBuffer cmd, bool deferred, VkImage image,
	VkBuffer staging, VkDeviceMemory stagingMemory)
{
	if (deferred)
	{
		// The staging buffer survives until the upload batch fence signals and
		// the next batch starts (see beginUploadBatch()). A layout-only copy
		// passes VK_NULL_HANDLE for both.
		if (staging != VK_NULL_HANDLE)
			_UploadStaging.push_back(std::make_pair(staging, stagingMemory));
		if (!isImageInUploadBatch(image))
			_UploadBatchImages.push_back(image);
		return true;
	}

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
	{
		nlwarning("Vulkan: vkEndCommandBuffer(texture) failed");
		return false;
	}
	VkSubmitInfo si;
	memset(&si, 0, sizeof(si));
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(_GraphicsQueue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(_GraphicsQueue);

	vkFreeCommandBuffers(_Device, _CommandPool, 1, &cmd);
	if (staging != VK_NULL_HANDLE)
	{
		vkDestroyBuffer(_Device, staging, NULL);
		vkFreeMemory(_Device, stagingMemory, NULL);
	}
	return true;
}

bool CDriverVulkan::uploadTexturePixels(ITexture &tex, CTextureVulkan &out, bool deferred)
{
	uint mipCount = 1;
	if (out.Image == VK_NULL_HANDLE)
		return false;

	// Mip count is implied by the image; re-derive it from the texture.
	if (tex.mipMapOn())
		mipCount = max(1u, (uint)tex.getMipMapCount());

	// Read the bitmap in its own (uncompressed) format; see the note above.
	if (!prepareTexturePixels(tex))
		return false;

	// Build the staging buffer and record all mip copies in one submit.
	VkDeviceSize totalSize = 0;
	for (uint i = 0; i < mipCount; ++i)
		totalSize += (VkDeviceSize)tex.getWidth(i) * tex.getHeight(i) * 4;

	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	void *mapped = NULL;
	staging = createStagingBuffer(_Device, _PhysicalDevice, totalSize, &stagingMemory, &mapped);
	if (staging == VK_NULL_HANDLE)
		return false;

	VkDeviceSize offset = 0;
	VkBufferImageCopy regions[16];
	memset(regions, 0, sizeof(regions));
	uint regionCount = 0;
	for (uint i = 0; i < mipCount && regionCount < 16; ++i)
	{
		uint w = tex.getWidth(i);
		uint h = tex.getHeight(i);
		VkDeviceSize size = (VkDeviceSize)w * h * 4;
		copyTextureRect(tex, i, CRect(0, 0, w, h), (uint8 *)mapped + offset);
		regions[regionCount].bufferOffset = offset;
		regions[regionCount].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		regions[regionCount].imageSubresource.mipLevel = i;
		regions[regionCount].imageSubresource.layerCount = 1;
		regions[regionCount].imageExtent.width = w;
		regions[regionCount].imageExtent.height = h;
		regions[regionCount].imageExtent.depth = 1;
		offset += size;
		++regionCount;
	}

	// Record the copies, either into the per-frame batch (a brand-new image) or
	// into a one-off command buffer submitted and waited for here.
	bool batched = false;
	VkCommandBuffer cmd = beginTextureCopy(out.Image, deferred, batched);
	if (cmd == VK_NULL_HANDLE)
	{
		vkDestroyBuffer(_Device, staging, NULL);
		vkFreeMemory(_Device, stagingMemory, NULL);
		return false;
	}

	VkImageMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = out.Layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = out.Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = mipCount;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd,
		barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	vkCmdCopyBufferToImage(cmd, staging, out.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regionCount, regions);
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier);
	out.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	out.Uploaded = true;
	return endTextureCopy(cmd, batched, out.Image, staging, stagingMemory);
}

bool CDriverVulkan::uploadTextureRegion(ITexture &tex, CTextureVulkan &out, const NLMISC::CRect &rect, uint mipLevel)
{
	if (out.Image == VK_NULL_HANDLE || tex.getWidth(mipLevel) == 0 || tex.getHeight(mipLevel) == 0)
		return false;

	CRect clipped = rect;
	if (!clampTextureRect(clipped, tex.getWidth(mipLevel), tex.getHeight(mipLevel)))
		return true; // fully outside the mip: nothing to update

	// See the note in prepareTexturePixels(): keep Alpha/Luminance textures as
	// they are, so CTextureFont can keep drawing into its atlas.
	if (!prepareTexturePixels(tex))
		return false;

	const VkDeviceSize size = (VkDeviceSize)clipped.Width * clipped.Height * 4;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	void *mapped = NULL;
	staging = createStagingBuffer(_Device, _PhysicalDevice, size, &stagingMemory, &mapped);
	if (staging == VK_NULL_HANDLE)
		return false;
	copyTextureRect(tex, mipLevel, clipped, (uint8 *)mapped);

	// The first write to a brand-new image is still a first-time copy, so it
	// can ride the per-frame upload batch; anything else updates an image an
	// in-flight frame may be sampling and stays synchronous.
	bool batched = false;
	VkCommandBuffer cmd = beginTextureCopy(out.Image, out.Layout == VK_IMAGE_LAYOUT_UNDEFINED, batched);
	if (cmd == VK_NULL_HANDLE)
	{
		vkDestroyBuffer(_Device, staging, NULL);
		vkFreeMemory(_Device, stagingMemory, NULL);
		return false;
	}

	// Unlike a full upload this must preserve the rest of the image, so the old
	// layout is the one the image is actually in (UNDEFINED on first touch).
	VkImageMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = out.Layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = out.Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseMipLevel = mipLevel;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = out.Layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd,
		out.Layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	VkBufferImageCopy region;
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = mipLevel;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x = clipped.X;
	region.imageOffset.y = clipped.Y;
	region.imageExtent.width = clipped.Width;
	region.imageExtent.height = clipped.Height;
	region.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(cmd, staging, out.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	out.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	out.Uploaded = true;
	return endTextureCopy(cmd, batched, out.Image, staging, stagingMemory);
}

CTextureVulkan *CDriverVulkan::getTextureVulkan(ITexture &tex)
{
	map<ITexture *, CTextureVulkan>::iterator it = _TexturesVulkan.find(&tex);
	if (it != _TexturesVulkan.end())
		return &it->second;

	CTextureVulkan created;
	if (!uploadTextureVulkan(tex, created, true))
		return NULL;
	pair<map<ITexture *, CTextureVulkan>::iterator, bool> inserted = _TexturesVulkan.insert(make_pair(&tex, created));
	return &inserted.first->second;
}

bool CDriverVulkan::isTextureExist(const ITexture &tex)
{
	return _TexturesVulkan.find(const_cast<ITexture *>(&tex)) != _TexturesVulkan.end();
}

bool CDriverVulkan::setupTexture(ITexture &tex)
{
	bool allUploaded = false;
	return setupTextureEx(tex, true, allUploaded, false);
}

bool CDriverVulkan::setupTextureEx(ITexture &tex, bool bUpload, bool &bAllUploaded, bool bMustRecreateSharedTexture)
{
	bAllUploaded = false;

	map<ITexture *, CTextureVulkan>::iterator it = _TexturesVulkan.find(&tex);

	// Render targets are never uploaded from the CPU: their contents come from
	// rendering. setRenderTarget() touches the texture on every bind, so this
	// early-out is what keeps the rendered image from being overwritten.
	if (tex.getRenderTarget())
	{
		if (it != _TexturesVulkan.end() && it->second.Image != VK_NULL_HANDLE &&
			it->second.Width == tex.getWidth() && it->second.Height == tex.getHeight())
		{
			tex.clearTouched();
			bAllUploaded = true;
			return true;
		}
		CTextureVulkan created;
		if (!uploadTextureVulkan(tex, created, false))
			return false;
		if (it != _TexturesVulkan.end())
		{
			destroyTexture(it->second);
			it->second = created;
		}
		else
		{
			_TexturesVulkan.insert(make_pair(&tex, created));
		}
		tex.clearTouched();
		bAllUploaded = true;
		return true;
	}

	if (it != _TexturesVulkan.end() && !bMustRecreateSharedTexture && !tex.touched())
		return true;

	if (it != _TexturesVulkan.end() && it->second.Image != VK_NULL_HANDLE &&
		it->second.Width == tex.getWidth() && it->second.Height == tex.getHeight())
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
				if (!uploadTexturePixels(tex, it->second, false))
					return false;
			}
			else
			{
				const list<CRect> &rects = tex._ListInvalidRect;
				for (list<CRect>::const_iterator r = rects.begin(); r != rects.end(); ++r)
				{
					if (!uploadTextureRegion(tex, it->second, *r, 0))
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
	if (it != _TexturesVulkan.end())
	{
		destroyTexture(it->second);
		it->second = created;
	}
	else
	{
		_TexturesVulkan.insert(make_pair(&tex, created));
	}
	tex.clearTouched();
	bAllUploaded = bUpload;
	return true;
}

bool CDriverVulkan::uploadTexture(ITexture &tex, CRect &rect, uint8 nNumMipMap)
{
	map<ITexture *, CTextureVulkan>::iterator it = _TexturesVulkan.find(&tex);

	// Create the image without pixels if this is the first use: the caller is
	// streaming one rectangle of a mip level in (shape bank / async texture
	// manager), so uploading the whole bitmap here would read regions that have
	// not been generated yet.
	if (it == _TexturesVulkan.end() || it->second.Image == VK_NULL_HANDLE ||
		it->second.Width != tex.getWidth() || it->second.Height != tex.getHeight())
	{
		CTextureVulkan created;
		if (!uploadTextureVulkan(tex, created, false))
			return false;
		if (it != _TexturesVulkan.end())
		{
			destroyTexture(it->second);
			it->second = created;
		}
		else
		{
			it = _TexturesVulkan.insert(make_pair(&tex, created)).first;
		}
	}

	return uploadTextureRegion(tex, it->second, rect, nNumMipMap);
}

bool CDriverVulkan::uploadTextureCube(ITexture &tex, CRect &rect, uint8 nNumMipMap, uint8 nNumFace)
{
	return setupTexture(tex);
}

uintptr_t CDriverVulkan::getTextureHandle(const ITexture &tex)
{
	map<ITexture *, CTextureVulkan>::const_iterator it = _TexturesVulkan.find(const_cast<ITexture *>(&tex));
	if (it == _TexturesVulkan.end())
		return 0;
	return (uintptr_t)it->second.Image;
}

void CDriverVulkan::swapTextureHandle(ITexture &tex0, ITexture &tex1)
{
	swap(_TexturesVulkan[&tex0], _TexturesVulkan[&tex1]);
}

} /* namespace NLVULKAN */
