/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RenderSystemGLES.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "guilib/DirtyRegion.h"
#include "guilib/GUITextureGLES.h"
#include "platform/MessagePrinter.h"
#include "rendering/GLExtensions.h"
#include "rendering/MatrixGL.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/FileUtils.h"
#include "utils/GLUtils.h"
#include "utils/MathUtils.h"
#include "utils/SystemInfo.h"
#include "utils/TimeUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/LogThrottle.h"
#include "utils/log.h"
#include "guilib/Shader.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <chrono>

#if defined(TARGET_LINUX)
#include "utils/EGLUtils.h"
#endif

#include <cmath>

using namespace std::chrono_literals;

namespace
{
thread_local GLint tlsSavedGuiRenderTargetViewport[4]{};
thread_local GLint tlsSavedGuiRenderTargetScissor[4]{};
thread_local GLint tlsSavedGuiRenderTargetFramebuffer{0};
thread_local bool tlsGuiRenderTargetActive{false};
thread_local bool tlsGuiRenderTargetFillBypass{false};
thread_local bool tlsWorkerShaderScope{false};
thread_local unsigned int tlsWorkerShaderEpoch{0};
thread_local ShaderMethodGLES tlsMethod{ShaderMethodGLES::SM_DEFAULT};

constexpr GLenum GUI_TIME_ELAPSED_EXT = 0x88BF;
constexpr GLenum GUI_QUERY_RESULT_EXT = 0x8866;
constexpr GLenum GUI_QUERY_RESULT_AVAILABLE_EXT = 0x8867;
constexpr GLenum GUI_GPU_DISJOINT_EXT = 0x8FBB;
constexpr size_t GUI_RENDER_TIMER_SLOTS = 4;

using PFNGenQueriesEXT = void(GL_APIENTRYP)(GLsizei, GLuint*);
using PFNDeleteQueriesEXT = void(GL_APIENTRYP)(GLsizei, const GLuint*);
using PFNBeginQueryEXT = void(GL_APIENTRYP)(GLenum, GLuint);
using PFNEndQueryEXT = void(GL_APIENTRYP)(GLenum);
using PFNGetQueryObjectuivEXT = void(GL_APIENTRYP)(GLuint, GLenum, GLuint*);
using PFNGetQueryObjectui64vEXT = void(GL_APIENTRYP)(GLuint, GLenum, GLuint64*);

struct GuiRenderTimerApi
{
  PFNGenQueriesEXT genQueries{nullptr};
  PFNDeleteQueriesEXT deleteQueries{nullptr};
  PFNBeginQueryEXT beginQuery{nullptr};
  PFNEndQueryEXT endQuery{nullptr};
  PFNGetQueryObjectuivEXT getObjectuiv{nullptr};
  PFNGetQueryObjectui64vEXT getObjectui64v{nullptr};
  bool resolved{false};
  bool usable{false};
};

GuiRenderTimerApi& GetGuiRenderTimerApi()
{
  static GuiRenderTimerApi api;
  if (!api.resolved)
  {
    api.resolved = true;
    api.genQueries = reinterpret_cast<PFNGenQueriesEXT>(eglGetProcAddress("glGenQueriesEXT"));
    api.deleteQueries =
        reinterpret_cast<PFNDeleteQueriesEXT>(eglGetProcAddress("glDeleteQueriesEXT"));
    api.beginQuery = reinterpret_cast<PFNBeginQueryEXT>(eglGetProcAddress("glBeginQueryEXT"));
    api.endQuery = reinterpret_cast<PFNEndQueryEXT>(eglGetProcAddress("glEndQueryEXT"));
    api.getObjectuiv =
        reinterpret_cast<PFNGetQueryObjectuivEXT>(eglGetProcAddress("glGetQueryObjectuivEXT"));
    api.getObjectui64v =
        reinterpret_cast<PFNGetQueryObjectui64vEXT>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    api.usable = api.genQueries && api.deleteQueries && api.beginQuery && api.endQuery &&
                 api.getObjectuiv && api.getObjectui64v;
  }
  return api;
}

thread_local GLuint tlsGuiRenderTimerQuery[GUI_RENDER_TIMER_SLOTS]{};
thread_local bool tlsGuiRenderTimerPending[GUI_RENDER_TIMER_SLOTS]{};
thread_local size_t tlsGuiRenderTimerHead{0};
thread_local size_t tlsGuiRenderTimerTail{0};
thread_local bool tlsGuiRenderTimerActive{false};

class CGUIRenderTargetGLES final : public CGUIRenderTargetFBO
{
public:
  CGUIRenderTargetGLES(unsigned int width, unsigned int height)
    : m_width(width), m_height(height)
  {
  }

  ~CGUIRenderTargetGLES() override
  {
    if (m_depthBuffer != 0)
      glDeleteRenderbuffers(1, &m_depthBuffer);
    if (m_framebuffer != 0)
      glDeleteFramebuffers(1, &m_framebuffer);
    if (m_texture != 0)
      glDeleteTextures(1, &m_texture);
  }

  unsigned int GetWidth() const override { return m_width; }
  unsigned int GetHeight() const override { return m_height; }

  GLuint GetFramebuffer() const { return m_framebuffer; }
  GLuint GetTexture() const { return m_texture; }
  void SetFramebuffer(GLuint framebuffer) { m_framebuffer = framebuffer; }
  void SetTexture(GLuint texture) { m_texture = texture; }
  void SetDepthBuffer(GLuint depthBuffer) { m_depthBuffer = depthBuffer; }

private:
  unsigned int m_width{0};
  unsigned int m_height{0};
  GLuint m_framebuffer{0};
  GLuint m_texture{0};
  GLuint m_depthBuffer{0};
};
}

CRenderSystemGLES::CRenderSystemGLES()
 : CRenderSystemBase()
{
}

const std::array<std::unique_ptr<CGLESShader>, CRenderSystemGLES::SM_COUNT>& CRenderSystemGLES::
    activeShaderArray() const
{
  return tlsWorkerShaderScope ? m_pShaderWorker : m_pShader;
}

std::array<std::unique_ptr<CGLESShader>, CRenderSystemGLES::SM_COUNT>& CRenderSystemGLES::
    activeShaderArray()
{
  return tlsWorkerShaderScope ? m_pShaderWorker : m_pShader;
}

