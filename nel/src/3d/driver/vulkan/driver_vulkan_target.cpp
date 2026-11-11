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
 * Render targets for the Vulkan driver.
 *
 * A NeL texture used as a render target (ITexture::getRenderTarget()) owns a
 * VkImage that is both the colour attachment of an offscreen VkRenderPass and
 * the sampled image every other draw sees. Each target gets a VkFramebuffer;
 * depth/stencil attachments are shared between the targets of the same size,
 * like GL3's CDepthStencilFBO cache.
 *
 * Only one render pass is recorded at a time on the frame's command buffer.
 * setRenderTarget() ends the current pass (the swapchain one or another
 * target's) and begins the destination one. Going back to the swapchain uses
 * a LOAD render pass with the same attachment formats as the first one, so the
 * scene drawn before the offscreen pass is kept and the cached pipelines stay
 * valid. Offscreen targets have their own RGBA8 pass and their own pipelines:
 * Vulkan pipelines are tied to the render pass colour format.
 *
 * The colour image is transitioned to SHADER_READ_ONLY_OPTIMAL when its pass
 * ends: that is the layout every sampled descriptor assumes, so the engine can
 * sample a target right after having rendered into it.
 */

#include "stdvulkan.h"
#include "driver_vulkan.h"

using namespace std;
using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

// ---------------------------------------------------------------------------
// Pass setup
// ---------------------------------------------------------------------------

/// True for the block-compressed (BC / DXTC) formats this driver creates, for
/// which vkCmdBlitImage is not supported: they can only be copied.
static bool compressedImageFormat(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
	case VK_FORMAT_BC2_UNORM_BLOCK:
	case VK_FORMAT_BC3_UNORM_BLOCK:
		return true;
	default:
		return false;
	}
}

VkExtent2D CDriverVulkan::currentTargetExtent() const
{
	VkExtent2D extent;
	if (_RenderTargetInfo && _RenderTargetInfo->Width && _RenderTargetInfo->Height)
	{
		extent.width = _RenderTargetInfo->Width;
		extent.height = _RenderTargetInfo->Height;
	}
	else
	{
		extent = _SwapchainExtent;
	}
	return extent;
}

VkViewport CDriverVulkan::computeViewport(VkExtent2D extent) const
{
	// NeL viewports are normalized (0..1) relative to the render target, with
	// the origin at the bottom left; Vulkan clips with the origin at the top.
	float x = _Viewport.getX() * extent.width;
	float y = _Viewport.getY() * extent.height;
	float w = _Viewport.getWidth() * extent.width;
	float h = _Viewport.getHeight() * extent.height;
	if (w <= 0.f)
		w = (float)extent.width;
	if (h <= 0.f)
		h = (float)extent.height;

	// Depth range, like GL's glDepthRange: setDepthRange() selects the window
	// depth range and the material zBias shifts it. Shadow receivers and decals
	// rely on a small negative bias to win the depth test against the surface
	// they are drawn over.
	float depthBias = _MaterialZBias * (_DepthRangeFar - _DepthRangeNear);
	float nearDepth = _DepthRangeNear + depthBias;
	float farDepth = _DepthRangeFar + depthBias;

	VkViewport vkViewport;
	vkViewport.x = x;
	vkViewport.y = (float)extent.height - y;
	vkViewport.width = w;
	vkViewport.height = -h; // GL-like bottom-up
	vkViewport.minDepth = std::min(std::max(nearDepth, 0.f), 1.f);
	vkViewport.maxDepth = std::min(std::max(farDepth, 0.f), 1.f);
	return vkViewport;
}

void CDriverVulkan::applyDepthRange(VkCommandBuffer cmd)
{
	VkViewport vkViewport = computeViewport(currentTargetExtent());
	vkCmdSetViewport(cmd, 0, 1, &vkViewport);
}

