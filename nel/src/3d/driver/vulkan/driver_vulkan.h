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

#ifndef NL_DRIVER_VULKAN_H
#define NL_DRIVER_VULKAN_H

#include "stdvulkan.h"

#include "driver_vulkan_mega.h"

#include "nel/3d/driver.h"
#include "nel/3d/driver_user.h"
#include "nel/3d/vertex_buffer.h"
#include "nel/3d/index_buffer.h"
#include "nel/3d/material.h"
#include "nel/3d/texture.h"
#include "nel/3d/texture_cube.h"
#include "nel/3d/vertex_program.h"
#include "nel/3d/pixel_program.h"
#include "nel/3d/program.h"
#include "nel/3d/uniform_buffer.h"
#include "nel/3d/texture_mem.h"
#include "nel/misc/bitmap.h"
#include "nel/misc/event_emitter.h"

#if defined(NL_OS_UNIX) && !defined(NL_OS_MAC)
#include "unix_event_emitter.h"
#endif

#include <cstring>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <vector>

namespace NLVULKAN {

using namespace NL3D;
using namespace NLMISC;

class CDriverVulkan;

/// Texture upload staging ring sizes. The initial size is what a spot load
/// needs; the loading phase can push one frame past it (tens of MiB of
/// textures in a single frame) and the ring doubles up to the maximum. In game
/// the traffic is tiny, so the ring is shrunk back once it stays below a
/// quarter of the initial size (see CDriverVulkan::swapBuffers()).
const uint32 NL_VK_STAGING_RING_INITIAL = 8u * 1024u * 1024u;
const uint32 NL_VK_STAGING_RING_MAX = 64u * 1024u * 1024u;

/// Driver-side data of a compiled user vertex program.
///
/// A program is either a nelvp assembly program converted to Vulkan GLSL by
/// this driver, or a GLSL vertex program provided by the engine (the wind
/// tree, vegetable and landscape UBO programs). Both are compiled to a
/// VkShaderModule that replaces the mega vertex stage for the draws made
/// while the program is active; the mega pixel stage keeps drawing.
class CProgramDrvInfosVulkan : public IProgramDrvInfos
{
public:
	CProgramDrvInfosVulkan(CDriverVulkan *drv, ItGPUPrgDrvInfoPtrList it);
	virtual ~CProgramDrvInfosVulkan();
	virtual uint getUniformIndex(const char *name) const NL_OVERRIDE;

	CDriverVulkan					*_VkDriver;
	/// True for a nelvp program converted by convertNelvpToGLSL(): uniform
	/// writes are redirected into NelvpConstantUB at the engine's user VP
	/// binding instead of being ignored.
	bool							IsNelvpConverted;
	/// Name -> constant register map carried over from the nelvp source.
	std::map<std::string, uint>		ParamIndices;
	/// Constant registers in NlNelvpConstants (multiple of 4).
	uint32							RegisterCount;
	/// Constant register UBO, owned by this object (nelvp programs only).
	NLMISC::CRefPtr<CUniformBuffer>	NelvpConstantUB;
	/// Stable identity of the compiled shader in the pipeline cache.
	uint32							Id;
	/// Engine display name, for logs.
	std::string						DisplayName;
	/// True once the first draw using this program has been logged.
	bool							FirstDrawLogged;
	/// True when the program writes the specular colour varying; selects the
	/// mega pixel program variant, same contract as GL3's m_VPSpecularOutput.
	bool							OutputsSpecularColor;
	/// True for a glsl3vi insert program (GPU skinning / MRM geomorph). Such a
	/// program has no standalone shader: the executable is a mega vertex
	/// program variant with the engine's nlPreTransform() snippet spliced in,
	/// stored in the driver's _InsertVPShaders map instead of _UserVPShaders.
	bool							IsInsertProgram;
	/// True for a user pixel program, whose module lives in _UserPPShaders.
	bool							IsPixelProgram;
	/// CVertexBuffer texcoord flags the program writes as varyings. The pixel
	/// program uses them to decide which texCoordN to sample (bloom's blur
	/// writes four offset coordinates even though the quad has one UV set).
	uint16							WrittenTexCoords;
};

/// A Vulkan vertex buffer created on behalf of a NeL CVertexBuffer.
/// The memory is host visible and coherent: NeL writes vertex data through
/// lock()/unlock() (and through CVertexBuffer::fillBuffer when the engine keeps
/// a local copy), so the driver never has to stage uploads for vertex data.
class CVBDrvInfosVulkan : public IVBDrvInfos
{
public:
	CVBDrvInfosVulkan(CDriverVulkan *drv, ItVBDrvInfoPtrList it, CVertexBuffer *vb);
	virtual ~CVBDrvInfosVulkan();

	void	ensureCapacity(uint32 size);
	uint8	*lock(uint begin, uint end, bool readOnly);
	void	unlock(uint begin, uint end);
	/// Buffer and offset the current frame's vertices are in. Dynamic buffers
	/// point into the driver's per-frame vertex ring, static ones at their own
	/// VkBuffer.
	VkBuffer	currentBuffer() const { return _DrawBuffer; }
	uint32		currentOffset() const { return _DrawOffset; }

	/// One buffer per frame in flight for buffers the CPU rewrites in full but
	/// not necessarily every frame (skinning, water geometry). Static buffers
	/// use a single one.
	static const uint	MaxSlots = 2;
	/// True for buffers whose contents are undefined after swapBuffers
	/// (FullStream / SmallStream): their data goes to the per-frame vertex
	/// ring, so every lock gets its own storage and neither an earlier draw of
	/// this frame nor a previous frame can see a later write.
	bool		_Dynamic;
	VkBuffer	_VertexBuffer[MaxSlots];
	VkDeviceMemory	_Memory[MaxSlots];
	void		*_Mapped[MaxSlots];
	uint32		_SlotCount;
	uint32		_Slot;
	uint64		_SlotFrame[MaxSlots];
	uint64		_LockFrame;
	/// True once a draw used the current slot: the next full rewrite must go
	/// to another copy, or the recorded draw would read the new vertices.
	bool		_Drawn;
	VkBuffer	_DrawBuffer;
	uint32		_DrawOffset;
	uint32		_Size;
};

/// A Vulkan texture created on behalf of a NeL ITexture.
struct CTextureVulkan
{
	CTextureVulkan()
		: Image(VK_NULL_HANDLE), Memory(VK_NULL_HANDLE), MemorySize(0), View(VK_NULL_HANDLE), Sampler(VK_NULL_HANDLE)
		, Framebuffer(VK_NULL_HANDLE), Width(0), Height(0), MipLevels(1), Layers(1), HasAlpha(false), Uploaded(false)
		, Compressed(false), SourceFormat(CBitmap::DonTKnow), Format(VK_FORMAT_UNDEFINED), Usage(0)
		, Layout(VK_IMAGE_LAYOUT_UNDEFINED), DepthTarget(-1) {}