CGLESShader* CRenderSystemGLES::shader(ShaderMethodGLES m) const
{
  return activeShaderArray()[static_cast<size_t>(m)].get();
}

std::unique_ptr<CGLESShader>& CRenderSystemGLES::shaderSlot(ShaderMethodGLES m)
{
  return activeShaderArray()[static_cast<size_t>(m)];
}

void CRenderSystemGLES::SetThreadGuiShaderScope(bool worker)
{
  tlsWorkerShaderScope = worker;
}

void CRenderSystemGLES::ReleaseThreadGuiShaders()
{
  ReleaseShaders();
  tlsWorkerShaderEpoch = 0;

  auto& api = GetGuiRenderTimerApi();
  for (size_t i = 0; i < GUI_RENDER_TIMER_SLOTS; ++i)
  {
    if (tlsGuiRenderTimerQuery[i] != 0 && api.usable)
      api.deleteQueries(1, &tlsGuiRenderTimerQuery[i]);
    tlsGuiRenderTimerQuery[i] = 0;
    tlsGuiRenderTimerPending[i] = false;
  }
  tlsGuiRenderTimerHead = 0;
  tlsGuiRenderTimerTail = 0;
  tlsGuiRenderTimerActive = false;
}

unsigned int CRenderSystemGLES::GetGuiShaderEpoch() const
{
  return m_guiShaderEpoch.load(std::memory_order_relaxed);
}

bool CRenderSystemGLES::InitRenderSystem()
{
  GLint maxTextureSize;

  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

  m_maxTextureSize = maxTextureSize;

  // Get the GLES version number
  m_RenderVersion = "<none>";
  m_RenderVersionMajor = 0;
  m_RenderVersionMinor = 0;

  auto ver = (const char*)glGetString(GL_VERSION);
  if (ver != nullptr)
  {
    sscanf(ver, "%d.%d", &m_RenderVersionMajor, &m_RenderVersionMinor);
    if (!m_RenderVersionMajor)
      sscanf(ver, "%*s %*s %d.%d", &m_RenderVersionMajor, &m_RenderVersionMinor);
    m_RenderVersion = ver;
  }

  // Get our driver vendor and renderer
  auto tmpVendor = (const char*) glGetString(GL_VENDOR);
  m_RenderVendor.clear();
  if (tmpVendor != nullptr)
    m_RenderVendor = tmpVendor;

  auto tmpRenderer = (const char*) glGetString(GL_RENDERER);
  m_RenderRenderer.clear();
  if (tmpRenderer != nullptr)
    m_RenderRenderer = tmpRenderer;

  m_RenderExtensions = "";

  auto tmpExtensions = (const char*) glGetString(GL_EXTENSIONS);
  if (tmpExtensions != nullptr)
  {
    m_RenderExtensions += tmpExtensions;
    m_RenderExtensions += " ";
  }

#if defined(GL_KHR_debug) && defined(TARGET_LINUX)
  if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_openGlDebugging)
  {
    if (IsExtSupported("GL_KHR_debug"))
    {
      auto glDebugMessageCallback = CEGLUtils::GetRequiredProcAddress<PFNGLDEBUGMESSAGECALLBACKKHRPROC>("glDebugMessageCallbackKHR");
      auto glDebugMessageControl = CEGLUtils::GetRequiredProcAddress<PFNGLDEBUGMESSAGECONTROLKHRPROC>("glDebugMessageControlKHR");

      glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
      glDebugMessageCallback(KODI::UTILS::GL::GlErrorCallback, nullptr);

      // ignore shader compilation information
      glDebugMessageControl(GL_DEBUG_SOURCE_SHADER_COMPILER_KHR, GL_DEBUG_TYPE_OTHER_KHR, GL_DONT_CARE, 0, nullptr, GL_FALSE);

      CLog::Log(LOGDEBUG, "OpenGL(ES): debugging enabled");
    }
    else
    {
      CLog::Log(LOGDEBUG, "OpenGL(ES): debugging requested but the required extension isn't available (GL_KHR_debug)");
    }
  }
#endif

  // Shut down gracefully if OpenGL context could not be allocated
  if (m_RenderVersionMajor == 0)
  {
    CLog::Log(LOGFATAL, "Can not initialize OpenGL context. Exiting");
    CMessagePrinter::DisplayError("ERROR: Can not initialize OpenGL context. Exiting");
    return false;
  }

  LogGraphicsInfo();

  m_bRenderCreated = true;

  Shaders::LogShaderBinaryCacheState();
  InitialiseShaders();
  WarmAllGuiHdrModeShaderCaches();

  CGUITextureGLES::Register();

  return true;
}

bool CRenderSystemGLES::ResetRenderSystem(int width, int height)
{
  m_width = width;
  m_height = height;

  glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
  CalculateMaxTexturesize();

  CRect rect( 0, 0, width, height );
  SetViewPort( rect );

  glEnable(GL_SCISSOR_TEST);

  glMatrixProject.Clear();
  glMatrixProject->LoadIdentity();
  glMatrixProject->Ortho(0.0f, width-1, height-1, 0.0f, -1.0f, 1.0f);
  glMatrixProject.Load();

  glMatrixModview.Clear();
  glMatrixModview->LoadIdentity();
  glMatrixModview.Load();

  glMatrixTexture.Clear();
  glMatrixTexture->LoadIdentity();
  glMatrixTexture.Load();

  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glEnable(GL_BLEND); // Turn Blending On

  return true;
}

bool CRenderSystemGLES::DestroyRenderSystem()
{
  ResetScissors();
  CDirtyRegionList dirtyRegions;
  CDirtyRegion dirtyWindow(CServiceBroker::GetWinSystem()->GetGfxContext().GetViewWindow());
  dirtyRegions.push_back(dirtyWindow);

  ClearBuffers(0);
  glFinish();
  PresentRenderImpl(true);

  ReleaseShaders();
  for (auto& workerSlot : m_pShaderWorker)
    workerSlot.reset();
  m_bRenderCreated = false;

  return true;
}