void CDriverVulkan::applyViewportScissor(VkCommandBuffer cmd, VkExtent2D extent)
{
	VkViewport vkViewport = computeViewport(extent);
	vkCmdSetViewport(cmd, 0, 1, &vkViewport);

	VkRect2D scissor;
	scissor.offset.x = (sint32)(_Scissor.X * extent.width);
	scissor.offset.y = (sint32)(extent.height - (_Scissor.Y + _Scissor.Height) * extent.height);
	scissor.extent.width = (uint32)(_Scissor.Width * extent.width);
	scissor.extent.height = (uint32)(_Scissor.Height * extent.height);
	if (scissor.extent.width == 0 || scissor.extent.height == 0)
	{
		scissor.offset.x = 0;
		scissor.offset.y = 0;
		scissor.extent = extent;
	}
	// The swapchain and the targets do not have the same aspect ratio, so a
	// full-screen scissor saved for one does not fit the other: clamp it.
	if (scissor.offset.x < 0)
		scissor.offset.x = 0;
	if (scissor.offset.y < 0)
		scissor.offset.y = 0;
	if (scissor.offset.x >= (sint32)extent.width)
		scissor.offset.x = 0;
	if (scissor.offset.y >= (sint32)extent.height)
		scissor.offset.y = 0;
	if (scissor.offset.x + (sint32)scissor.extent.width > (sint32)extent.width)
		scissor.extent.width = extent.width - scissor.offset.x;
	if (scissor.offset.y + (sint32)scissor.extent.height > (sint32)extent.height)
		scissor.extent.height = extent.height - scissor.offset.y;
	vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void CDriverVulkan::beginMainPass(VkCommandBuffer cmd, bool first)
{
	VkClearValue clears[2];
	memset(clears, 0, sizeof(clears));
	if (getenv("NLVK_FORCECLEAR"))
	{
		clears[0].color.float32[0] = 1.f;
		clears[0].color.float32[1] = 0.f;
		clears[0].color.float32[2] = 1.f;
		clears[0].color.float32[3] = 1.f;
	}
	else
	{
		clears[0].color.float32[0] = _ClearColor.R / 255.f;
		clears[0].color.float32[1] = _ClearColor.G / 255.f;
		clears[0].color.float32[2] = _ClearColor.B / 255.f;
		clears[0].color.float32[3] = _ClearColor.A / 255.f;
	}
	clears[1].depthStencil.depth = _ClearDepth;
	clears[1].depthStencil.stencil = (uint32)_ClearStencil;

	VkRenderPassBeginInfo rbi;
	memset(&rbi, 0, sizeof(rbi));
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = first ? _RenderPass : _ResumeRenderPass;
	rbi.framebuffer = _Framebuffers[_ImageIndex];
	rbi.renderArea.offset.x = 0;
	rbi.renderArea.offset.y = 0;
	rbi.renderArea.extent = _SwapchainExtent;
	rbi.clearValueCount = 2;
	rbi.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

	applyViewportScissor(cmd, _SwapchainExtent);
}

sint32 CDriverVulkan::getDepthTarget(uint32 width, uint32 height)
{
	for (uint i = 0; i < _DepthTargets.size(); ++i)
	{
		if (_DepthTargets[i].Width == width && _DepthTargets[i].Height == height &&
			_DepthTargets[i].Format == _DepthFormat)
			return (sint32)i;
	}

	CDepthTargetVulkan target;
	target.Format = _DepthFormat;
	target.Width = width;
	target.Height = height;

	VkImageCreateInfo ici;
	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.extent.width = width;
	ici.extent.height = height;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.format = _DepthFormat;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateImage(_Device, &ici, NULL, &target.Image) != VK_SUCCESS)
		return -1;

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(_Device, target.Image, &req);
	VkMemoryAllocateInfo mai;
	memset(&mai, 0, sizeof(mai));
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(_Device, &mai, NULL, &target.Memory) != VK_SUCCESS)
	{
		vkDestroyImage(_Device, target.Image, NULL);
		return -1;
	}
	vkBindImageMemory(_Device, target.Image, target.Memory, 0);

	VkImageViewCreateInfo vci;
	memset(&vci, 0, sizeof(vci));
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = target.Image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = _DepthFormat;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (_DepthFormat == VK_FORMAT_D24_UNORM_S8_UINT || _DepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
		vci.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	vci.subresourceRange.levelCount = 1;
	vci.subresourceRange.layerCount = 1;
	if (vkCreateImageView(_Device, &vci, NULL, &target.View) != VK_SUCCESS)
	{
		vkDestroyImage(_Device, target.Image, NULL);
		vkFreeMemory(_Device, target.Memory, NULL);
		return -1;
	}

	_DepthTargets.push_back(target);
	return (sint32)(_DepthTargets.size() - 1);
}