	VkImage			Image;
	VkDeviceMemory	Memory;
	/// Bytes allocated for Memory, from VkMemoryRequirements.
	VkDeviceSize	MemorySize;
	VkImageView		View;
	VkSampler		Sampler;
	/// Created when the texture is used as a render target, so that one image
	/// is both the colour attachment and the sampled texture. Always built
	/// with the offscreen render pass.
	VkFramebuffer	Framebuffer;
	uint32			Width;
	uint32			Height;
	/// Number of mip levels the image was created with.
	uint32			MipLevels;
	uint32			Layers;
	bool			HasAlpha;
	bool			Uploaded;
	/// True when the pixels are kept compressed (DXTC uploaded as a BC
	/// format), which makes the driver keep the texture bitmap compressed and
	/// upload raw blocks instead of converting every texel to RGBA.
	bool			Compressed;
	CBitmap::TType	SourceFormat;
	/// VkFormat of Image: NL_VK_TEXTURE_FORMAT for uncompressed textures,
	/// the matching BC format for compressed ones.
	VkFormat		Format;
	/// Usage flags the image was created with; render targets need
	/// COLOR_ATTACHMENT usage and are recreated if they were created without.
	VkImageUsageFlags	Usage;
	/// Current layout of Image. Sampled descriptors assume SHADER_READ_ONLY,
	/// which is where both uploads and render-target passes leave the image.
	VkImageLayout	Layout;
	/// Index in CDriverVulkan::_DepthTargets of the depth/stencil attachment
	/// shared with the other targets of this size (-1 when not a target).
	sint32			DepthTarget;
};

/// Driver-side data of one NeL texture. Owned by the texture's
/// ITexture::TextureDrvShare, so it lives exactly as long as an ITexture points
/// at it: when the engine frees a texture, the image is destroyed instead of
/// staying in the driver for the whole session. Textures that use the engine
/// sharing system (CTextureFile with sharing on, cube textures) share one
/// instance through the driver's name map, like the GL and GL3 drivers do.
class CTextureDrvInfosVulkan : public ITextureDrvInfos
{
public:
	CTextureDrvInfosVulkan(IDriver *drv, ItTexDrvInfoPtrMap it, CDriverVulkan *driver)
		: ITextureDrvInfos(drv, it), _Driver(driver), TextureMemory(0) {}
	~CTextureDrvInfosVulkan() NL_OVERRIDE;

	/// For Debug info. Return the memory cost of this texture.
	uint getTextureMemoryUsed() const NL_OVERRIDE { return TextureMemory; }

	CDriverVulkan			*_Driver;
	CTextureVulkan			Texture;
	/// Device memory used by Texture (sum of all mip levels).
	uint32					TextureMemory;
};

/// Depth/stencil attachment shared by every offscreen target of a given size,
/// like GL3's CDepthStencilFBO cache. The image is never destroyed before the
/// device: targets of that size may come and go.
struct CDepthTargetVulkan
{
	CDepthTargetVulkan()
		: Image(VK_NULL_HANDLE), Memory(VK_NULL_HANDLE), View(VK_NULL_HANDLE)
		, Format(VK_FORMAT_UNDEFINED), Width(0), Height(0), Initialized(false) {}

	VkImage			Image;
	VkDeviceMemory	Memory;
	VkImageView		View;
	VkFormat		Format;
	uint32			Width;
	uint32			Height;
	/// False until the image has been transitioned out of UNDEFINED layout by
	/// the first render pass that uses it.
	bool			Initialized;
};

/// Pipeline state key: everything that changes the created VkPipeline.
struct CPipelineKey
{
	CPipelineKey()
	{
		memset(this, 0, sizeof(*this));
		FrontFace = VK_FRONT_FACE_CLOCKWISE;
		DepthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
	}
	uint32	VertexFormat;      // hash of the CVertexBuffer layout (format + value types)
	uint32	VertexFlags;       // CVertexBuffer format bits (attribute presence)
	uint32	VertexStride;
	uint32	Topology;
	uint32	BlendEnable;
	uint32	SrcBlend;
	uint32	DstBlend;
	uint32	BlendOp;
	uint32	DepthTest;
	uint32	DepthWrite;
	uint32	DepthCompare;
	uint32	CullMode;
	uint32	FrontFace;
	uint32	ColorWriteMask;
	uint32	AlphaTest;
	uint32	HasTexture0;
	uint32	AlphaBlendBits;
	uint32	Mega;             // use the mega VP/PP instead of the simple shader
	uint32	MegaCube;         // mega PP samples a cubemap at sampler1 (Specular)
	uint32	UserVP;           // active user vertex program id (0 = builtin mega VP)
	uint32	UserVPInsert;     // the id refers to a glsl3vi insert mega VP variant
	uint32	MegaSpecular;     // mega PP adds the specularColor varying (from the VP)
	uint32	Target;           // render pass the pipeline is used with (0 = swapchain, 1 = offscreen)
	uint32	UserPP;           // active user pixel program id (0 = builtin mega PP)