bool CRenderSystemGLES::BeginRender()
{
  if (!m_bRenderCreated)
    return false;

  const bool useLimited = CServiceBroker::GetWinSystem()->UseLimitedColor();
  const GuiHdr useGuiHdr = CServiceBroker::GetWinSystem()->GetGfxContext().GetGuiHdr();

  if (m_limitedColorRange != useLimited || m_guiHdr != useGuiHdr)
  {
    ReleaseShaders();

    m_limitedColorRange = useLimited;
    m_guiHdr = useGuiHdr;

    InitialiseShaders();
    m_guiShaderEpoch.fetch_add(1, std::memory_order_relaxed);
  }

  CGLESShader::RefreshFrameGuiValues();

  return true;
}

bool CRenderSystemGLES::EndRender()
{
  if (!m_bRenderCreated)
    return false;

  return true;
}

bool CRenderSystemGLES::SupportsGuiRenderTargets() const
{
  return m_bRenderCreated;
}

std::unique_ptr<CGUIRenderTargetFBO> CRenderSystemGLES::CreateGuiRenderTarget(unsigned int width,
                                                                           unsigned int height)
{
  if (!m_bRenderCreated || width == 0 || height == 0)
    return {};

  auto target = std::make_unique<CGUIRenderTargetGLES>(width, height);

  GLint previousFramebuffer = 0;
  GLint previousRenderbuffer = 0;
  GLint previousTexture = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

  GLuint texture = 0;
  GLuint framebuffer = 0;
  GLuint depthBuffer = 0;

  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

  glGenRenderbuffers(1, &depthBuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer);

  static std::atomic<bool> s_guiTargetFailLogged{false};
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    if (!s_guiTargetFailLogged.exchange(true, std::memory_order_relaxed))
      logM(LOGERROR, "GLES: failed to create GUI render target {}x{} (status: 0x{:04x})", width,
           height, glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (depthBuffer != 0)
      glDeleteRenderbuffers(1, &depthBuffer);
    if (framebuffer != 0)
      glDeleteFramebuffers(1, &framebuffer);
    if (texture != 0)
      glDeleteTextures(1, &texture);
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, previousRenderbuffer);
    glBindTexture(GL_TEXTURE_2D, previousTexture);
    return {};
  }
  s_guiTargetFailLogged.store(false, std::memory_order_relaxed);

  target->SetTexture(texture);
  target->SetFramebuffer(framebuffer);
  target->SetDepthBuffer(depthBuffer);

  glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, previousRenderbuffer);
  glBindTexture(GL_TEXTURE_2D, previousTexture);

  return target;
}

bool CRenderSystemGLES::BeginGuiRenderTarget(CGUIRenderTargetFBO& target)
{
  if (!BindGuiRenderTarget(target))
    return false;

  return ClearBuffers(0);
}

bool CRenderSystemGLES::BeginGuiRenderTargetPersistent(CGUIRenderTargetFBO& target, bool clearColor)
{
  if (!BindGuiRenderTarget(target))
    return false;

  if (clearColor)
    return ClearBuffers(0);

  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings = settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
  if (advancedSettings && advancedSettings->m_guiFrontToBackRendering)
  {
    glClearDepthf(0);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
  }

  return true;
}

bool CRenderSystemGLES::SupportsGuiRenderTargetConvert() const
{
  return m_srgbCompositeEnabled && m_guiHdr != GuiHdr::SDR &&
         shader(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT) != nullptr;
}

bool CRenderSystemGLES::BindGuiRenderTarget(CGUIRenderTargetFBO& target)
{
  auto* glesTarget = dynamic_cast<CGUIRenderTargetGLES*>(&target);
  if (!m_bRenderCreated || !glesTarget || tlsGuiRenderTargetActive)
    return false;

  if (tlsWorkerShaderScope)
  {
    const unsigned int epoch = m_guiShaderEpoch.load(std::memory_order_relaxed);
    if (tlsWorkerShaderEpoch != epoch)
    {
      ReleaseShaders();
      InitialiseShaders();
      tlsWorkerShaderEpoch = epoch;
    }
  }

  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &tlsSavedGuiRenderTargetFramebuffer);
  glGetIntegerv(GL_VIEWPORT, tlsSavedGuiRenderTargetViewport);
  glGetIntegerv(GL_SCISSOR_BOX, tlsSavedGuiRenderTargetScissor);

  glBindFramebuffer(GL_FRAMEBUFFER, glesTarget->GetFramebuffer());
  glViewport(0, 0, glesTarget->GetWidth(), glesTarget->GetHeight());
  glScissor(0, 0, glesTarget->GetWidth(), glesTarget->GetHeight());

  m_viewPort[0] = 0;
  m_viewPort[1] = 0;
  m_viewPort[2] = glesTarget->GetWidth();
  m_viewPort[3] = glesTarget->GetHeight();
  tlsGuiRenderTargetActive = true;
  tlsGuiRenderTargetFillBypass = m_srgbCompositeEnabled && m_guiHdr != GuiHdr::SDR &&
                                 shader(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT) != nullptr;

  return true;
}

void CRenderSystemGLES::EndGuiRenderTarget(CGUIRenderTargetFBO& target)
{
  auto* glesTarget = dynamic_cast<CGUIRenderTargetGLES*>(&target);
  if (!glesTarget || !tlsGuiRenderTargetActive)
    return;

  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(tlsSavedGuiRenderTargetFramebuffer));
  glViewport(tlsSavedGuiRenderTargetViewport[0], tlsSavedGuiRenderTargetViewport[1],
             tlsSavedGuiRenderTargetViewport[2], tlsSavedGuiRenderTargetViewport[3]);
  glScissor(tlsSavedGuiRenderTargetScissor[0], tlsSavedGuiRenderTargetScissor[1],
            tlsSavedGuiRenderTargetScissor[2], tlsSavedGuiRenderTargetScissor[3]);

  m_viewPort[0] = tlsSavedGuiRenderTargetViewport[0];
  m_viewPort[1] = tlsSavedGuiRenderTargetViewport[1];
  m_viewPort[2] = tlsSavedGuiRenderTargetViewport[2];
  m_viewPort[3] = tlsSavedGuiRenderTargetViewport[3];
  tlsGuiRenderTargetActive = false;
  tlsGuiRenderTargetFillBypass = false;
}