void CDriverVulkan::destroyDepthTargets()
{
	for (uint i = 0; i < _DepthTargets.size(); ++i)
	{
		CDepthTargetVulkan &target = _DepthTargets[i];
		if (target.View) vkDestroyImageView(_Device, target.View, NULL);
		if (target.Image) vkDestroyImage(_Device, target.Image, NULL);
		if (target.Memory) vkFreeMemory(_Device, target.Memory, NULL);
	}
	_DepthTargets.clear();
}

bool CDriverVulkan::createTargetFramebuffer(CTextureVulkan &target, VkRenderPass pass)
{
	sint32 depthIndex = getDepthTarget(target.Width, target.Height);
	if (depthIndex < 0)
		return false;
	target.DepthTarget = depthIndex;

	VkImageView views[2] = { target.View, _DepthTargets[depthIndex].View };
	VkFramebufferCreateInfo fci;
	memset(&fci, 0, sizeof(fci));
	fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fci.renderPass = pass;
	fci.attachmentCount = 2;
	fci.pAttachments = views;
	fci.width = target.Width;
	fci.height = target.Height;
	fci.layers = 1;
	if (vkCreateFramebuffer(_Device, &fci, NULL, &target.Framebuffer) != VK_SUCCESS)
	{
		target.Framebuffer = VK_NULL_HANDLE;
		target.DepthTarget = -1;
		return false;
	}
	return true;
}

