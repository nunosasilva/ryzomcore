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
#include "nel/misc/bitmap.h"
#include "nel/misc/event_emitter.h"

#if defined(NL_OS_UNIX) && !defined(NL_OS_MAC)
#include "unix_event_emitter.h"
#endif

#include <cstring>
#include <unordered_map>
#include <vector>

namespace NLVULKAN {

using namespace NL3D;
using namespace NLMISC;

class CDriverVulkan;

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

	VkBuffer	_VertexBuffer;
	VkDeviceMemory	_Memory;
	void		*_Mapped;
	uint32		_Size;
};

/// A Vulkan texture created on behalf of a NeL ITexture.
struct CTextureVulkan
{
	CTextureVulkan()
		: Image(VK_NULL_HANDLE), Memory(VK_NULL_HANDLE), View(VK_NULL_HANDLE), Sampler(VK_NULL_HANDLE)
		, Width(0), Height(0), Layers(1), HasAlpha(false), Uploaded(false) {}

	VkImage			Image;
	VkDeviceMemory	Memory;
	VkImageView		View;
	VkSampler		Sampler;
	uint32			Width;
	uint32			Height;
	uint32			Layers;
	bool			HasAlpha;
	bool			Uploaded;
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

	bool operator==(const CPipelineKey &o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
	bool operator<(const CPipelineKey &o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};

/// A compiled pipeline for a given key.
struct CPipelineVulkan
{
	VkPipeline			Pipeline;
};

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
	virtual bool supportBuiltinUBO() const override { return true; }
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
#if defined(NL_OS_UNIX) && !defined(NL_OS_MAC)
	NLMISC::CUnixEventEmitter _EventEmitter;
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

	VkFormat				_DepthFormat;
	VkImage					_DepthImage;
	VkDeviceMemory			_DepthMemory;
	VkImageView				_DepthView;

	VkCommandPool			_CommandPool;
	std::vector<VkCommandBuffer> _CommandBuffers;
	std::vector<VkFence>	_Fences;
	std::vector<VkSemaphore> _ImageAvailable;
	std::vector<VkSemaphore> _RenderFinished;

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

	bool					_FogEnabled;
	float					_FogStart;
	float					_FogEnd;
	NLMISC::CRGBA			_FogColor;
	TFogMode				_FogMode;
	float					_FogDensity;
	float					_DepthRangeNear;
	float					_DepthRangeFar;
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
	void					recreateSwapchain();
	void					recordFrameCommands(VkCommandBuffer cmd);
	bool					beginFrame();
	void					dumpScreenshot(const char *path);
	void					recordScreenshotCopy(VkCommandBuffer cmd);
	void					writeScreenshot(const char *path);

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
	void					commitPushConstants(const CPipelineKey &key, VkCommandBuffer cmd, CVertexBuffer &vb);

	// --- Textures (driver_vulkan_texture.cpp) ---
	CTextureVulkan			*getTextureVulkan(ITexture &tex);
	bool					uploadTextureVulkan(ITexture &tex, CTextureVulkan &out, bool upload);
	void					destroyTexture(CTextureVulkan &tex);
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

	std::map<ITexture *, CTextureVulkan>	_TexturesVulkan;
	std::vector<CTextureVulkan>				_PendingTextureDestroy;
	std::vector<std::pair<VkBuffer, VkDeviceMemory> >	_PendingBufferDestroy;
	bool					uploadTexturePixels(ITexture &tex, CTextureVulkan &out);
	std::map<uint32, std::vector<VkVertexInputAttributeDescription> >	_VertexLayouts;

	// Per-frame index upload ring.
	VkBuffer				_IndexRing;
	VkDeviceMemory			_IndexRingMemory;
	void					*_IndexRingMapped;
	uint32					_IndexRingSize;
	uint32					_IndexRingOffset;
	uint32					_IndexRingDrawOffset;

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
	void					deferDestroyBuffer(VkBuffer buffer, VkDeviceMemory memory) { _PendingBufferDestroy.push_back(std::make_pair(buffer, memory)); }

	uint32					findMemoryType(uint32 typeBits, VkMemoryPropertyFlags props) const;
};

} /* namespace NLVULKAN */

#endif /* NL_DRIVER_VULKAN_H */