bool CRenderSystemGLES::RenderGuiRenderTarget(const CGUIRenderTargetFBO& target, bool replace)
{
  const auto* glesTarget = dynamic_cast<const CGUIRenderTargetGLES*>(&target);
  if (!m_bRenderCreated || !glesTarget)
    return false;

  const float fboWidth = static_cast<float>(glesTarget->GetWidth());
  const float fboHeight = static_cast<float>(glesTarget->GetHeight());
  if (fboWidth <= 0.0f || fboHeight <= 0.0f)
    return false;
  const CRect full(0.0f, 0.0f, fboWidth, fboHeight);

  constexpr size_t maxQuads = CGUIRenderTargetFBO::MAX_CONTENT_RECTS;
  CRect quads[maxQuads];
  size_t quadCount = 0;
  const std::vector<CRect>& content = target.GetContentRects();
  if (content.empty() || content.size() > maxQuads)
  {
    quads[quadCount++] = full;
  }
  else
  {
    for (const CRect& candidate : content)
    {
      CRect clipped = candidate;
      clipped.Intersect(full);
      if (!clipped.IsEmpty())
        quads[quadCount++] = clipped;
    }
  }
  if (quadCount == 0)
    quads[quadCount++] = full;

  GLubyte idx[4] = {0, 1, 3, 2};
  GLfloat ver[4][3] = {};
  GLfloat tex[4][2] = {};

  const bool convertOnComposite = m_srgbCompositeEnabled && m_guiHdr != GuiHdr::SDR &&
                                  shader(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT) != nullptr;
  const ShaderMethodGLES method = convertOnComposite ? ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT
                                                     : ShaderMethodGLES::SM_TEXTURE_RAW;
  if (!shader(method))
    return false;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, glesTarget->GetTexture());
  if (replace)
  {
    glDisable(GL_BLEND);
  }
  else
  {
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
  }
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);

  EnableGUIShader(method);

  GLint posLoc = GUIShaderGetPos();
  GLint tex0Loc = GUIShaderGetCoord0();
  GLint uniColLoc = GUIShaderGetUniCol();
  GLint depthLoc = GUIShaderGetDepth();

  glVertexAttribPointer(posLoc, 3, GL_FLOAT, 0, 0, ver);
  glVertexAttribPointer(tex0Loc, 2, GL_FLOAT, 0, 0, tex);
  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(tex0Loc);

  glUniform4f(uniColLoc, 1.0f, 1.0f, 1.0f, 1.0f);
  glUniform1f(depthLoc, 0.0f);

  for (size_t quad = 0; quad < quadCount; ++quad)
  {
    const CRect& rect = quads[quad];
    ver[0][0] = rect.x1;
    ver[0][1] = rect.y1;
    ver[1][0] = rect.x2;
    ver[1][1] = rect.y1;
    ver[2][0] = rect.x2;
    ver[2][1] = rect.y2;
    ver[3][0] = rect.x1;
    ver[3][1] = rect.y2;

    const float u1 = rect.x1 / fboWidth;
    const float u2 = rect.x2 / fboWidth;
    const float v1 = 1.0f - rect.y1 / fboHeight;
    const float v2 = 1.0f - rect.y2 / fboHeight;
    tex[0][0] = u1;
    tex[0][1] = v1;
    tex[1][0] = u2;
    tex[1][1] = v1;
    tex[2][0] = u2;
    tex[2][1] = v2;
    tex[3][0] = u1;
    tex[3][1] = v2;

    glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);
  }

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(tex0Loc);
  DisableGUIShader();

  auto& depthGfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  depthGfx.SetRenderOrder(depthGfx.GetRenderOrder());
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  return true;
}