bool CDriverVulkan::ensureRenderTarget(ITexture &tex, CTextureVulkan &target)
{
	uint32 width = tex.getWidth();
	uint32 height = tex.getHeight();
	if (width == 0 || height == 0)
		return false;

	const bool usable = target.Image != VK_NULL_HANDLE &&
		(target.Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
		target.Width == width && target.Height == height;

	if (!usable)
	{
		// The texture was set up before it was flagged as a render target, or
		// it changed size: its image has no colour-attachment usage, so it has
		// to be recreated. Whatever it held is replaced by rendered pixels.
		VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		CTextureVulkan created;
		if (!createTextureImage(width, height, 1, usage, NL_VK_TEXTURE_FORMAT, created))
			return false;
		CTextureDrvInfosVulkan *info = findTextureDrvInfos(tex);
		if (info)
			setTextureImage(*info, created);
		else
		{
			destroyTexture(target);
			target = created;
		}
		target.HasAlpha = true;
		target.SourceFormat = tex.getPixelFormat();
		nldebug("Vulkan: render target %ux%u created", width, height);
	}

	if (target.Framebuffer == VK_NULL_HANDLE &&
		!createTargetFramebuffer(target, _OffscreenRenderPass))
		return false;

	return true;
}

// ---------------------------------------------------------------------------
// Offscreen pass
// ---------------------------------------------------------------------------

void CDriverVulkan::beginOffscreenPass(VkCommandBuffer cmd, CTextureVulkan &target, VkExtent2D extent)
{
	// Make the colour image a render target again. It was left in
	// SHADER_READ_ONLY layout by endOffscreenPass() so that it could be sampled
	// in between. ALL_COMMANDS as a source scope covers every way the image
	// could have been used since then (draws, transfers).
	VkImageMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = target.Layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = target.Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = target.Layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	vkCmdPipelineBarrier(cmd,
		target.Layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	target.Layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	// A depth attachment is shared between all the targets of its size, so its
	// layout is already the attachment layout after the first use.
	if (target.DepthTarget >= 0 && (uint)target.DepthTarget < _DepthTargets.size())
	{
		CDepthTargetVulkan &depth = _DepthTargets[target.DepthTarget];
		if (!depth.Initialized)
		{
			memset(&barrier, 0, sizeof(barrier));
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = depth.Image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			if (depth.Format == VK_FORMAT_D24_UNORM_S8_UINT || depth.Format == VK_FORMAT_D32_SFLOAT_S8_UINT)
				barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				0, 0, NULL, 0, NULL, 1, &barrier);
			depth.Initialized = true;
		}
	}

	// Both attachments use LOAD: like a GL framebuffer, a target keeps what was
	// rendered into it until the engine clears it. The shadow-map pass clears
	// the area of each shadow map itself.
	VkRenderPassBeginInfo rbi;
	memset(&rbi, 0, sizeof(rbi));
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = _OffscreenRenderPass;
	rbi.framebuffer = target.Framebuffer;
	rbi.renderArea.offset.x = 0;
	rbi.renderArea.offset.y = 0;
	rbi.renderArea.extent = extent;
	rbi.clearValueCount = 0;
	rbi.pClearValues = NULL;
	vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

	applyViewportScissor(cmd, extent);
}

void CDriverVulkan::endOffscreenPass(VkCommandBuffer cmd, CTextureVulkan &target)
{
	vkCmdEndRenderPass(cmd);

	// Leave the image in the layout sampled descriptors declare, so that any
	// draw after setRenderTarget(NULL) can bind it without another barrier.
	VkImageMemoryBarrier barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = target.Layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = target.Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier);
	target.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ---------------------------------------------------------------------------
// IDriver render target interface
// ---------------------------------------------------------------------------

bool CDriverVulkan::setRenderTarget(ITexture *tex, uint32 x, uint32 y, uint32 width, uint32 height, uint32 mipmapLevel, uint32 cubeFace)
{
	if (!_Device)
		return false;

	if (tex && !tex->getRenderTarget())
	{
		nlwarning("Vulkan: setRenderTarget() on a texture that is not a render target");
		return false;
	}
	if (tex && (mipmapLevel != 0 || cubeFace != 0))
	{
		// Render-target mip levels and cube faces need one view per subresource;
		// no caller in the engine uses them yet.
		nlwarning("Vulkan: setRenderTarget(mip=%u, face=%u) is not supported", mipmapLevel, cubeFace);
		return false;
	}

	if (!tex)
	{
		// Back to the swapchain. Nothing to do when we are already there.
		if (!_RenderTargetInfo)
			return true;
		if (!_FrameOpen)
		{
			_RenderTargetTexture = NULL;
			_RenderTargetInfo = NULL;
			return true;
		}

		VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];
		CTextureVulkan *target = _RenderTargetInfo;
		endOffscreenPass(cmd, *target);
		_RenderTargetTexture = NULL;
		_RenderTargetInfo = NULL;

		// setRenderTarget() switched the viewport to the target: restore the
		// one the main target had when the target was bound.
		_Viewport = _SavedViewport;
		beginMainPass(cmd, false);
		return true;
	}

	CTextureVulkan *target = getTextureVulkan(*tex);
	if (!target || !ensureRenderTarget(*tex, *target))
		return false;

	if (!_FrameOpen && !beginFrame())
		return false;

	const uint32 targetWidth = tex->getWidth();
	const uint32 targetHeight = tex->getHeight();
	if (width == 0) width = targetWidth;
	if (height == 0) height = targetHeight;
	if (x >= targetWidth || y >= targetHeight)
		return false;
	width = min(width, targetWidth - x);
	height = min(height, targetHeight - y);

	VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];

	// Bind the same target again: keep the pass open so its contents survive.
	// The shadow-map pass re-issues setRenderTarget() for the same blur
	// texture between its blur iterations.
	if (target == _RenderTargetInfo)
	{
		CViewport vp;
		vp.init((float)x / targetWidth, (float)y / targetHeight,
			(float)width / targetWidth, (float)height / targetHeight);
		_Viewport = vp;
		applyViewportScissor(cmd, currentTargetExtent());
		return true;
	}

	if (_RenderTargetInfo)
	{
		endOffscreenPass(cmd, *_RenderTargetInfo);
	}
	else
	{
		// Leaving the main target: remember its viewport for the return trip.
		_SavedViewport = _Viewport;
		vkCmdEndRenderPass(cmd);
	}

	_RenderTargetTexture = tex;
	_RenderTargetInfo = target;
	++_RenderTargetBinds;
	if (!_RenderTargetLogged)
	{
		_RenderTargetLogged = true;
		nlinfo("Vulkan: rendering into offscreen target %ux%u", targetWidth, targetHeight);
	}

	VkExtent2D extent;
	extent.width = targetWidth;
	extent.height = targetHeight;
	beginOffscreenPass(cmd, *target, extent);

	// The engine usually calls setupViewport() with the region it wants, but a
	// target bound without one still has to render into the requested rect.
	CViewport vp;
	vp.init((float)x / targetWidth, (float)y / targetHeight,
		(float)width / targetWidth, (float)height / targetHeight);
	_Viewport = vp;
	applyViewportScissor(cmd, extent);
	return true;
}