	bool operator==(const CPipelineKey &o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
	bool operator<(const CPipelineKey &o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};

/// A compiled pipeline for a given key.
struct CPipelineVulkan
{
	VkPipeline			Pipeline;
};

/// Key of a glsl3vi mega vertex program variant: the insert program id plus
/// the two GL3 variant bits. Only (fogOrPpl=1, hwClip=0) is generated for now:
/// the mega pixel stage is always the fog/PPL variant and the driver reports
/// no hardware clip plane support. Keeping the bits in the key leaves room for
/// the remaining variants without invalidating the insert program identity.
inline uint64 insertVertexShaderKey(uint32 programId, uint fogOrPpl, uint hwClip)
{
	return ((uint64)programId << 2) | ((uint64)(fogOrPpl & 1u) << 1) | (hwClip & 1u);
}

class CDriverVulkan : public IDriver
{
public:
	CDriverVulkan();
	virtual ~CDriverVulkan();

	virtual bool init(uintptr_t windowIcon, emptyProc exitFunc) override;
	virtual bool release() override;
	virtual bool activate() override;
	virtual bool isLost() const override;
	virtual bool isActive() override;
	virtual uint64 getSwapBufferCounter() const override;
	virtual void disableHardwareVertexProgram() override;
	virtual void disableHardwarePixelProgram() override;
	virtual void disableHardwareVertexArrayAGP() override;
	virtual void disableHardwareTextureShader() override;
	virtual bool setDisplay(nlWindow wnd, const GfxMode& mode, bool show, bool resizeable) override;
	virtual bool setMode(const GfxMode &mode) override;
	virtual bool getModes(std::vector<GfxMode> &modes) override;
	virtual void setWindowTitle(const ucstring &title) override;
	virtual void setWindowIcon(const std::vector<NLMISC::CBitmap> &bitmaps) override;
	virtual void setWindowPos(sint32 x, sint32 y) override;
	virtual void showWindow(bool show) override;
	virtual bool getCurrentScreenMode(GfxMode &mode) override;
	virtual void beginDialogMode() override;
	virtual void endDialogMode() override;
	virtual nlWindow getDisplay() override;
	virtual bool supportMonitorColorProperties() const override;
	virtual bool setMonitorColorProperties(const CMonitorColorProperties &properties) override;
	virtual emptyProc getWindowProc() override;
	virtual bool copyTextToClipboard(const std::string &text) override;
	virtual bool pasteTextFromClipboard(std::string &text) override;
	virtual uint8 getBitPerPixel() override;
	virtual void getWindowSize(uint32 &width, uint32 &height) override;
	virtual void getWindowPos(sint32 &x, sint32 &y) override;
	virtual bool clear2D(CRGBA rgba) override;
	virtual bool clearZBuffer(float zval) override;
	virtual bool clearStencilBuffer(sint stencilval) override;
	virtual void setColorMask(bool bRed, bool bGreen, bool bBlue, bool bAlpha) override;
	virtual void getBuffer(CBitmap &bitmap) override;
	virtual void getZBuffer(std::vector<float> &zbuffer) override;
	virtual void getBufferPart(CBitmap &bitmap, NLMISC::CRect &rect) override;
	virtual void getZBufferPart(std::vector<float> &zbuffer, NLMISC::CRect &rect) override;
	virtual bool fillBuffer(CBitmap &bitmap) override;
	virtual void setDepthRange(float znear, float zfar) override;
	virtual void getDepthRange(float &znear, float &zfar) const override;
	virtual bool isTextureExist(const ITexture &tex) override;
	virtual bool setupTexture(ITexture &tex) override;
	virtual bool setupTextureEx(ITexture &tex, bool bUpload, bool &bAllUploaded, bool bMustRecreateSharedTexture) override;
	virtual bool uploadTexture(ITexture &tex, NLMISC::CRect &rect, uint8 nNumMipMap) override;
	virtual bool uploadTextureCube(ITexture &tex, NLMISC::CRect &rect, uint8 nNumMipMap, uint8 nNumFace) override;
	virtual void forceDXTCCompression(bool dxtcComp) override;
	virtual void setAnisotropicFilter(sint filter) override;
	virtual uint getAnisotropicFilter() const override;
	virtual uint getAnisotropicFilterMaximum() const override;
	virtual void forceTextureResize(uint divisor) override;
	virtual uint getNbTextureStages() const override;
	virtual void getNumPerStageConstant(uint &lightedMaterial, uint &unlightedMaterial) const override;
	virtual bool isTextureRectangle(ITexture *tex) const override;
	virtual bool supportNonPowerOfTwoTextures() const override;
	virtual bool stretchRect(ITexture *srcText, NLMISC::CRect &srcRect, ITexture *destText, NLMISC::CRect &destRect) override;
	virtual bool setupMaterial(CMaterial &mat) override;
	virtual void startSpecularBatch() override;
	virtual void endSpecularBatch() override;
	virtual sint beginMaterialMultiPass() override;
	virtual void setupMaterialPass(uint pass) override;
	virtual void endMaterialMultiPass() override;
	virtual bool supportPerPixelLighting(bool specular) const override;
	virtual bool supportWorldSpacePPL() const override;
	virtual void setFrustum(float left, float right, float bottom, float top, float znear, float zfar, bool perspective) override;
	virtual void setFrustumMatrix(CMatrix &frust) override;
	virtual CMatrix getFrustumMatrix() override;
	virtual float getClipSpaceZMin() const override;
	virtual void setupViewMatrix(const CMatrix &mtx) override;
	virtual void setupViewMatrixEx(const CMatrix &mtx, const CVector &cameraPos) override;
	virtual void setupModelMatrix(const CMatrix &mtx) override;
	virtual CMatrix getViewMatrix() const override;
	virtual void forceNormalize(bool normalize) override;
	virtual bool isForceNormalize() const override;
	virtual bool supportVertexBufferHard() const override;
	virtual bool supportVolatileVertexBuffer() const override;
	virtual bool supportIndexOffset() const override;
	virtual bool slowUnlockVertexBufferHard() const override;
	virtual uint getMaxVerticesByVertexBufferHard() const override;
	virtual bool initVertexBufferHard(uint agpMem, uint vramMem) override;
	virtual uint32 getAvailableVertexAGPMemory() override;
	virtual uint32 getAvailableVertexVRAMMemory() override;
	virtual bool activeVertexBuffer(CVertexBuffer &VB) override;
	virtual bool activeIndexBuffer(CIndexBuffer &IB) override;
	virtual bool renderLines(CMaterial &mat, uint32 firstIndex, uint32 nlines) override;
	virtual bool renderTriangles(CMaterial &mat, uint32 firstIndex, uint32 ntris) override;
	virtual bool renderSimpleTriangles(uint32 firstIndex, uint32 ntris) override;
	virtual bool renderRawPoints(CMaterial &mat, uint32 startVertex, uint32 numPoints) override;
	virtual bool renderRawLines(CMaterial &mat, uint32 startVertex, uint32 numTri) override;
	virtual bool renderRawTriangles(CMaterial &mat, uint32 startVertex, uint32 numTri) override;
	virtual bool renderLinesWithIndexOffset(CMaterial &mat, uint32 firstIndex, uint32 nlines, uint indexOffset) override;
	virtual bool renderTrianglesWithIndexOffset(CMaterial &mat, uint32 firstIndex, uint32 ntris, uint indexOffset) override;
	virtual bool renderSimpleTrianglesWithIndexOffset(uint32 firstIndex, uint32 ntris, uint indexOffset) override;
	virtual bool renderRawQuads(CMaterial &mat, uint32 startVertex, uint32 numQuads) override;
	virtual void mapTextureStageToUV(uint stage, uint uv) override;
	virtual bool swapBuffers() override;
	virtual void setSwapVBLInterval(uint interval) override;
	virtual uint getSwapVBLInterval() override;
	virtual void profileRenderedPrimitives(CPrimitiveProfile &pIn, CPrimitiveProfile &pOut) override;
	virtual uint32 profileAllocatedTextureMemory() override;
	virtual uint32 profileSetupedMaterials() const override;
	virtual uint32 profileSetupedModelMatrix() const override;
	virtual void enableUsedTextureMemorySum(bool enable) override;
	virtual uint32 getUsedTextureMemory() const override;
	virtual void startProfileVBHardLock() override;
	virtual void endProfileVBHardLock(std::vector<std::string> &result) override;
	virtual void profileVBHardAllocation(std::vector<std::string> &result) override;
	virtual void startProfileIBLock() override;
	virtual void endProfileIBLock(std::vector<std::string> &result) override;
	virtual void profileIBAllocation(std::vector<std::string> &result) override;
	virtual bool fogEnabled() override;
	virtual void enableFog(bool enable) override;
	virtual void setupFog(float start, float end, NLMISC::CRGBA color) override;
	virtual void setupFogMode(TFogMode mode, float density) override;
	virtual float getFogStart() const override;
	virtual float getFogEnd() const override;
	virtual NLMISC::CRGBA getFogColor() const override;
	virtual TFogMode getFogMode() const override;
	virtual float getFogDensity() const override;
	virtual void setupViewport(const class CViewport &viewport) override;
	virtual void getViewport(CViewport &viewport) override;
	virtual void setupScissor(const class CScissor &scissor) override;
	virtual uint32 getImplementationVersion() const override;
	virtual sint getTotalVideoMemory() const override;
	virtual void showCursor(bool b) override;
	virtual void setMousePos(float x, float y) override;
	virtual void setCapture(bool b) override;
	virtual bool isSystemCursorCaptured() override;
	virtual void addCursor(const std::string &name, const NLMISC::CBitmap &bitmap) override;
	virtual void setCursor(const std::string &name, NLMISC::CRGBA col, uint8 rot, sint hotSpotX, sint hotSpotY, bool forceRebuild) override;
	virtual void setCursorScale(float scale) override;
	virtual bool setRenderTarget(ITexture *tex, uint32 x, uint32 y, uint32 width, uint32 height, uint32 mipmapLevel, uint32 cubeFace) override;
	virtual bool getRenderTargetSize(uint32 &width, uint32 &height) override;
	virtual bool copyTargetToTexture(ITexture *tex, uint32 offsetx, uint32 offsety, uint32 x, uint32 y, uint32 width, uint32 height, uint32 mipmapLevel) override;
	virtual bool textureCoordinateAlternativeMode() const override;
	virtual uint getMaxLight() const override;
	virtual void setLight(uint8 num, const CLight &light) override;
	virtual void enableLight(uint8 num, bool enable) override;
	virtual void setAmbientColor(NLMISC::CRGBA color) override;
	virtual void setPerPixelLightingLight(NLMISC::CRGBA diffuse, NLMISC::CRGBA specular, float shininess) override;
	virtual void setLightMapDynamicLight(bool enable, const CLight &light) override;
	virtual void enableLightTableMode(bool enable) override;
	virtual void setLightTableSize(uint count) override;
	virtual void setLightTableEntry(uint index, const CLight &light) override;
	virtual void setLights(const sint16 *tableIndices, const uint8 *factors, uint numLights, uint numPerPixelLights, NLMISC::CRGBA ambient) override;
	virtual bool isVertexProgramEmulated() const override;
	virtual bool supportVertexProgram(CVertexProgram::TProfile profile) const override;
	virtual bool compileVertexProgram(CVertexProgram *program) override;
	virtual bool activeVertexProgram(CVertexProgram *program) override;
	virtual bool supportPixelProgram(CPixelProgram::TProfile profile) const override;
	virtual bool compilePixelProgram(CPixelProgram *program) override;
	virtual bool activePixelProgram(CPixelProgram *program) override;
	virtual bool supportGeometryProgram(CGeometryProgram::TProfile profile) const override;
	virtual bool compileGeometryProgram(CGeometryProgram *program) override;
	virtual bool activeGeometryProgram(CGeometryProgram *program) override;
	virtual void setUniform1f(TProgram program, uint index, float f0) override;
	virtual void setUniform2f(TProgram program, uint index, float f0, float f1) override;
	virtual void setUniform3f(TProgram program, uint index, float f0, float f1, float f2) override;
	virtual void setUniform4f(TProgram program, uint index, float f0, float f1, float f2, float f3) override;
	virtual void setUniform1i(TProgram program, uint index, sint32 i0) override;
	virtual void setUniform2i(TProgram program, uint index, sint32 i0, sint32 i1) override;
	virtual void setUniform3i(TProgram program, uint index, sint32 i0, sint32 i1, sint32 i2) override;
	virtual void setUniform4i(TProgram program, uint index, sint32 i0, sint32 i1, sint32 i2, sint32 i3) override;
	virtual void setUniform1ui(TProgram program, uint index, uint32 ui0) override;
	virtual void setUniform2ui(TProgram program, uint index, uint32 ui0, uint32 ui1) override;
	virtual void setUniform3ui(TProgram program, uint index, uint32 ui0, uint32 ui1, uint32 ui2) override;
	virtual void setUniform4ui(TProgram program, uint index, uint32 ui0, uint32 ui1, uint32 ui2, uint32 ui3) override;
	virtual void setUniform3f(TProgram program, uint index, const NLMISC::CVector& v) override;
	virtual void setUniform4f(TProgram program, uint index, const NLMISC::CVector& v, float f3) override;
	virtual void setUniform4f(TProgram program, uint index, const NLMISC::CRGBAF& rgba) override;
	virtual void setUniform4x4f(TProgram program, uint index, const NLMISC::CMatrix& m) override;
	virtual void setUniform4fv(TProgram program, uint index, size_t num, const float *src) override;
	virtual void setUniform4iv(TProgram program, uint index, size_t num, const sint32 *src) override;
	virtual void setUniform4uiv(TProgram program, uint index, size_t num, const uint32 *src) override;
	virtual void setUniformMatrix(TProgram program, uint index, TMatrix matrix, TTransform transform) override;
	virtual void setUniformFog(TProgram program, uint index) override;
	virtual bool isUniformProgramState() override;
	virtual bool supportCloudRenderSinglePass() const override;
	virtual bool supportBloomEffect() const override;
	virtual bool supportVertexProgramDoubleSidedColor() const override;
	virtual bool supportTextureShaders() const override;
	virtual bool supportWaterShader() const override;
	virtual bool cubemapZPositiveForward() const override;
	virtual bool supportTextureAddrMode(CMaterial::TTexAddressingMode mode) const override;
	virtual void setMatrix2DForTextureOffsetAddrMode(const uint stage, const float mat[4]) override;
	virtual bool supportEMBM() const override;
	virtual bool isEMBMSupportedAtStage(uint stage) const override;
	virtual void setEMBMMatrix(const uint stage, const float mat[4]) override;
	virtual bool supportBlendConstantColor() const override;
	virtual void setBlendConstantColor(NLMISC::CRGBA col) override;
	virtual NLMISC::CRGBA getBlendConstantColor() const override;
	virtual void finish() override;
	virtual void flush() override;
	virtual void enablePolygonSmoothing(bool smooth) override;
	virtual bool isPolygonSmoothingEnabled() const override;
	virtual void swapTextureHandle(ITexture &tex0, ITexture &tex1) override;
	virtual uintptr_t getTextureHandle(const ITexture&tex) override;
	virtual bool supportMADOperator() const override;
	virtual uint getNumAdapter() const override;
	virtual bool getAdapter(uint adapter, CAdapter &desc) const override;
	virtual bool setAdapter(uint adapter) override;
	virtual bool supportOcclusionQuery() const override;
	virtual void deleteOcclusionQuery(IOcclusionQuery *oq) override;
	virtual void setCullMode(TCullMode cullMode) override;
	virtual TCullMode getCullMode() const override;
	virtual void enableStencilTest(bool enable) override;
	virtual bool isStencilTestEnabled() const override;
	virtual void stencilFunc(TStencilFunc stencilFunc, int ref, uint mask) override;
	virtual void stencilOp(TStencilOp fail, TStencilOp zfail, TStencilOp zpass) override;
	virtual void stencilMask(uint mask) override;
	virtual void enableClipPlane(uint index, bool enable) override;
	virtual void setClipPlane(uint index, const NLMISC::CPlane &plane) override;
	// Not generated (pointer return types / default implementations in IDriver).
	virtual NLMISC::IEventEmitter *getEventEmitter() override;
	virtual const char *getDriverInformation() override;
	virtual const char *getVideocardInformation() override;
	virtual ITexture *getRenderTarget() const override;
	virtual IOcclusionQuery *createOcclusionQuery() override;
	virtual bool bindUniformBuffer(TUBBinding binding, CUniformBuffer *ub) override;
	virtual bool supportBuiltinUBO() const override { return getenv("NLVK_NOUBO") == NULL; }
	virtual uint getMaxLightTableSize() const override;
	virtual bool supportVertexProgramClipPlanes() const override { return false; }
	virtual bool needVertexProgramClipVariant() const override { return false; }
	virtual bool supportLargeUBOArrays() const override { return true; }
	virtual bool isTripleBufferPipelined() const override { return false; }
	virtual uint64 getSwapBufferInFlight() const override { return 0; }
	virtual bool isFrameReady() override { return true; }
	virtual void setPolygonMode(TPolygonMode mode) override;
	virtual bool clearDepthStencil(float zval, sint stencilval) override;
	virtual void enableVertexProgramDoubleSidedColor(bool doubleSided) override;
	virtual CVertexBuffer::TVertexColorType getVertexColorFormat() const override;
	virtual void startBench(bool wantStandardDeviation, bool quick, bool reset) override;
	virtual void endBench() override;
	virtual void displayBench(class NLMISC::CLog *log) override;

private:
	// --- Window / platform ---
	Display					*_Display;
	Window					_XWindow;
	bool					_OwnWindow;
	Atom					_WmDeleteMessage;
	uint32					_WindowWidth;
	uint32					_WindowHeight;
	bool					_WindowVisible;
	GfxMode					_CurrentMode;
	/// release() started: no more frames, no more swapchain work.
	bool					_ShuttingDown;
	/// The swapchain could not be recreated (surface gone): stop presenting.
	bool					_SwapchainLost;
	/// NLVK_PRESENT_LOG=1: trace acquire/present per frame.
	bool					_PresentLog;
	/// Results of the calls of the frame being presented (for the present log).
	VkResult				_LastAcquireResult;
	VkResult				_LastFenceResult;
	VkResult				_LastSubmitResult;
	/// Timestamp of the previous logged present, for the frame interval.
	uint64					_PresentLogLastNs;
	/// Number of frames that could not be rendered/acquired, for the log.
	uint64					_SkippedFrames;
	/// Incremented on every successful swapchain creation.
	uint32					_SwapchainGeneration;
#if defined(NL_OS_UNIX) && !defined(NL_OS_MAC)
	NLMISC::CVulkanEventEmitter _EventEmitter;
#endif

	// --- Vulkan core ---
	VkInstance				_Instance;
	VkDebugUtilsMessengerEXT _DebugMessenger;
	VkPhysicalDevice		_PhysicalDevice;
	VkPhysicalDeviceProperties _DeviceProps;
	VkDevice				_Device;
	VkQueue					_GraphicsQueue;
	uint32					_GraphicsFamily;
	VkSurfaceKHR			_Surface;
	VkSwapchainKHR			_Swapchain;
	VkFormat				_SwapchainFormat;
	VkExtent2D				_SwapchainExtent;
	std::vector<VkImage>	_SwapchainImages;
	std::vector<VkImageView> _SwapchainViews;
	VkRenderPass			_RenderPass;
	std::vector<VkFramebuffer> _Framebuffers;
	/// Same attachments as _RenderPass but with LOAD ops: used to resume the
	/// main pass after an offscreen target was rendered in the middle of a
	/// frame. Render-pass compatible with _RenderPass, so it shares pipelines.
	VkRenderPass			_ResumeRenderPass;
	/// Colour + depth pass for render targets (RGBA8, same depth format).
	VkRenderPass			_OffscreenRenderPass;
	/// Depth/stencil images shared by offscreen targets of the same size.
	std::vector<CDepthTargetVulkan>	_DepthTargets;

	/// Render target state. _RenderTargetTexture is NULL while the main
	/// swapchain pass is bound; while it is set, the offscreen pass of that
	/// texture is the pass being recorded (_FrameOpen is still true).
	ITexture				*_RenderTargetTexture;
	CTextureVulkan			*_RenderTargetInfo;
	/// Number of setRenderTarget() binds, reported with the frame counters.
	uint64					_RenderTargetBinds;
	bool					_RenderTargetLogged;
	/// Viewport of the main target, saved when an offscreen target is bound
	/// and restored by setRenderTarget(NULL), like the GL3 driver.
	CViewport				_SavedViewport;

	VkFormat				_DepthFormat;
	// One depth/stencil image per swapchain image: frames in flight must never
	// share a depth attachment.
	std::vector<VkImage>	_DepthImages;
	std::vector<VkDeviceMemory>	_DepthMemories;
	std::vector<VkImageView>	_DepthViews;

	VkCommandPool			_CommandPool;
	std::vector<VkCommandBuffer> _CommandBuffers;
	std::vector<VkFence>	_Fences;
	std::vector<VkSemaphore> _ImageAvailable;
	std::vector<VkSemaphore> _RenderFinished;
	/// NLVK_STATS: two GPU timestamps per frame slot (start/end of the frame
	/// command buffer), used for the GPU-time column of the report.
	VkQueryPool				_GpuTimeQuery;
	bool					_GpuTimeEnabled;
	double					_GpuTimePeriod;

	uint32					_FrameIndex;
	uint32					_ImageIndex;
	bool					_FrameOpen;
	uint64					_SwapBufferCounter;

	// Pending clear values for the current frame.
	NLMISC::CRGBA			_ClearColor;
	float					_ClearDepth;
	sint					_ClearStencil;
	bool					_ClearColorPending;
	bool					_ClearDepthPending;
	bool					_ClearStencilPending;

	// --- Render state cache (subset of the IDriver state we honour) ---
	CMatrix					_ViewMatrix;
	CMatrix					_ModelMatrix;
	CMatrix					_ProjectionMatrix;
	CViewport				_Viewport;
	CScissor				_Scissor;
	TCullMode				_CullMode;
	bool					_ZWrite;
	bool					_ZTest;
	CMaterial				*_CurrentMaterial;
	bool					_FogColorOverrideBlack; // additive blends get black fog (GL3 behaviour)

	bool					_FogEnabled;
	float					_FogStart;
	float					_FogEnd;
	NLMISC::CRGBA			_FogColor;
	TFogMode				_FogMode;
	float					_FogDensity;
	float					_DepthRangeNear;
	float					_DepthRangeFar;
	/// 1 / (zfar - znear) of the current frustum; converts a material zBias
	/// (expressed in NDC units) into the depth range offset GL uses.
	float					_OODeltaZ;
	/// mat.getZBias() * _OODeltaZ of the current draw, applied to the viewport
	/// depth range like the GL/GL3 drivers do (glDepthRange).
	float					_MaterialZBias;
	emptyProc				_ExitFunc;
	int						_ScreenshotFrame;
	VkBuffer				_ScreenshotBuffer;
	VkDeviceMemory			_ScreenshotMemory;
	void					*_ScreenshotMapped;
	VkDeviceSize			_ScreenshotSize;
	bool					_ScreenshotPending;
	uint32					_ArrayDrawCount;
	uint32					_IndexedDrawCount;

	// --- Helpers ---
	bool					initVulkan();
	void					shutdownVulkan();
	bool					createSurface();
	bool					pickPhysicalDevice();
	bool					createLogicalDevice();
	bool					createSwapchain();
	void					destroySwapchain();
	bool					createDepthResources();
	void					destroyDepthResources();
	bool					createRenderPass();
	bool					createFramebuffers();
	bool					createCommands();
	bool					createSyncObjects();
	bool					recreateSwapchain(const char *reason);
	/// True while the X window still exists (X errors are not fatal).
	bool					windowAlive();
	void					recordFrameCommands(VkCommandBuffer cmd);
	bool					beginFrame();
	void					dumpScreenshot(const char *path);
	void					recordScreenshotCopy(VkCommandBuffer cmd);
	void					writeScreenshot(const char *path);

	// --- Render targets (driver_vulkan_target.cpp) ---
	/// Start the swapchain pass. 'first' begins the frame (clears the target),
	/// otherwise the pass resumes after an offscreen target (loads it).
	void					beginMainPass(VkCommandBuffer cmd, bool first);
	/// Extent of the bound render target, or of the swapchain.
	VkExtent2D				currentTargetExtent() const;
	/// Bind the cached viewport/scissor, clamped to 'extent'.
	void					applyViewportScissor(VkCommandBuffer cmd, VkExtent2D extent);
	/// Viewport from _Viewport, the depth range and the current material zBias.
	VkViewport				computeViewport(VkExtent2D extent) const;
	/// Re-issue the dynamic viewport when the material zBias changes.
	void					applyDepthRange(VkCommandBuffer cmd);
	/// Depth/stencil attachment shared by the targets of this size.
	sint32					getDepthTarget(uint32 width, uint32 height);
	bool					createTargetFramebuffer(CTextureVulkan &target, VkRenderPass pass);
	/// Make 'tex' usable as a render target (image with colour-attachment
	/// usage, framebuffer); recreated when the texture size changes.
	bool					ensureRenderTarget(ITexture &tex, CTextureVulkan &target);
	void					beginOffscreenPass(VkCommandBuffer cmd, CTextureVulkan &target, VkExtent2D extent);
	void					endOffscreenPass(VkCommandBuffer cmd, CTextureVulkan &target);
	void					destroyDepthTargets();

	VkShaderModule			createShaderModule(const std::vector<uint32> &code);
	VkFormat				findDepthFormat() const;

	// --- Rendering (driver_vulkan_render.cpp) ---
	bool					compileShader(const char *glsl, VkShaderModule *outModule, bool vertex);
	VkPipeline				getPipeline(const CPipelineKey &key);
	void					updateMaterialState(CMaterial &mat);
	bool					setupVertexBuffer(CVertexBuffer &vb);
	bool					drawIndexed(CMaterial &mat, uint32 firstIndex, uint32 count, VkPrimitiveTopology topo, uint32 indexStride);
	bool					drawArrays(CMaterial &mat, uint32 firstVertex, uint32 count, VkPrimitiveTopology topo);
	VkBuffer				getIndexUploadBuffer(const void *data, uint32 size, uint32 stride);
	void					drawTestTriangle(VkCommandBuffer cmd);
	bool					commitPushConstants(const CPipelineKey &key, VkCommandBuffer cmd, CVertexBuffer &vb);

	// --- Mega shaders (driver_vulkan_mega_render.cpp) ---
	bool					initMega();
	void					shutdownMega();
	VkShaderModule			getMegaVertexShader();
	VkShaderModule			getMegaPixelShader(bool cube, bool specular);
	bool					canUseMegaPath(bool &cube);
	void					fillMegaPipelineKey(CPipelineKey &key);
	bool					bindMegaDescriptors(VkCommandBuffer cmd, CVertexBuffer &vb);
	const CCameraUBOData	&stageMegaCameraUBO();
	void					stageMegaLightTableUBO();
	void					stageMegaObjectUBO(CVertexBuffer &vb, CObjectUBOData &data);
	void					stageMegaMaterialUBO(CMaterial &mat, CMaterialUBOData &data);
	uint32					allocUniformBlock(const void *data, uint32 size);
	/// Make room for one more per-draw uniform block, growing the ring without
	/// invalidating blocks a frame in flight may still reference.
	bool					growUniformRing(uint32 required);
	void					computeLightMapInfos(const CMaterial &mat);

	// --- User vertex programs (driver_vulkan_vp.cpp) ---
	/// True when a compiled user VP is selected and its shader exists.
	bool					hasActiveUserVPS();
	/// The engine UBO the active user VP reads at a binding (nelvp programs
	/// answer UBBindingVertexProgram with their constant register block).
	CUniformBuffer			*activeUserUB(TUBBinding binding);
	/// Copy that block into the per-frame ring for the descriptor at 'binding'.
	bool					stageUserUB(TUBBinding binding, VkDescriptorBufferInfo &info);
	/// Per-draw staging cache for user uniform blocks (see stageUserUB).
	CUniformBuffer			*_UserUBStageCache[UBBindingCount];
	uint32					_UserUBStageOffset[UBBindingCount];
	uint32					_UserUBStageSize[UBBindingCount];
	/// NLVK_STATS=1: per-draw CPU cost counters, logged every 120 frames.
	bool					_StatsEnabled;
	uint64					_StatsDraws;
	uint64					_StatsDescriptorNs;
	uint64					_StatsIndexNs;
	/// Breakdown of the per-draw region: pipeline-key build, pipeline lookup,
	/// descriptor/binding setup, and the final vertex/index/draw commands.
	uint64					_StatsKeyNs;
	uint64					_StatsPipelineNs;
	uint64					_StatsBindNs;
	uint64					_StatsSubmitNs;
	/// CPU frame recording and the waits that bound it, per NLVK_STATS window.
	uint64					_StatsCpuNs;
	uint64					_StatsFenceWaitNs;
	uint64					_StatsAcquireNs;
	/// GPU time between the frame's two timestamps, per NLVK_STATS window.
	uint64					_StatsGpuNs;
	uint64					_StatsGpuSamples;
	uint64					_StatsStagingPeak;
	/// Time at which the current frame started recording (0 outside a frame).
	uint64					_FrameCpuStartNs;
	/// Texture copies recorded and bytes staged, per NLVK_STATS window.
	uint64					_StatsTextureCopies;
	uint64					_StatsUploadBytes;
	uint64					_StatsWindowStartNs;
	uint64					_StatsWindowFrames;
	/// Compile a glsl3vi snippet into the mega VP variants it can select.
	bool					compileInsertVertexProgram(CVertexProgram *program, IProgram::CSource *insertSrc);

	// --- Textures (driver_vulkan_texture.cpp) ---
	/// Driver data of `tex`, created (and registered in the share-name map for
	/// shared textures) on first use.
	CTextureDrvInfosVulkan	*getTextureDrvInfos(ITexture &tex, bool *created = NULL);
	/// Same, but only returns an existing entry (NULL when the texture has
	/// never been set up).
	CTextureDrvInfosVulkan	*findTextureDrvInfos(const ITexture &tex) const;
	CTextureVulkan			*getTextureVulkan(ITexture &tex);
	bool					uploadTextureVulkan(ITexture &tex, CTextureVulkan &out, bool upload);
	bool					createTextureImage(uint32 width, uint32 height, uint mipCount,
								VkImageUsageFlags usage, VkFormat format, CTextureVulkan &out);
	/// VkFormat for a DXTC bitmap, or VK_FORMAT_UNDEFINED when the texture is
	/// not compressed (or the device has no BC support).
	VkFormat				compressedTextureFormat(CBitmap::TType format) const;
	void					destroyTexture(CTextureVulkan &tex);
	/// Install a freshly uploaded image in `info`, retiring the previous one
	/// and keeping the texture-memory counter in sync.
	void					setTextureImage(CTextureDrvInfosVulkan &info, CTextureVulkan &created);
	VkImageView				getWhiteTexture();

	// --- Cached Vulkan render objects ---
	std::map<CPipelineKey, CPipelineVulkan>	_PipelineCache;
	VkPipelineLayout			_PipelineLayout;
	VkPipeline				_TestPipeline;
	VkDescriptorSetLayout		_DescriptorSetLayout;
	VkDescriptorPool			_DescriptorPools[2];
	std::map<uint32, VkShaderModule>	_VertexShaderVariants;
	std::map<uint32, VkShaderModule>	_FragmentShaderVariants;
	VkShaderModule				_VertexShader;
	VkShaderModule				_FragmentShader;
	VkSampler					_DefaultSampler;
	CTextureVulkan				_WhiteTexture;
	/// True when the device supports BC (DXTC) compressed sampled images.
	bool						_SupportsBC;
	/// Device memory of every live texture, for texture profiling and the
	/// driver's used-memory counter.
	uint64						_AllocatedTextureMemory;
	std::set<CTextureDrvInfosVulkan *>	_TextureUsed;
	bool						_UsedTextureMemorySumEnabled;

	// --- Mega shader resources ---
	bool						_MegaEnabled;
	bool						_MegaResourcesReady; // mega descriptor layout/pipeline layout exist
	VkDescriptorSetLayout		_MegaDescriptorSetLayout;
	VkPipelineLayout			_MegaPipelineLayout;
	VkShaderModule				_MegaVertexShader;
	std::map<uint32, VkShaderModule>	_MegaPixelShaderVariants;
	VkBuffer					_MegaLightTableBuffer[2];
	VkDeviceMemory				_MegaLightTableMemory[2];
	void						*_MegaLightTableMapped[2];
	bool						_MegaLightTableStaged;
	VkBuffer					_MegaUserBuffer;
	VkDeviceMemory				_MegaUserBufferMemory;
	CCameraUBOData				_MegaCameraData;
	bool						_MegaCameraDirty;

	// --- User vertex programs (driver_vulkan_vp.cpp) ---
	// The active program replaces the mega vertex stage for every draw made
	// while it is selected. _NelvpActiveUB is the constant register block a
	// converted nelvp program writes through setUniform*; _BoundUserUB holds
	// the UBO the engine bound explicitly for a GLSL user program.
	CProgramDrvInfosVulkan		*_ActiveUserVP;
	CUniformBuffer				*_NelvpActiveUB;
	CUniformBuffer				*_BoundUserUB[UBBindingCount];
	bool						_VPSpecularOutput;
	uint32						_NextUserVPId;
	std::map<uint32, VkShaderModule>	_UserVPShaders;

	// --- User pixel programs (driver_vulkan_vp.cpp) ---
	// A user PP replaces the mega pixel stage while it is active (FXAA is the
	// only engine user today). It draws on the mega pipeline so the UBOs and
	// samplers keep the same bindings.
	CProgramDrvInfosVulkan		*_ActiveUserPP;
	uint32						_NextUserPPId;
	std::map<uint32, VkShaderModule>	_UserPPShaders;
	/// glsl3vi insert mega VP variants, keyed by insertVertexShaderKey().
	std::map<uint64, VkShaderModule>	_InsertVPShaders;

	// --- Mega lightmap multi-pass staging ---
	// Mirrors the GL3 driver: the engine draws a LightMap material once per
	// pass, with stages 0..N-2 holding the pass' lightmaps and stage N-1 the
	// material texture.
	bool						_MegaMultiPassActive;
	/// One-shot diagnostic: the mega path renders a LightMap material.
	bool						_LightMapLogged;
	CTextureVulkan				*_BaseStageTextures[IDRV_PROGRAM_MAXSAMPLERS];
	uint						_NLightMaps;
	uint						_LightMapLUT[NL_VK_MAX_LIGHTMAP];
	uint						_NLightMapPerPass;
	uint						_NLightMapPass;
	bool						_MegaPassOverrideActive;
	float						_MegaPassDiffuse;
	float						_MegaPassLightMapScale;
	float						_MegaPassConstants[IDRV_PROGRAM_MAXSAMPLERS][4];
	float						_MegaPassSelfIllumination[4];
	bool						_MegaPassZeroLightFactors;
	bool						_MegaPassFogBlack;

	/// Textures retired during the given swap-buffer frame. They are freed once
	/// every frame that can reference them is known to be finished, three frame
	/// slots later, without a device-wide wait (see beginFrame()).
	std::vector<std::pair<uint64, CTextureVulkan> >	_PendingTextureDestroy;
	std::vector<std::pair<uint64, std::pair<VkBuffer, VkDeviceMemory> > >	_PendingBufferDestroy;

	// --- Texture uploads ---
	// Every texture copy (the pixels of a new image, an in-place update of an
	// animated texture, one font glyph tile) is recorded into the upload command
	// buffer of the frame being recorded, which is submitted in the same
	// vkQueueSubmit as the frame, ahead of its draw commands. Consequences:
	//  - the copies execute before any draw of that frame, so an image is in its
	//    final layout and has its final content when the frame starts;
	//  - the pipeline barriers in the buffer are ordered against the previous
	//    frame's sampling (submission order on the graphics queue), which is what
	//    used to cost a vkDeviceWaitIdle + submit + vkQueueWaitIdle per update;
	//  - no CPU wait is needed at all: the frame's own fence covers the uploads.
	// Updating the same image twice in one frame is simply two copies in the
	// same buffer, in recording order; the frame sees the last one.
	// One command buffer per frame in flight: beginFrame() waits for that slot's
	// fence, which is what makes it safe to reuse.
	VkCommandBuffer			_UploadCommandBuffers[2];
	bool					_UploadRecording;
	/// Copies recorded in the current command buffer (0 = nothing to submit).
	uint32					_UploadCopyCount;
	/// Mip levels one copy command can span (16 covers any texture the engine
	/// can produce: 2^16 texels are not representable anyway).
	static const uint		MaxTextureCopyRegions = 16;
	/// One image copy: layout transitions around a vkCmdCopyBufferToImage, or a
	/// layout-only transition when RegionCount is 0.
	struct STextureCopy
	{
		STextureCopy()
			: Image(VK_NULL_HANDLE), OldLayout(VK_IMAGE_LAYOUT_UNDEFINED), BaseLevel(0), LevelCount(1)
			, Staging(VK_NULL_HANDLE), StagingBytes(0), RegionCount(0) {}
		VkImage			Image;
		VkImageLayout	OldLayout;
		uint32			BaseLevel;
		uint32			LevelCount;
		VkBuffer		Staging;
		/// Bytes copied out of Staging (0 for a layout-only transition).
		VkDeviceSize	StagingBytes;
		uint32			RegionCount;
		VkBufferImageCopy	Regions[MaxTextureCopyRegions];
	};
	/// Copies requested while no frame was open (the engine sets textures up
	/// between two frames); beginFrame() replays them.
	std::vector<STextureCopy>	_PendingTextureCopies;
	/// Upload staging ring: host visible, written by the CPU and read by the
	/// copy commands, one half per frame in flight. A slice stays valid until
	/// the submission that reads it has finished, i.e. until the same frame slot
	/// comes round again (beginFrame() waits for that fence).
	VkBuffer				_StagingRing;
	VkDeviceMemory			_StagingRingMemory;
	void					*_StagingRingMapped;
	uint32					_StagingRingSize;
	uint32					_StagingRingOffset;
	uint32					_StagingRingFrameBase;
	uint32					_StagingRingHalf;
	/// Staging memory for one upload.
	struct STextureStaging
	{
		VkBuffer		Buffer;
		/// Dedicated allocation (too large for the ring) to retire with the
		/// frame; VK_NULL_HANDLE when Buffer is a slice of the ring.
		VkDeviceMemory	Dedicated;
		VkDeviceSize	Offset;
		void			*Mapped;
	};
	bool					beginUploadBatch();
	/// End the upload command buffer. Returns true when it holds copies and has
	/// to be submitted with the frame.
	bool					submitUploadBatch();
	/// Record one copy, or queue it for the next beginUploadBatch().
	bool					recordTextureCopy(const STextureCopy &copy);
	/// Reserve upload staging for `size` bytes in the current frame.
	bool					allocTextureStaging(VkDeviceSize size, STextureStaging &out);
	/// Host visible buffer for texture traffic that does not go through the
	/// staging ring: uploads too large for it (TRANSFER_SRC) and the one-shot
	/// BC self-test readback (TRANSFER_DST).
	VkBuffer				createStagingBuffer(VkDeviceSize size, VkDeviceMemory *memory, void **mapped,
								VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	/// Allocate a ring of `size` bytes (two halves) and retire the previous
	/// one at a frame boundary.
	bool					createStagingRing(uint32 size);
	bool					growStagingRing(uint32 required);
	/// Give the idle frames after loading their memory back: the ring is
	/// rebuilt at its initial size once no upload has used it for a while.
	void					shrinkStagingRing();
	/// Frames since the staging ring was last used; the idle shrink runs after
	/// a long enough stretch (a texture burst resets the counter).
	uint32					_StagingRingIdleFrames;
	/// Upload all mip levels of the bitmap.
	bool					uploadTexturePixels(ITexture &tex, CTextureVulkan &out);
	/// Upload a sub-rectangle of one mip level, leaving the rest of the image
	/// intact (used by touchRect() and uploadTexture()).
	bool					uploadTextureRegion(ITexture &tex, CTextureVulkan &out, const NLMISC::CRect &rect, uint mipLevel);
	/// stretchRect() implementation: scaled image blit between two NeL textures.
	bool					stretchRectImage(ITexture *srcText, NLMISC::CRect &srcRect, ITexture *destText, NLMISC::CRect &destRect);
	/// NLVK_TEXTEST=1: one-shot check of the compressed stretchRect paths (see
	/// driver_vulkan_target.cpp). Stage 0 = to run, 1 = waiting for the copy to
	/// complete, 2 = done.
	int						_BlitTestStage;
	bool					_BlitTestRequestOk;
	CTextureMem				_BlitTestSrc;
	CTextureMem				_BlitTestDst;
	void					runTextureBlitTest();
	void					checkTextureBlitTest();
	std::map<uint32, std::vector<VkVertexInputAttributeDescription> >	_VertexLayouts;

	// Per-frame index upload ring.
	// Per-draw uniform data (lighting + material + matrices + texgen + fog).
	// This layout must match the std140 NlSimple block in the shaders exactly.
	// Padding members only exist to make the C offsets line up with the
	// implicit std140 padding; the std140 member order is the GLSL declaration
	// order in s_UniformBlockGLSL (driver_vulkan_render.cpp).
	struct SSimpleUniforms
	{
		float	MVP[16];				// 0
		float	NormalMatrix[12];		// 64
		float	Color[4];				// 112
		float	Diffuse[4];				// 128
		float	Ambient[4];				// 144
		float	AlphaRef;				// 160
		sint32	Flags;					// 164
		uint32	TexEnv[4];				// 168
		uint32	UvRouting;				// 184
		uint32	RtFlipMask;				// 188 (V flip for render-target stages)
		float	LightDirOrPos[4][4];	// 192
		float	LightDiffuse[4][4];		// 256
		sint32	LightCount;				// 320
		sint32	Pad1[3];				// 324 (std140: texGenMode is ivec4-aligned)
		sint32	TexGenMode[4];			// 336 (-1 = disabled, else TexGen* mode)
		float	FogColor[4];			// 352
		float	FogParams[2];			// 368 (start, end)
		float	FogDensity;				// 376
		sint32	FogMode;				// 380
		sint32	FogEnabled;				// 384
		sint32	Pad2[3];				// 388 (std140: texGenPlane is vec4-aligned)
		// Four planes per stage (s, t, r, q), identity unless the engine ever
		// exposes GL's TEXTURE_GEN plane coefficients: the GL driver always uses
		// the default object/eye planes, so only the user texture matrix moves
		// the coordinates.
		float	TexGenPlane[4][4][4];	// 400
		float	TexMatrix[4][16];		// 656 (user texture matrix per stage)
		float	SpecularTexMtx[16];		// 912 (inverse view rotation, reflection texgen)
		float	ModelView[16];			// 976 (eye-space position for texgen and fog)
	};									// 1040
	static_assert(sizeof(SSimpleUniforms) == 1040, "SSimpleUniforms must match the std140 NlSimple block");

	VkBuffer				_UniformRing;
	VkDeviceMemory			_UniformRingMemory;
	void					*_UniformRingMapped;
	uint32					_UniformRingSize;
	uint32					_UniformRingOffset;
	/// Each frame writes into its own half of the ring: the previous frame can
	/// still be executing (and reading its descriptor blocks) while the next
	/// one is recorded. Without this, a draw of frame N would read the uniform
	/// block frame N+1 just wrote at the same offset.
	uint32					_UniformRingFrameBase;
	uint32					_UniformRingHalf;

	// Light state tracked from the IDriver calls.
	struct SLightState
	{
		CVector		DirOrPos;
		CRGBA		Diffuse;
		CRGBA		Ambient;
		uint8		Mode;
		bool		Enabled;
		CLightTableUBOEntry	Entry; // full std140 light table entry for the mega shaders
	};
	SLightState				_Lights[8];
	SLightState				_LightTable[128];
	uint32					_LightTableSize;
	bool					_LightTableMode;
	sint16					_LightIndices[8];
	uint8					_LightFactors[8];
	uint32					_LightCount;
	uint32					_NumPerPixelLights;
	CRGBA					_ObjectAmbient;
	CRGBA					_AmbientColor;

	/// Per-frame vertex upload ring for dynamic vertex buffers. Same layout
	/// and lifetime rules as the index ring below.
	VkBuffer				_VertexRing;
	VkDeviceMemory			_VertexRingMemory;
	void					*_VertexRingMapped;
	uint32					_VertexRingSize;
	uint32					_VertexRingOffset;
	uint32					_VertexRingFrameBase;
	uint32					_VertexRingHalf;
	VkBuffer				_IndexRing;
	VkDeviceMemory			_IndexRingMemory;
	void					*_IndexRingMapped;
	uint32					_IndexRingSize;
	uint32					_IndexRingOffset;
	uint32					_IndexRingDrawOffset;
	/// Per-frame halves, same reason as the uniform ring.
	uint32					_IndexRingFrameBase;
	uint32					_IndexRingHalf;

	// --- Cached draw state ---
	CMatrix					_GLProjMat;
	CMatrix					_ChangeBasis;
	CMatrix					_ModelViewMatrix;
	CVertexBuffer			*_CurrentVB;
	CIndexBuffer			*_CurrentIB;
	const uint8				*_IndexValues;
	uint32					_IndexFormat;
	bool					_BlendEnabled;
	uint32					_SrcBlend;
	uint32					_DstBlend;
	bool					_DepthTestEnabled;
	bool					_DepthWriteEnabled;
	uint32					_DepthFunc;
	CRGBA					_BlendConstantColor;
	CTextureVulkan			*_StageTextures[IDRV_PROGRAM_MAXSAMPLERS];
	CRGBA					_MaterialColor;
	uint32					_TexEnv[4];
	uint8					_MappedUV[4];
	bool					_MaterialLighting;
	bool					_MaterialLightedVertexColor;
	bool					_MaterialAlphaTest;
	float					_MaterialAlphaRef;
	bool					_MaterialTwoSided;
	CMaterial::TShader		_MaterialShader;
	bool					_TexturesDirty;
	uint32					_ClearColorMask;


public:
	// Used by the factory and by the vertex buffer driver infos.
	static const char		*driverName() { return "Vulkan"; }
	VkDevice				getDevice() const { return _Device; }
	bool					isFrameOpen() const { return _FrameOpen; }
	void					deferDestroyBuffer(VkBuffer buffer, VkDeviceMemory memory) { _PendingBufferDestroy.push_back(std::make_pair(_SwapBufferCounter, std::make_pair(buffer, memory))); }
	/// Release a user VP shader module and forget its cache entry.
	void					releaseUserVPShader(uint32 id);
	/// Release a user PP shader module and forget its cache entry.
	void					releaseUserPPShader(uint32 id);
	/// Destroy the Vulkan objects of a texture whose last ITexture is gone.
	void					releaseTextureDrvInfo(CTextureDrvInfosVulkan &info);

	uint32					findMemoryType(uint32 typeBits, VkMemoryPropertyFlags props) const;
	/// Reserve `size` bytes in the current frame's vertex ring (4-byte aligned)
	/// and return the offset, or 0xFFFFFFFF when the ring could not grow.
	uint32					allocVertexRing(uint32 size);
	VkBuffer				vertexRingBuffer() const { return _VertexRing; }
	void					*vertexRingMapped() const { return _VertexRingMapped; }
};

} /* namespace NLVULKAN */

#endif /* NL_DRIVER_VULKAN_H */