void* CRenderSystemGLES::CreateGuiRenderFence()
{
  if (!m_bRenderCreated)
    return nullptr;
  return glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

bool CRenderSystemGLES::WaitGuiRenderFence(void* fence, bool poll)
{
  if (!fence)
    return true;
  constexpr GLuint64 boundedWaitNs = 100000000;
  const GLenum result = glClientWaitSync(static_cast<GLsync>(fence), GL_SYNC_FLUSH_COMMANDS_BIT,
                                         poll ? 0 : boundedWaitNs);
  return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
}

bool CRenderSystemGLES::WaitGuiRenderFenceBounded(void* fence, uint64_t maxWaitNs)
{
  if (!fence)
    return true;
  const GLenum result = glClientWaitSync(static_cast<GLsync>(fence), GL_SYNC_FLUSH_COMMANDS_BIT,
                                         static_cast<GLuint64>(maxWaitNs));
  return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
}

void CRenderSystemGLES::DeleteGuiRenderFence(void* fence)
{
  if (fence)
    glDeleteSync(static_cast<GLsync>(fence));
}

bool CRenderSystemGLES::SupportsGuiRenderTimer() const
{
  if (!m_bRenderCreated || !IsExtSupported("GL_EXT_disjoint_timer_query"))
    return false;
  return GetGuiRenderTimerApi().usable;
}

void CRenderSystemGLES::BeginGuiRenderTimer()
{
  if (tlsGuiRenderTimerActive || tlsGuiRenderTimerPending[tlsGuiRenderTimerHead])
    return;
  auto& api = GetGuiRenderTimerApi();
  if (!api.usable)
    return;
  GLuint& query = tlsGuiRenderTimerQuery[tlsGuiRenderTimerHead];
  if (query == 0)
    api.genQueries(1, &query);
  if (query == 0)
    return;
  api.beginQuery(GUI_TIME_ELAPSED_EXT, query);
  tlsGuiRenderTimerActive = true;
}

void CRenderSystemGLES::EndGuiRenderTimer()
{
  if (!tlsGuiRenderTimerActive)
    return;
  auto& api = GetGuiRenderTimerApi();
  api.endQuery(GUI_TIME_ELAPSED_EXT);
  tlsGuiRenderTimerActive = false;
  tlsGuiRenderTimerPending[tlsGuiRenderTimerHead] = true;
  tlsGuiRenderTimerHead = (tlsGuiRenderTimerHead + 1) % GUI_RENDER_TIMER_SLOTS;
}

bool CRenderSystemGLES::PollGuiRenderTimerNs(uint64_t& elapsedNs)
{
  if (!tlsGuiRenderTimerPending[tlsGuiRenderTimerTail])
    return false;
  auto& api = GetGuiRenderTimerApi();
  if (!api.usable)
    return false;
  const GLuint query = tlsGuiRenderTimerQuery[tlsGuiRenderTimerTail];
  GLuint available = 0;
  api.getObjectuiv(query, GUI_QUERY_RESULT_AVAILABLE_EXT, &available);
  if (!available)
    return false;
  GLuint64 result = 0;
  api.getObjectui64v(query, GUI_QUERY_RESULT_EXT, &result);
  GLint disjoint = 0;
  glGetIntegerv(GUI_GPU_DISJOINT_EXT, &disjoint);
  tlsGuiRenderTimerPending[tlsGuiRenderTimerTail] = false;
  tlsGuiRenderTimerTail = (tlsGuiRenderTimerTail + 1) % GUI_RENDER_TIMER_SLOTS;
  if (disjoint)
    return false;
  elapsedNs = result;
  return true;
}

void CRenderSystemGLES::EstablishGuiRenderBaseline(unsigned int width, unsigned int height)
{
  if (!m_bRenderCreated)
    return;
  glEnable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glActiveTexture(GL_TEXTURE0);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glViewport(0, 0, width, height);
  glScissor(0, 0, width, height);
  m_viewPort[0] = 0;
  m_viewPort[1] = 0;
  m_viewPort[2] = width;
  m_viewPort[3] = height;
}

void CRenderSystemGLES::InvalidateColorBuffer()
{
  if (!m_bRenderCreated)
    return;

  // some platforms prefer a clear, instead of rendering over
  if (!CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_guiGeometryClear)
  {
    ClearBuffers(0);
    return;
  }

  if (!CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_guiFrontToBackRendering)
    return;

  glClearDepthf(0);
  glDepthMask(true);
  glClear(GL_DEPTH_BUFFER_BIT);
}

bool CRenderSystemGLES::ClearBuffers(UTILS::COLOR::Color color)
{
  if (!m_bRenderCreated)
    return false;

  float r = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::R, color) / 255.0f;
  float g = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::G, color) / 255.0f;
  float b = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::B, color) / 255.0f;
  float a = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::A, color) / 255.0f;

  glClearColor(r, g, b, a);

  GLbitfield flags = GL_COLOR_BUFFER_BIT;

  if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_guiFrontToBackRendering)
  {
    glClearDepthf(0);
    glDepthMask(GL_TRUE);
    flags |= GL_DEPTH_BUFFER_BIT;
  }

  glClear(flags);

  return true;
}

bool CRenderSystemGLES::IsExtSupported(const char* extension) const
{
  if (strcmp( extension, "GL_EXT_framebuffer_object" ) == 0)
  {
    // GLES has FBO as a core element, not an extension!
    return true;
  }
  else
  {
    std::string name;
    name  = " ";
    name += extension;
    name += " ";

    return m_RenderExtensions.find(name) != std::string::npos;
  }
}

void CRenderSystemGLES::PresentRender(bool rendered, bool videoLayer)
{
  SetVSync(true);

  if (!m_bRenderCreated)
    return;

  PresentRenderImpl(rendered);

  static auto s_lastRenderedFrame = std::chrono::steady_clock::now();
  if (rendered)
    s_lastRenderedFrame = std::chrono::steady_clock::now();

  // if video is rendered to a separate layer, we should not block this thread
  if (!rendered && !videoLayer)
  {
    auto sleepTime = 40ms;
    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    const auto advancedSettings =
        settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
    const int activeWindow = advancedSettings ? advancedSettings->m_guiSkipSleepActiveWindow : 0;
    if (activeWindow > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s_lastRenderedFrame) <
            std::chrono::milliseconds(activeWindow))
    {
      const float fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
      const auto interval = std::chrono::milliseconds(
          static_cast<int64_t>(1000.0f / (fps > 1.0f ? fps : 60.0f)));
      if (interval < sleepTime)
        sleepTime = interval;
    }
    KODI::TIME::Sleep(sleepTime);
  }
}

void CRenderSystemGLES::SetVSync(bool enable)
{
  if (m_bVsyncInit)
    return;

  if (!m_bRenderCreated)
    return;

  if (enable)
    CLog::Log(LOGINFO, "GLES: Enabling VSYNC");
  else
    CLog::Log(LOGINFO, "GLES: Disabling VSYNC");

  m_bVsyncInit = true;

  SetVSyncImpl(enable);
}

void CRenderSystemGLES::CaptureStateBlock()
{
  if (!m_bRenderCreated)
    return;

  glMatrixProject.Push();
  glMatrixModview.Push();
  glMatrixTexture.Push();

  glDisable(GL_SCISSOR_TEST); // fixes FBO corruption on Macs
  glActiveTexture(GL_TEXTURE0);
//! @todo - NOTE: Only for Screensavers & Visualisations
//  glColor3f(1.0, 1.0, 1.0);
}

void CRenderSystemGLES::ApplyStateBlock()
{
  if (!m_bRenderCreated)
    return;

  glMatrixProject.PopLoad();
  glMatrixModview.PopLoad();
  glMatrixTexture.PopLoad();
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_BLEND);
  glEnable(GL_SCISSOR_TEST);
  glClear(GL_DEPTH_BUFFER_BIT);
}

void CRenderSystemGLES::SetCameraPosition(const CPoint &camera, int screenWidth, int screenHeight, float stereoFactor)
{
  if (!m_bRenderCreated)
    return;

  CPoint offset = camera - CPoint(screenWidth*0.5f, screenHeight*0.5f);

  float w = (float)m_viewPort[2]*0.5f;
  float h = (float)m_viewPort[3]*0.5f;

  glMatrixModview->LoadIdentity();
  glMatrixModview->Translatef(-(w + offset.x - stereoFactor), +(h + offset.y), 0);
  glMatrixModview->LookAt(0.0f, 0.0f, -2.0f * h, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f);
  glMatrixModview.Load();

  glMatrixProject->LoadIdentity();
  glMatrixProject->Frustum( (-w - offset.x)*0.5f, (w - offset.x)*0.5f, (-h + offset.y)*0.5f, (h + offset.y)*0.5f, h, 100*h);
  glMatrixProject.Load();
}