bool CDriverVulkan::getRenderTargetSize(uint32 &width, uint32 &height)
{
	if (_RenderTargetInfo)
	{
		width = _RenderTargetInfo->Width;
		height = _RenderTargetInfo->Height;
	}
	else
	{
		getWindowSize(width, height);
	}
	// Same contract as the GL3 driver: the sizes are always valid, so callers
	// can use them even though the return value is false.
	return false;
}

bool CDriverVulkan::copyTargetToTexture(ITexture *tex, uint32 offsetx, uint32 offsety, uint32 x, uint32 y, uint32 width, uint32 height, uint32 mipmapLevel)
{
	if (!tex || !_RenderTargetInfo || !_FrameOpen)
		return false;
	if (mipmapLevel != 0)
		return false;

	CTextureVulkan *src = _RenderTargetInfo;
	CTextureVulkan *dst = getTextureVulkan(*tex);
	if (!dst || dst->Image == VK_NULL_HANDLE)
		return false;
	if (dst == src)
		return false;

	if (width == 0)
		width = src->Width > x ? src->Width - x : 0;
	if (height == 0)
		height = src->Height > y ? src->Height - y : 0;
	if (width == 0 || height == 0)
		return false;
	if (x + width > src->Width || y + height > src->Height ||
		offsetx + width > dst->Width || offsety + height > dst->Height)
		return false;

	VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];

	// Suspend the pass: an attachment cannot be a transfer source while it is
	// being rendered into.
	endOffscreenPass(cmd, *src);

	VkImageMemoryBarrier barriers[2];
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].oldLayout = src->Layout;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = src->Image;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[1].oldLayout = dst->Layout;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].image = dst->Image;
	barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[1].subresourceRange.levelCount = 1;
	barriers[1].subresourceRange.layerCount = 1;
	barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 2, barriers);
	src->Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	dst->Layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	// Both images are RGBA8, so a (possibly offset) copy is exact and cheaper
	// than a blit; offsets of 0 with equal sizes would also allow a blit, but
	// the formats have to match for vkCmdCopyImage. NeL rects have their origin
	// at the bottom left while the images are stored top down, hence the flip.
	{
		VkImageCopy region;
		memset(&region, 0, sizeof(region));
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.srcOffset.x = (sint32)x;
		region.srcOffset.y = (sint32)(src->Height - y - height);
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.layerCount = 1;
		region.dstOffset.x = (sint32)offsetx;
		region.dstOffset.y = (sint32)(dst->Height - offsety - height);
		region.extent.width = width;
		region.extent.height = height;
		region.extent.depth = 1;
		vkCmdCopyImage(cmd, src->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	// Put both images back where the rest of the driver expects them.
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = src->Image;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].image = dst->Image;
	barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[1].subresourceRange.levelCount = 1;
	barriers[1].subresourceRange.layerCount = 1;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 0, NULL, 0, NULL, 2, barriers);
	src->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dst->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// Resume the target's pass so the engine can keep drawing into it.
	VkExtent2D extent;
	extent.width = src->Width;
	extent.height = src->Height;
	beginOffscreenPass(cmd, *src, extent);
	applyViewportScissor(cmd, extent);
	return true;
}

// ---------------------------------------------------------------------------
// stretchRect()
// ---------------------------------------------------------------------------

