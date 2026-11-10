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
	return false;
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
	return 0.f;
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
	return 0;
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
}

uint32 CDriverVulkan::getUsedTextureMemory() const
{
	return 0;
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

void CDriverVulkan::showCursor(bool b)
{
}

void CDriverVulkan::setMousePos(float x, float y)
{
}

void CDriverVulkan::setCapture(bool b)
{
}

bool CDriverVulkan::isSystemCursorCaptured()
{
	return false;
}

void CDriverVulkan::addCursor(const std::string &name, const NLMISC::CBitmap &bitmap)
{
}

void CDriverVulkan::setCursor(const std::string &name, NLMISC::CRGBA col, uint8 rot, sint hotSpotX, sint hotSpotY, bool forceRebuild)
{
}

void CDriverVulkan::setCursorScale(float scale)
{
}

bool CDriverVulkan::setRenderTarget(ITexture *tex, uint32 x, uint32 y, uint32 width, uint32 height, uint32 mipmapLevel, uint32 cubeFace)
{
	return false;
}

bool CDriverVulkan::getRenderTargetSize(uint32 &width, uint32 &height)
{
	return false;
}

bool CDriverVulkan::copyTargetToTexture(ITexture *tex, uint32 offsetx, uint32 offsety, uint32 x, uint32 y, uint32 width, uint32 height, uint32 mipmapLevel)
{
	return false;
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

bool CDriverVulkan::supportVertexProgram(CVertexProgram::TProfile profile) const
{
	return false;
}

bool CDriverVulkan::compileVertexProgram(CVertexProgram *program)
{
	return false;
}

bool CDriverVulkan::activeVertexProgram(CVertexProgram *program)
{
	return false;
}

bool CDriverVulkan::supportPixelProgram(CPixelProgram::TProfile profile) const
{
	return false;
}

bool CDriverVulkan::compilePixelProgram(CPixelProgram *program)
{
	return false;
}

bool CDriverVulkan::activePixelProgram(CPixelProgram *program)
{
	return false;
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

void CDriverVulkan::setUniform1f(TProgram program, uint index, float f0)
{
}

void CDriverVulkan::setUniform2f(TProgram program, uint index, float f0, float f1)
{
}

void CDriverVulkan::setUniform3f(TProgram program, uint index, float f0, float f1, float f2)
{
}

void CDriverVulkan::setUniform4f(TProgram program, uint index, float f0, float f1, float f2, float f3)
{
}

void CDriverVulkan::setUniform1i(TProgram program, uint index, sint32 i0)
{
}

void CDriverVulkan::setUniform2i(TProgram program, uint index, sint32 i0, sint32 i1)
{
}

void CDriverVulkan::setUniform3i(TProgram program, uint index, sint32 i0, sint32 i1, sint32 i2)
{
}

void CDriverVulkan::setUniform4i(TProgram program, uint index, sint32 i0, sint32 i1, sint32 i2, sint32 i3)
{
}

void CDriverVulkan::setUniform1ui(TProgram program, uint index, uint32 ui0)
{
}

void CDriverVulkan::setUniform2ui(TProgram program, uint index, uint32 ui0, uint32 ui1)
{
}

void CDriverVulkan::setUniform3ui(TProgram program, uint index, uint32 ui0, uint32 ui1, uint32 ui2)
{
}

void CDriverVulkan::setUniform4ui(TProgram program, uint index, uint32 ui0, uint32 ui1, uint32 ui2, uint32 ui3)
{
}

void CDriverVulkan::setUniform3f(TProgram program, uint index, const NLMISC::CVector& v)
{
}

void CDriverVulkan::setUniform4f(TProgram program, uint index, const NLMISC::CVector& v, float f3)
{
}

void CDriverVulkan::setUniform4f(TProgram program, uint index, const NLMISC::CRGBAF& rgba)
{
}

void CDriverVulkan::setUniform4x4f(TProgram program, uint index, const NLMISC::CMatrix& m)
{
}

void CDriverVulkan::setUniform4fv(TProgram program, uint index, size_t num, const float *src)
{
}

void CDriverVulkan::setUniform4iv(TProgram program, uint index, size_t num, const sint32 *src)
{
}

void CDriverVulkan::setUniform4uiv(TProgram program, uint index, size_t num, const uint32 *src)
{
}

void CDriverVulkan::setUniformMatrix(TProgram program, uint index, TMatrix matrix, TTransform transform)
{
}

void CDriverVulkan::setUniformFog(TProgram program, uint index)
{
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
	return false;
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

ITexture * CDriverVulkan::getRenderTarget() const
{
	return NULL;
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

bool CDriverVulkan::bindUniformBuffer(TUBBinding binding, CUniformBuffer *ub)
{
	return false;
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