void CRenderSystemGLES::Project(float &x, float &y, float &z)
{
  GLfloat coordX, coordY, coordZ;
  if (CMatrixGL::Project(x, y, z, glMatrixModview.Get(), glMatrixProject.Get(), m_viewPort, &coordX, &coordY, &coordZ))
  {
    x = coordX;
    y = (float)(m_viewPort[1] + m_viewPort[3] - coordY);
    z = 0;
  }
}

void CRenderSystemGLES::CalculateMaxTexturesize()
{
  // GLES cannot do PROXY textures to determine maximum size,
  CLog::Log(LOGINFO, "GLES: Maximum texture width: {}", m_maxTextureSize);
}

void CRenderSystemGLES::GetViewPort(CRect& viewPort)
{
  if (!m_bRenderCreated)
    return;

  viewPort.x1 = m_viewPort[0];
  viewPort.y1 = m_height - m_viewPort[1] - m_viewPort[3];
  viewPort.x2 = m_viewPort[0] + m_viewPort[2];
  viewPort.y2 = viewPort.y1 + m_viewPort[3];
}

void CRenderSystemGLES::SetViewPort(const CRect& viewPort)
{
  if (!m_bRenderCreated)
    return;

  glScissor((GLint) viewPort.x1, (GLint) (m_height - viewPort.y1 - viewPort.Height()), (GLsizei) viewPort.Width(), (GLsizei) viewPort.Height());
  glViewport((GLint) viewPort.x1, (GLint) (m_height - viewPort.y1 - viewPort.Height()), (GLsizei) viewPort.Width(), (GLsizei) viewPort.Height());
  m_viewPort[0] = viewPort.x1;
  m_viewPort[1] = m_height - viewPort.y1 - viewPort.Height();
  m_viewPort[2] = viewPort.Width();
  m_viewPort[3] = viewPort.Height();
}

bool CRenderSystemGLES::ScissorsCanEffectClipping()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->HardwareClipIsPossible();

  return false;
}

CRect CRenderSystemGLES::ClipRectToScissorRect(const CRect &rect)
{
  if (!shader(tlsMethod))
    return CRect();
  float xFactor = shader(tlsMethod)->GetClipXFactor();
  float xOffset = shader(tlsMethod)->GetClipXOffset();
  float yFactor = shader(tlsMethod)->GetClipYFactor();
  float yOffset = shader(tlsMethod)->GetClipYOffset();
  return CRect(rect.x1 * xFactor + xOffset,
               rect.y1 * yFactor + yOffset,
               rect.x2 * xFactor + xOffset,
               rect.y2 * yFactor + yOffset);
}

void CRenderSystemGLES::SetScissors(const CRect &rect)
{
  if (!m_bRenderCreated)
    return;
  GLint x1 = static_cast<GLint>(std::floor(rect.x1));
  GLint y1 = static_cast<GLint>(std::floor(rect.y1));
  GLint x2 = static_cast<GLint>(std::ceil(rect.x2));
  GLint y2 = static_cast<GLint>(std::ceil(rect.y2));
  glScissor(x1, m_height - y2, x2-x1, y2-y1);
}

void CRenderSystemGLES::ResetScissors()
{
  SetScissors(CRect(0, 0, (float)m_width, (float)m_height));
}

void CRenderSystemGLES::SetDepthCulling(DEPTH_CULLING culling)
{
  if (culling == DEPTH_CULLING_OFF)
  {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
  }
  else if (culling == DEPTH_CULLING_BACK_TO_FRONT)
  {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_GEQUAL);
  }
  else if (culling == DEPTH_CULLING_FRONT_TO_BACK)
  {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GREATER);
  }
}