namespace /* anonymous */ {

/// NeL rectangles have their origin at the bottom left; images are stored top
/// down. Clamp a rect to an image and return the blit region.
static bool makeBlitRegion(const CRect &rect, uint32 imageWidth, uint32 imageHeight, VkImageBlit &blit, bool source)
{
	sint32 x = rect.X;
	sint32 y = rect.Y;
	sint32 w = (sint32)rect.Width;
	sint32 h = (sint32)rect.Height;
	if (w == 0)
	{
		x = 0;
		w = (sint32)imageWidth;
	}
	if (h == 0)
	{
		y = 0;
		h = (sint32)imageHeight;
	}
	if (x < 0)
	{
		w += x;
		x = 0;
	}
	if (y < 0)
	{
		h += y;
		y = 0;
	}
	if (x >= (sint32)imageWidth || y >= (sint32)imageHeight)
		return false;
	if (x + w > (sint32)imageWidth)
		w = (sint32)imageWidth - x;
	if (y + h > (sint32)imageHeight)
		h = (sint32)imageHeight - y;
	if (w <= 0 || h <= 0)
		return false;
	// Top-down image rows.
	const sint32 top = (sint32)imageHeight - (y + h);
	if (source)
	{
		blit.srcOffsets[0].x = x;
		blit.srcOffsets[0].y = top;
		blit.srcOffsets[0].z = 0;
		blit.srcOffsets[1].x = x + w;
		blit.srcOffsets[1].y = top + h;
		blit.srcOffsets[1].z = 1;
	}
	else
	{
		blit.dstOffsets[0].x = x;
		blit.dstOffsets[0].y = top;
		blit.dstOffsets[0].z = 0;
		blit.dstOffsets[1].x = x + w;
		blit.dstOffsets[1].y = top + h;
		blit.dstOffsets[1].z = 1;
	}
	return true;
}

} /* anonymous namespace */

