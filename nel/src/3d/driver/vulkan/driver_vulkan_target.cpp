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

void CDriverVulkan::applyViewportScissor(VkCommandBuffer cmd, VkExtent2D extent)
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

	VkViewport vkViewport;
	vkViewport.x = x;
	vkViewport.y = (float)extent.height - y;
	vkViewport.width = w;
	vkViewport.height = -h; // GL-like bottom-up
	vkViewport.minDepth = 0.f;
	vkViewport.maxDepth = 1.f;
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
		destroyTexture(target);
		VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (!createTextureImage(width, height, 1, usage, target))
			return false;
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

ITexture *CDriverVulkan::getRenderTarget() const
{
	return _RenderTargetTexture;
}

} /* namespace NLVULKAN */