void CRenderSystemGLES::InitialiseShaders()
{
  std::string defines;
  std::string definesHdrPgsPqOutput;
  std::string definesHdrPgsSdrOutput;
  m_limitedColorRange = CServiceBroker::GetWinSystem()->UseLimitedColor();
  if (m_limitedColorRange)
  {
    defines += "#define KODI_LIMITED_RANGE 1\n";
    definesHdrPgsPqOutput += "#define KODI_LIMITED_RANGE 1\n";
    definesHdrPgsSdrOutput += "#define KODI_LIMITED_RANGE 1\n";
  }

  definesHdrPgsPqOutput += "#define KODI_HDR_PGS_PQ_OUTPUT 1\n";
  definesHdrPgsPqOutput += "#define KODI_PREMULTIPLIED_ALPHA 1\n";
  definesHdrPgsSdrOutput += "#define KODI_HDR_PGS_SDR_OUTPUT 1\n";
  definesHdrPgsSdrOutput += "#define KODI_PREMULTIPLIED_ALPHA 1\n";

  m_guiHdr = CServiceBroker::GetWinSystem()->GetGfxContext().GetGuiHdr();
  if (m_guiHdr == GuiHdr::HDR_PQ)
    defines += "#define KODI_TRANSFER_PQ 1\n";
  else if (m_guiHdr == GuiHdr::HDR)
    defines += "#define KODI_TRANSFER_HDR 1\n";
#if defined(HAS_LIBAMCODEC)
  else
    defines += "#define KODI_GUI_SDR_SATURATION 1\n";
#endif

  std::string definesPma = defines + "#define KODI_PREMULTIPLIED_ALPHA 1\n";
  std::string definesSdrImageSubs = definesPma + "#define KODI_SDR_IMAGE_SUBS 1\n";

  shaderSlot(ShaderMethodGLES::SM_DEFAULT) =
      std::make_unique<CGLESShader>("gles_shader.vert", "gles_shader_default.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_DEFAULT)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_DEFAULT)->Free();
    shaderSlot(ShaderMethodGLES::SM_DEFAULT).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_default.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE) =
      std::make_unique<CGLESShader>("gles_shader_texture.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_111R) =
      std::make_unique<CGLESShader>("gles_shader_texture_111r.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_111R)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_111R)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_111R).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture_111r.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_MULTI) =
      std::make_unique<CGLESShader>("gles_shader_multi.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_MULTI)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_MULTI)->Free();
    shaderSlot(ShaderMethodGLES::SM_MULTI).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_multi.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R) =
      std::make_unique<CGLESShader>("gles_shader_multi_rgba_111r.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R)->Free();
    shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_multi_rgba_111r.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_FONTS) =
      std::make_unique<CGLESShader>("gles_shader_simple.vert", "gles_shader_fonts.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_FONTS)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_FONTS)->Free();
    shaderSlot(ShaderMethodGLES::SM_FONTS).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_fonts.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_FONTS_SHADER_CLIP) =
      std::make_unique<CGLESShader>("gles_shader_clip.vert", "gles_shader_fonts.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_FONTS_SHADER_CLIP)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_FONTS_SHADER_CLIP)->Free();
    shaderSlot(ShaderMethodGLES::SM_FONTS_SHADER_CLIP).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_clip.vert + gles_shader_fonts.frag - compile "
                        "and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND) =
      std::make_unique<CGLESShader>("gles_shader_texture_noblend.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture_noblend.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA) =
      std::make_unique<CGLESShader>("gles_shader_texture_noblend.frag", definesPma);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA).reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_texture_noblend.frag (premultiplied alpha) - compile and link failed");
  }

  // Same shader, but compiled for HDR-authored PQ overlays targeting PQ GUI output.
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT) =
      std::make_unique<CGLESShader>("gles_shader_texture_noblend.frag", definesHdrPgsPqOutput);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT).reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_texture_noblend.frag (HDR PGS PQ output) - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT) =
      std::make_unique<CGLESShader>("gles_shader_texture_noblend.frag", definesHdrPgsSdrOutput);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT).reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_texture_noblend.frag (HDR PGS SDR output) - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA_SDR_IMAGE_SUBS) =
      std::make_unique<CGLESShader>("gles_shader_texture_noblend.frag", definesSdrImageSubs);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA_SDR_IMAGE_SUBS)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA_SDR_IMAGE_SUBS)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA_SDR_IMAGE_SUBS).reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_texture_noblend.frag (SDR-authored image subs) - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_MULTI_BLENDCOLOR) =
      std::make_unique<CGLESShader>("gles_shader_multi_blendcolor.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_MULTI_BLENDCOLOR)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_MULTI_BLENDCOLOR)->Free();
    shaderSlot(ShaderMethodGLES::SM_MULTI_BLENDCOLOR).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_multi_blendcolor.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR) =
      std::make_unique<CGLESShader>("gles_shader_multi_rgba_111r_blendcolor.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR)->Free();
    shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR).reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_multi_rgba_111r_blendcolor.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR) =
      std::make_unique<CGLESShader>("gles_shader_multi_111r_111r_blendcolor.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR)->Free();
    shaderSlot(ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR).reset();
    CLog::Log(LOGERROR,
              "GUI Shader gles_shader_multi_111r_111r_blendcolor.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA) =
      std::make_unique<CGLESShader>("gles_shader_rgba.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR) =
      std::make_unique<CGLESShader>("gles_shader_rgba_blendcolor.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_blendcolor.frag - compile and link failed");
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB) =
      std::make_unique<CGLESShader>("gles_shader_rgba_bob.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_bob.frag - compile and link failed");
  }

  if (IsExtSupported("GL_OES_EGL_image_external"))
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_OES) =
        std::make_unique<CGLESShader>("gles_shader_rgba_oes.frag", defines);
    if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_OES)->CompileAndLink())
    {
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_OES)->Free();
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_OES).reset();
      CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_oes.frag - compile and link failed");
    }


    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES) =
        std::make_unique<CGLESShader>("gles_shader_rgba_bob_oes.frag", defines);
    if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES)->CompileAndLink())
    {
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES)->Free();
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES).reset();
      CLog::Log(LOGERROR, "GUI Shader gles_shader_rgba_bob_oes.frag - compile and link failed");
    }
  }

  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOALPHA) =
      std::make_unique<CGLESShader>("gles_shader_texture_noalpha.frag", defines);
  if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOALPHA)->CompileAndLink())
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOALPHA)->Free();
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOALPHA).reset();
    CLog::Log(LOGERROR, "GUI Shader gles_shader_texture_noalpha.frag - compile and link failed");
  }

  const auto advancedSettings = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
  if (advancedSettings &&
      (advancedSettings->m_videoAsyncFullscreenOSD || advancedSettings->m_guiSkinHdrFbo) &&
      !shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW))
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW) =
        std::make_unique<CGLESShader>("gles_shader_texture.frag", std::string());
    if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW)->CompileAndLink())
    {
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW)->Free();
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW).reset();
      logM(LOGERROR, "GUI Shader gles_shader_texture.frag (raw passthrough) - compile and link failed");
    }
  }

  m_srgbCompositeEnabled = advancedSettings && advancedSettings->m_guiSrgbHdrComposite;
  if (advancedSettings &&
      (advancedSettings->m_videoAsyncFullscreenOSD || advancedSettings->m_guiSkinHdrFbo) &&
      m_srgbCompositeEnabled && m_guiHdr != GuiHdr::SDR &&
      !shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT))
  {
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT) = std::make_unique<CGLESShader>(
        "gles_shader_texture.frag", defines + "#define KODI_COMPOSITE_CONVERT 1\n");
    if (!shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT)->CompileAndLink())
    {
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT)->Free();
      shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT).reset();
      logM(LOGERROR,
           "GUI Shader gles_shader_texture.frag (composite convert) - compile and link failed, "
           "falling back to per-primitive transfer");
    }
  }
}

void CRenderSystemGLES::WarmAllGuiHdrModeShaderCaches()
{
  if (!m_bRenderCreated)
    return;

  const auto warmStart = std::chrono::steady_clock::now();
  auto& gfxContext = CServiceBroker::GetWinSystem()->GetGfxContext();
  const GuiHdr originalMode = m_guiHdr;
  int warmedModes = 0;

  for (GuiHdr mode : {GuiHdr::SDR, GuiHdr::HDR, GuiHdr::HDR_PQ})
  {
    if (mode == originalMode)
      continue;

    gfxContext.SetGuiHdr(mode);
    ReleaseShaders();
    m_guiHdr = mode;
    InitialiseShaders();
    ++warmedModes;
  }

  gfxContext.SetGuiHdr(originalMode);
  ReleaseShaders();
  m_guiHdr = originalMode;
  InitialiseShaders();

  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - warmStart).count();
  logM(LOGINFO,
       "Warmed shader binary cache for {} other GuiHdr mode(s) in {} ms (boot one-time cost; runtime mode-change now hits binary cache)",
       warmedModes, elapsedMs);
}