bool CDriverVulkan::stretchRectImage(ITexture *srcText, CRect &srcRect, ITexture *destText, CRect &destRect)
{
	if (!_Device || !srcText || !destText || srcText == destText)
		return false;
	// getTextureVulkan() generates and uploads the texture on first use, so the
	// sizes and formats are only meaningful after it. Mip levels and cube faces
	// have no caller here: a whole 2D image is copied.
	CTextureVulkan *src = getTextureVulkan(*srcText);
	CTextureVulkan *dst = getTextureVulkan(*destText);
	if (!src || !dst || src->Image == VK_NULL_HANDLE || dst->Image == VK_NULL_HANDLE ||
		src->Width == 0 || src->Height == 0 || dst->Width == 0 || dst->Height == 0)
		return false;

	static bool logged = false;
	if (!logged)
	{
		logged = true;
		nlinfo("Vulkan: stretchRect %ux%u -> %ux%u (scaled blit)", src->Width, src->Height, dst->Width, dst->Height);
	}
	if (!(src->Usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(dst->Usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		return false;

	// vkCmdBlitImage requires both images to have the same format, and no driver
	// reports VK_FORMAT_FEATURE_BLIT_SRC_BIT / _DST_BIT for a BC format: a
	// compressed texture can be copied but not scaled. Returning false makes the
	// engine fall back to its textured quad (CDriverUser::stretchRect), which
	// handles any format, so a compressed source stays correct.
	const bool compressed = compressedImageFormat(src->Format);
	if (src->Format != dst->Format)
	{
		nlwarning("Vulkan: stretchRect from format %d to %d: vkCmdBlitImage needs matching formats, using the quad fallback",
			(int)src->Format, (int)dst->Format);
		return false;
	}

	VkImageBlit blit;
	memset(&blit, 0, sizeof(blit));
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	if (!makeBlitRegion(srcRect, src->Width, src->Height, blit, true) ||
		!makeBlitRegion(destRect, dst->Width, dst->Height, blit, false))
		return false;

	// Same-size, same-place rects are a copy: cheaper than a blit, and the only
	// thing a BC image can do (block-aligned offsets are guaranteed by the
	// caller passing whole blocks).
	const bool sameRect = src->Width == dst->Width && src->Height == dst->Height &&
		memcmp(blit.srcOffsets, blit.dstOffsets, sizeof(blit.srcOffsets)) == 0;
	const bool blockAligned = (blit.srcOffsets[0].x % 4) == 0 && (blit.srcOffsets[0].y % 4) == 0 &&
		(blit.srcOffsets[1].x % 4) == 0 && (blit.srcOffsets[1].y % 4) == 0;
	if (compressed && (!sameRect || !blockAligned))
	{
		nlwarning("Vulkan: stretchRect cannot scale a compressed (format %d) image; using the quad fallback",
			(int)src->Format);
		return false;
	}

	if (!_FrameOpen && !beginFrame())
		return false;
	VkCommandBuffer cmd = _CommandBuffers[_FrameIndex];

	// A blit is not allowed inside a render pass, and an attachment cannot be
	// a transfer source or destination. Suspend the open pass and restore it
	// afterwards; the engine calls stretchRect() in the middle of a frame.
	const bool resumeTarget = _RenderTargetInfo != NULL;
	if (resumeTarget)
		endOffscreenPass(cmd, *_RenderTargetInfo);
	else
		vkCmdEndRenderPass(cmd);

	VkImageMemoryBarrier barriers[2];
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].oldLayout = src->Layout;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = src->Image;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].srcAccessMask = src->Layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[1].oldLayout = dst->Layout;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].image = dst->Image;
	barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[1].subresourceRange.levelCount = 1;
	barriers[1].subresourceRange.layerCount = 1;
	barriers[1].srcAccessMask = dst->Layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd,
		src->Layout == VK_IMAGE_LAYOUT_UNDEFINED && dst->Layout == VK_IMAGE_LAYOUT_UNDEFINED
			? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
	src->Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	dst->Layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	// Filtered blit: the bloom effect stretches the scene target into its fixed
	// 256x256 blur target, so source and destination sizes usually differ. A
	// compressed image is copied instead: the formats match and the rects are
	// identical, which is what a copy requires.
	if (compressed)
	{
		VkImageCopy region;
		memset(&region, 0, sizeof(region));
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.srcOffset = blit.srcOffsets[0];
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.layerCount = 1;
		region.dstOffset = blit.dstOffsets[0];
		region.extent.width = (uint32)(blit.srcOffsets[1].x - blit.srcOffsets[0].x);
		region.extent.height = (uint32)(blit.srcOffsets[1].y - blit.srcOffsets[0].y);
		region.extent.depth = 1;
		vkCmdCopyImage(cmd, src->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}
	else
	{
		vkCmdBlitImage(cmd, src->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	}

	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = src->Image;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].image = dst->Image;
	barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[1].subresourceRange.levelCount = 1;
	barriers[1].subresourceRange.layerCount = 1;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 0, NULL, 0, NULL, 2, barriers);
	src->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dst->Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// Resume the pass that was open when stretchRect() was called.
	if (resumeTarget)
	{
		VkExtent2D extent;
		extent.width = _RenderTargetInfo->Width;
		extent.height = _RenderTargetInfo->Height;
		beginOffscreenPass(cmd, *_RenderTargetInfo, extent);
		applyViewportScissor(cmd, extent);
	}
	else
	{
		beginMainPass(cmd, false);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Compressed stretchRect self-test (NLVK_TEXTEST=1)
// ---------------------------------------------------------------------------
//
// vkCmdBlitImage cannot read or write a block-compressed image, so stretchRect()
// copies two compressed images (same format, same rect) and refuses to scale
// them, which makes CDriverUser draw its textured quad instead. Bloom and FXAA
// only ever stretch RGBA render targets, so this one-shot test is the only
// in-game coverage of the compressed path: it builds a solid red DXT1 texture,
// copies it into another DXT1 texture through stretchRect(), asks for a scaled
// copy as well (which must be refused), reads the destination image back and
// checks that the copy landed.
void CDriverVulkan::runTextureBlitTest()
{
	if (_BlitTestStage != 0 || !_FrameOpen || _SwapBufferCounter < 2)
		return;
	_BlitTestStage = 1;
	if (!_SupportsBC)
	{
		nlinfo("Vulkan TEXTEST: skipped, the device has no BC texture support");
		return;
	}

	// 64x64 DXT1, every 4x4 block solid red: colour0 = 0xF800, indices 0.
	const uint32 size = 64;
	const uint32 blockBytes = 8;
	uint32 bytes = (size / 4) * (size / 4) * blockBytes;
	uint8 *srcBlocks = new uint8[bytes];
	uint8 *dstBlocks = new uint8[bytes];
	for (uint32 i = 0; i < bytes; i += blockBytes)
	{
		srcBlocks[i + 0] = 0x00;	// colour0 low byte
		srcBlocks[i + 1] = 0xF8;	// colour0 high byte
		srcBlocks[i + 2] = 0x00;	// colour1
		srcBlocks[i + 3] = 0x00;
		memset(srcBlocks + i + 4, 0, 4);	// indices: all colour0
	}
	memset(dstBlocks, 0, bytes);
	_BlitTestSrc.setPointer(srcBlocks, bytes, true, false, size, size, CBitmap::DXTC1);
	_BlitTestDst.setPointer(dstBlocks, bytes, true, false, size, size, CBitmap::DXTC1);

	CTextureVulkan *srcImage = getTextureVulkan(_BlitTestSrc);
	if (!srcImage || !srcImage->Compressed)
	{
		nlinfo("Vulkan TEXTEST: skipped, the DXT1 texture was not uploaded as a BC image");
		return;
	}

	CRect full(0, 0, size, size);
	CRect half(0, 0, size / 2, size / 2);
	const bool copied = stretchRect(&_BlitTestSrc, full, &_BlitTestDst, full);
	const bool scaled = stretchRect(&_BlitTestSrc, full, &_BlitTestDst, half);
	_BlitTestRequestOk = copied && !scaled;
	nlinfo("Vulkan TEXTEST: stretchRect of a BC1 texture: same size -> %s, scaled -> %s (expected true, false)",
		copied ? "true" : "false", scaled ? "true" : "false");
}

void CDriverVulkan::checkTextureBlitTest()
{
	// The copy was recorded in frame N and is proven finished when beginFrame()
	// waits for the same frame slot again, two frames later.
	if (_BlitTestStage != 1 || _SwapBufferCounter < 4)
		return;
	_BlitTestStage = 2;
	if (!_BlitTestRequestOk)
	{
		nlinfo("Vulkan TEXTEST: FAIL, the compressed stretchRect paths did not answer as expected");
		return;
	}
	CTextureVulkan *dst = getTextureVulkan(_BlitTestDst);
	if (!dst || dst->Image == VK_NULL_HANDLE || !dst->Compressed)
	{
		nlinfo("Vulkan TEXTEST: FAIL, the destination image does not exist");
		return;
	}

	// Read level 0 back and look at the first block.
	const uint32 readSize = 2048;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void *mapped = NULL;
	buffer = createStagingBuffer(readSize, &memory, &mapped, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	if (buffer == VK_NULL_HANDLE)
	{
		nlinfo("Vulkan TEXTEST: FAIL, could not allocate the readback buffer");
		return;
	}
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
	barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = dst->Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier);
	VkBufferImageCopy region;
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = dst->Width;
	region.imageExtent.height = dst->Height;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(cmd, dst->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si;
	memset(&si, 0, sizeof(si));
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(_GraphicsQueue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(_GraphicsQueue);
	vkFreeCommandBuffers(_Device, _CommandPool, 1, &cmd);

	const uint8 *pixels = (const uint8 *)mapped;
	const bool red = pixels[0] == 0x00 && pixels[1] == 0xF8;
	nlinfo("Vulkan TEXTEST: %s, destination block colour0 = 0x%02x%02x (expected 0xF800)",
		red ? "PASS" : "FAIL", (unsigned)pixels[1], (unsigned)pixels[0]);
	vkDestroyBuffer(_Device, buffer, NULL);
	vkFreeMemory(_Device, memory, NULL);
}

ITexture *CDriverVulkan::getRenderTarget() const
{
	return _RenderTargetTexture;
}

} /* namespace NLVULKAN */
