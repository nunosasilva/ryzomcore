// Generated from the pure virtual list of IDriver. Methods that are not yet
// implemented by the Vulkan driver live here, so the real implementation files
// stay readable. Move a method out of this file when implementing it.
#include "stdvulkan.h"
#include "driver_vulkan.h"

using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

void CDriverVulkan::disableHardwareVertexProgram()
{
}

void CDriverVulkan::disableHardwarePixelProgram()
{
}

void CDriverVulkan::disableHardwareVertexArrayAGP()
{
}

void CDriverVulkan::disableHardwareTextureShader()
{
}

void CDriverVulkan::beginDialogMode()
{
}

void CDriverVulkan::endDialogMode()
{
}

bool CDriverVulkan::supportMonitorColorProperties() const
{
	return false;
}

bool CDriverVulkan::setMonitorColorProperties(const CMonitorColorProperties &properties)
{
	return false;
}

bool CDriverVulkan::copyTextToClipboard(const std::string &text)
{
	return false;
}

bool CDriverVulkan::pasteTextFromClipboard(std::string &text)
{
	return false;
}

uint8 CDriverVulkan::getBitPerPixel()
{
	return 24;
}

void CDriverVulkan::getZBuffer(std::vector<float> &zbuffer)
{
}

void CDriverVulkan::getBufferPart(CBitmap &bitmap, NLMISC::CRect &rect)
{
}

void CDriverVulkan::getZBufferPart(std::vector<float> &zbuffer, NLMISC::CRect &rect)
{
}

bool CDriverVulkan::fillBuffer(CBitmap &bitmap)
{
	return false;
}

void CDriverVulkan::setDepthRange(float znear, float zfar)
{
	_DepthRangeNear = znear;
	_DepthRangeFar = zfar;
}

void CDriverVulkan::getDepthRange(float &znear, float &zfar) const
{
	znear = _DepthRangeNear;
	zfar = _DepthRangeFar;
}

void CDriverVulkan::forceDXTCCompression(bool dxtcComp)
{
	_ForceDXTCCompression = dxtcComp;
}

void CDriverVulkan::setAnisotropicFilter(sint filter)
{
}

uint CDriverVulkan::getAnisotropicFilter() const
{
	return 1;
}

uint CDriverVulkan::getAnisotropicFilterMaximum() const
{
	return 16;
}

void CDriverVulkan::forceTextureResize(uint divisor)
{
}

uint CDriverVulkan::getNbTextureStages() const
{
	return 4;
}

void CDriverVulkan::getNumPerStageConstant(uint &lightedMaterial, uint &unlightedMaterial) const
{
	lightedMaterial = 4; unlightedMaterial = 4;
}

bool CDriverVulkan::isTextureRectangle(ITexture *tex) const
{
	return false;
}

bool CDriverVulkan::supportNonPowerOfTwoTextures() const
{
	return true;
}

bool CDriverVulkan::stretchRect(ITexture *srcText, NLMISC::CRect &srcRect, ITexture *destText, NLMISC::CRect &destRect)
{
	// Implemented in driver_vulkan_target.cpp, next to the other image copies.
	return stretchRectImage(srcText, srcRect, destText, destRect);
}

bool CDriverVulkan::supportPerPixelLighting(bool specular) const
{
	return false;
}

bool CDriverVulkan::supportWorldSpacePPL() const
{
	return false;
}

float CDriverVulkan::getClipSpaceZMin() const
{
	// Matrices from CMatrix::frustum()/ortho() are GL-style (z in [-1, 1]); the
	// vertex shader converts them to Vulkan's [0, 1] clip space.
	return -1.f;
}

void CDriverVulkan::forceNormalize(bool normalize)
{
}

bool CDriverVulkan::isForceNormalize() const
{
	return false;
}

bool CDriverVulkan::supportVertexBufferHard() const
{
	return true;
}

bool CDriverVulkan::supportVolatileVertexBuffer() const
{
	return true;
}

bool CDriverVulkan::supportIndexOffset() const
{
	return false;
}

bool CDriverVulkan::slowUnlockVertexBufferHard() const
{
	return false;
}

uint CDriverVulkan::getMaxVerticesByVertexBufferHard() const
{
	return 0xFFFFFFFF;
}

uint32 CDriverVulkan::getAvailableVertexAGPMemory()
{
	return 0xFFFFFFFF;
}

uint32 CDriverVulkan::getAvailableVertexVRAMMemory()
{
	return 0xFFFFFFFF;
}

void CDriverVulkan::setSwapVBLInterval(uint interval)
{
}

uint CDriverVulkan::getSwapVBLInterval()
{
	return 0;
}

void CDriverVulkan::profileRenderedPrimitives(CPrimitiveProfile &pIn, CPrimitiveProfile &pOut)
{
}

uint32 CDriverVulkan::profileAllocatedTextureMemory()
{
	return (uint32)std::min<uint64>(_AllocatedTextureMemory, 0xFFFFFFFFu);
}

uint32 CDriverVulkan::profileSetupedMaterials() const
{
	return 0;
}

uint32 CDriverVulkan::profileSetupedModelMatrix() const
{
	return 0;
}

void CDriverVulkan::enableUsedTextureMemorySum(bool enable)
{
	_UsedTextureMemorySumEnabled = enable;
	if (!enable)
		_TextureUsed.clear();
}

uint32 CDriverVulkan::getUsedTextureMemory() const
{
	uint64 memory = 0;
	for (std::set<CTextureDrvInfosVulkan *>::const_iterator it = _TextureUsed.begin(); it != _TextureUsed.end(); ++it)
		memory += (*it)->TextureMemory;
	return (uint32)std::min<uint64>(memory, 0xFFFFFFFFu);
}