void CRenderSystemGLES::ReleaseShaders()
{
  if (shaderSlot(ShaderMethodGLES::SM_DEFAULT))
    shaderSlot(ShaderMethodGLES::SM_DEFAULT)->Free();
  shaderSlot(ShaderMethodGLES::SM_DEFAULT).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_111R))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_111R)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_111R).reset();

  if (shaderSlot(ShaderMethodGLES::SM_MULTI))
    shaderSlot(ShaderMethodGLES::SM_MULTI)->Free();
  shaderSlot(ShaderMethodGLES::SM_MULTI).reset();

  if (shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R))
    shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R)->Free();
  shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R).reset();

  if (shaderSlot(ShaderMethodGLES::SM_FONTS))
    shaderSlot(ShaderMethodGLES::SM_FONTS)->Free();
  shaderSlot(ShaderMethodGLES::SM_FONTS).reset();

  if (shaderSlot(ShaderMethodGLES::SM_FONTS_SHADER_CLIP))
    shaderSlot(ShaderMethodGLES::SM_FONTS_SHADER_CLIP)->Free();
  shaderSlot(ShaderMethodGLES::SM_FONTS_SHADER_CLIP).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT).reset();

  if (shaderSlot(ShaderMethodGLES::SM_MULTI_BLENDCOLOR))
    shaderSlot(ShaderMethodGLES::SM_MULTI_BLENDCOLOR)->Free();
  shaderSlot(ShaderMethodGLES::SM_MULTI_BLENDCOLOR).reset();

  if (shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR))
    shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR)->Free();
  shaderSlot(ShaderMethodGLES::SM_MULTI_RGBA_111R_BLENDCOLOR).reset();

  if (shaderSlot(ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR))
    shaderSlot(ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR)->Free();
  shaderSlot(ShaderMethodGLES::SM_MULTI_111R_111R_BLENDCOLOR).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BLENDCOLOR).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_OES))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_OES)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_OES).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RGBA_BOB_OES).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOALPHA))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOALPHA)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_NOALPHA).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW).reset();

  if (shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT))
    shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT)->Free();
  shaderSlot(ShaderMethodGLES::SM_TEXTURE_RAW_CONVERT).reset();
}

void CRenderSystemGLES::EnableGUIShader(ShaderMethodGLES method)
{
  if (tlsWorkerShaderScope)
  {
    const unsigned int epoch = m_guiShaderEpoch.load(std::memory_order_relaxed);
    if (tlsWorkerShaderEpoch != epoch)
    {
      ReleaseShaders();
      InitialiseShaders();
      tlsWorkerShaderEpoch = epoch;
    }
  }
  tlsMethod = method;
  if (shader(tlsMethod))
  {
    shader(tlsMethod)->Enable();
    const GLint bypassLoc = shader(tlsMethod)->GetGuiTransferBypassLoc();
    if (bypassLoc >= 0)
      glUniform1f(bypassLoc, tlsGuiRenderTargetFillBypass ? 1.0f : 0.0f);
  }
  else
  {
    LOG_THROTTLE_PERIODIC_GENERAL(LOGERROR, 1000, "Invalid GUI Shader selected - {}", method);
  }
}

void CRenderSystemGLES::DisableGUIShader()
{
  if (shader(tlsMethod))
  {
    shader(tlsMethod)->Disable();
  }
  tlsMethod = ShaderMethodGLES::SM_DEFAULT;
}

GLint CRenderSystemGLES::GUIShaderGetPos()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetPosLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCol()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetColLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoord0()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetCord0Loc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoord1()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetCord1Loc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetDepth()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetDepthLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetUniCol()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetUniColLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoord0Matrix()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetCoord0MatrixLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetField()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetFieldLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetStep()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetStepLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetContrast()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetContrastLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetBrightness()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetBrightnessLoc();

  return -1;
}

bool CRenderSystemGLES::SupportsStereo(RENDER_STEREO_MODE mode) const
{
  return CRenderSystemBase::SupportsStereo(mode);
}

GLint CRenderSystemGLES::GUIShaderGetModel()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetModelLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetMatrix()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetMatrixLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetClip()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetShaderClipLoc();

  return -1;
}

GLint CRenderSystemGLES::GUIShaderGetCoordStep()
{
  if (shader(tlsMethod))
    return shader(tlsMethod)->GetShaderCoordStepLoc();

  return -1;
}

std::string CRenderSystemGLES::GetShaderPath(const std::string& filename)
{
  std::string path = "GLES/2.0/";

  if (m_RenderVersionMajor > 3 || (m_RenderVersionMajor == 3 && m_RenderVersionMinor >= 2))
  {
    std::string file = "special://xbmc/system/shaders/GLES/3.2/" + filename;
    const CURL pathToUrl(file);
    if (CFileUtils::Exists(pathToUrl.Get()))
      return "GLES/3.2/";
  }

  if (m_RenderVersionMajor > 3 || (m_RenderVersionMajor == 3 && m_RenderVersionMinor >= 1))
  {
    std::string file = "special://xbmc/system/shaders/GLES/3.1/" + filename;
    const CURL pathToUrl(file);
    if (CFileUtils::Exists(pathToUrl.Get()))
      return "GLES/3.1/";
  }

  return path;
}

namespace KODI::GLES
{
bool UsesFixedAttributeLocationsForShader(const std::string& vertexShaderName)
{
  const auto renderSystem = dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());
  if (!renderSystem || vertexShaderName.empty())
    return false;

  const std::string shaderPath = renderSystem->GetShaderPath(vertexShaderName);
  return shaderPath == "GLES/3.1/" || shaderPath == "GLES/3.2/";
}
}
