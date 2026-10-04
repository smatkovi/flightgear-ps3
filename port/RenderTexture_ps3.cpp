// RenderTexture for PS3: there are no pbuffers behind ps3gl, so a RenderTexture
// never initialises. Callers (cloud impostor cache, on-demand gauges) test
// IsInitialized()/the return values and fall back to drawing directly.

#include <simgear/compiler.h>
#include <simgear/screen/RenderTexture.h>

RenderTexture::RenderTexture(const char *)
:   _iWidth(0), _iHeight(0), _bIsTexture(false), _bIsDepthTexture(false),
    _bHasARBDepthTexture(false), _eUpdateMode(RT_COPY_TO_TEXTURE),
    _bInitialized(false), _iNumAuxBuffers(0), _bIsBufferBound(false),
    _iCurrentBoundBuffer(0), _iNumComponents(0), _iNumDepthBits(0),
    _iNumStencilBits(0), _bFloat(false), _bDoubleBuffered(false),
    _bPowerOf2(true), _bRectangle(false), _bMipmap(false),
    _bShareObjects(false), _bCopyContext(false), _pDisplay(0),
    _hGLContext(0), _hPBuffer(0), _hPreviousDrawable(0), _hPreviousContext(0),
    _iTextureTarget(GL_TEXTURE_2D), _iTextureID(0), _iDepthTextureID(0),
    _pPoorDepthTexture(0)
{
    _iNumColorBits[0] = _iNumColorBits[1] = _iNumColorBits[2] = _iNumColorBits[3] = 0;
}

RenderTexture::RenderTexture(int width, int height, bool, bool)
:   _iWidth(width), _iHeight(height), _bIsTexture(false), _bIsDepthTexture(false),
    _bHasARBDepthTexture(false), _eUpdateMode(RT_COPY_TO_TEXTURE),
    _bInitialized(false), _iNumAuxBuffers(0), _bIsBufferBound(false),
    _iCurrentBoundBuffer(0), _iNumComponents(0), _iNumDepthBits(0),
    _iNumStencilBits(0), _bFloat(false), _bDoubleBuffered(false),
    _bPowerOf2(true), _bRectangle(false), _bMipmap(false),
    _bShareObjects(false), _bCopyContext(false), _pDisplay(0),
    _hGLContext(0), _hPBuffer(0), _hPreviousDrawable(0), _hPreviousContext(0),
    _iTextureTarget(GL_TEXTURE_2D), _iTextureID(0), _iDepthTextureID(0),
    _pPoorDepthTexture(0)
{
    _iNumColorBits[0] = _iNumColorBits[1] = _iNumColorBits[2] = _iNumColorBits[3] = 0;
}

RenderTexture::~RenderTexture() {}

bool RenderTexture::Initialize(int width, int height, bool, bool)
{ _iWidth = width; _iHeight = height; return false; }
bool RenderTexture::Initialize(bool, bool, bool, bool, bool, unsigned int,
                               unsigned int, unsigned int, unsigned int, UpdateMode)
{ return false; }
bool RenderTexture::Reset(const char *, ...) { return false; }
bool RenderTexture::Reset(int, int) { return false; }
bool RenderTexture::Resize(int, int) { return false; }
bool RenderTexture::BeginCapture() { return false; }
bool RenderTexture::BeginCapture(RenderTexture *) { return false; }
bool RenderTexture::EndCapture() { return false; }
void RenderTexture::Bind() const {}
void RenderTexture::BindDepth() const {}
bool RenderTexture::BindBuffer(int) { return false; }