void CDriverVulkan::startProfileVBHardLock()
{
}

void CDriverVulkan::endProfileVBHardLock(std::vector<std::string> &result)
{
}

void CDriverVulkan::profileVBHardAllocation(std::vector<std::string> &result)
{
}

void CDriverVulkan::startProfileIBLock()
{
}

void CDriverVulkan::endProfileIBLock(std::vector<std::string> &result)
{
}

void CDriverVulkan::profileIBAllocation(std::vector<std::string> &result)
{
}

uint32 CDriverVulkan::getImplementationVersion() const
{
	return 1;
}

sint CDriverVulkan::getTotalVideoMemory() const
{
	return 4194304;
}

bool CDriverVulkan::textureCoordinateAlternativeMode() const
{
	return false;
}

uint CDriverVulkan::getMaxLight() const
{
	return 8;
}

bool CDriverVulkan::isVertexProgramEmulated() const
{
	return false;
}

bool CDriverVulkan::supportPixelProgram(CPixelProgram::TProfile profile) const
{
	// Only the UBO-formatted GLSL profile; compilePixelProgram() and
	// activePixelProgram() implement it (driver_vulkan_vp.cpp).
	return _MegaResourcesReady && profile == IProgram::glsl300esf;
}

bool CDriverVulkan::supportGeometryProgram(CGeometryProgram::TProfile profile) const
{
	return false;
}

bool CDriverVulkan::compileGeometryProgram(CGeometryProgram *program)
{
	return false;
}

bool CDriverVulkan::activeGeometryProgram(CGeometryProgram *program)
{
	return false;
}

bool CDriverVulkan::isUniformProgramState()
{
	return false;
}

bool CDriverVulkan::supportCloudRenderSinglePass() const
{
	return false;
}

bool CDriverVulkan::supportBloomEffect() const
{
	// Bloom needs offscreen render targets (implemented) and stretchRect()
	// (implemented as a scaled vkCmdBlitImage): the blur passes draw with the
	// engine's texture-offset vertex program, which the converted nelvp path
	// runs on the mega pipeline.
	return true;
}

bool CDriverVulkan::supportVertexProgramDoubleSidedColor() const
{
	return false;
}

bool CDriverVulkan::supportTextureShaders() const
{
	return false;
}

bool CDriverVulkan::supportWaterShader() const
{
	return false;
}

bool CDriverVulkan::cubemapZPositiveForward() const
{
	return true;
}

bool CDriverVulkan::supportTextureAddrMode(CMaterial::TTexAddressingMode mode) const
{
	return false;
}

void CDriverVulkan::setMatrix2DForTextureOffsetAddrMode(const uint stage, const float mat[4])
{
}

bool CDriverVulkan::supportEMBM() const
{
	return false;
}

bool CDriverVulkan::isEMBMSupportedAtStage(uint stage) const
{
	return false;
}

void CDriverVulkan::setEMBMMatrix(const uint stage, const float mat[4])
{
}

bool CDriverVulkan::supportBlendConstantColor() const
{
	return true;
}

void CDriverVulkan::finish()
{
}

void CDriverVulkan::flush()
{
}

void CDriverVulkan::enablePolygonSmoothing(bool smooth)
{
}

bool CDriverVulkan::isPolygonSmoothingEnabled() const
{
	return false;
}

bool CDriverVulkan::supportMADOperator() const
{
	return true;
}

uint CDriverVulkan::getNumAdapter() const
{
	return 1;
}

bool CDriverVulkan::getAdapter(uint adapter, CAdapter &desc) const
{
	return false;
}

bool CDriverVulkan::setAdapter(uint adapter)
{
	return false;
}

bool CDriverVulkan::supportOcclusionQuery() const
{
	return false;
}

void CDriverVulkan::deleteOcclusionQuery(IOcclusionQuery *oq)
{
}

void CDriverVulkan::enableStencilTest(bool enable)
{
}

bool CDriverVulkan::isStencilTestEnabled() const
{
	return false;
}

void CDriverVulkan::stencilFunc(TStencilFunc stencilFunc, int ref, uint mask)
{
}

void CDriverVulkan::stencilOp(TStencilOp fail, TStencilOp zfail, TStencilOp zpass)
{
}

void CDriverVulkan::stencilMask(uint mask)
{
}

void CDriverVulkan::enableClipPlane(uint index, bool enable)
{
}

void CDriverVulkan::setClipPlane(uint index, const NLMISC::CPlane &plane)
{
}

NLMISC::IEventEmitter * CDriverVulkan::getEventEmitter()
{
	return &_EventEmitter;
}

emptyProc CDriverVulkan::getWindowProc()
{
	return NULL;
}

const char * CDriverVulkan::getDriverInformation()
{
	return "Vulkan";
}

const char * CDriverVulkan::getVideocardInformation()
{
	return _DeviceProps.deviceName;
}

IOcclusionQuery * CDriverVulkan::createOcclusionQuery()
{
	return NULL;
}

bool CDriverVulkan::initVertexBufferHard(uint agpMem, uint vramMem)
{
	return true;
}

uint CDriverVulkan::getMaxLightTableSize() const
{
	return 128;
}

void CDriverVulkan::enableVertexProgramDoubleSidedColor(bool doubleSided)
{
}

CVertexBuffer::TVertexColorType CDriverVulkan::getVertexColorFormat() const
{
	return CVertexBuffer::TRGBA;
}

void CDriverVulkan::startBench(bool wantStandardDeviation, bool quick, bool reset)
{
}

void CDriverVulkan::endBench()
{
}

void CDriverVulkan::displayBench(class NLMISC::CLog *log)
{
}

} /* namespace NLVULKAN */
