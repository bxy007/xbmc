/*
 *      Initial code sponsored by: Voddler Inc (voddler.com)
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OverlayRendererGLES.h"

#include "LinuxRendererGLES.h"
#include "OverlayRenderer.h"
#include "OverlayRendererUtil.h"
#include "RenderManager.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlaySSA.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlaySpu.h"
#include "rendering/GLExtensions.h"
#include "rendering/MatrixGL.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/GLUtils.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "utils/LogThrottle.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>

// GLES2.0 cant do CLAMP, but can do CLAMP_TO_EDGE.
#define GL_CLAMP GL_CLAMP_TO_EDGE

#define USE_PREMULTIPLIED_ALPHA 1

using namespace OVERLAY;

namespace
{
ShaderMethodGLES GetOverlayTextureShaderMethod(bool isHdrPqAuthored, bool pma)
{
  const bool pqOutput = CServiceBroker::GetWinSystem()->GetGfxContext().IsTransferPQ();

  ShaderMethodGLES method;
  if (!isHdrPqAuthored)
  {
    if (pma && pqOutput)
      method = ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA_SDR_IMAGE_SUBS;
    else
      method = pma ? ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA
                   : ShaderMethodGLES::SM_TEXTURE_NOBLEND;
  }
  else
  {
    method = pqOutput ? ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_PQ_OUTPUT
                      : ShaderMethodGLES::SM_TEXTURE_NOBLEND_HDR_PGS_SDR_OUTPUT;
  }

  LOG_THROTTLE_ONCHANGE(LOGDEBUG, LOGVIDEO,
                        (static_cast<int>(method) << 3) | (isHdrPqAuthored ? 4 : 0) |
                            (pma ? 2 : 0) | (pqOutput ? 1 : 0),
                        1000,
                        "overlay shader -> {} (isHdrPqAuthored={} pma={} pqOutput={})",
                        method, isHdrPqAuthored, pma, pqOutput);
  return method;
}

bool QueryBgraTextureFormat(GLenum& internalFormat, GLenum& externalFormat)
{
  internalFormat = GL_RGBA;
  externalFormat = GL_RGBA;
  auto* renderSystem = dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());
  if (!renderSystem)
    return false;
  if (renderSystem->IsExtSupported("GL_EXT_texture_format_BGRA8888") ||
      renderSystem->IsExtSupported("GL_IMG_texture_format_BGRA8888"))
  {
    internalFormat = externalFormat = GL_BGRA_EXT;
    return true;
  }
  if (renderSystem->IsExtSupported("GL_APPLE_texture_format_BGRA8888"))
  {
    externalFormat = GL_BGRA_EXT;
    return true;
  }
  return false;
}

inline uint32_t SwapRB(uint32_t px)
{
  return (px & 0xFF00FF00u) | ((px & 0x00FF0000u) >> 16) | ((px & 0x000000FFu) << 16);
}

BDJ_COLOR::BakeKey QueryCurrentOverlayBakeKey(bool isHdrPqAuthored)
{
  BDJ_COLOR::BakeKey key{};
  auto* winSystem = CServiceBroker::GetWinSystem();
  if (!winSystem)
    return key;

  auto& gfx = winSystem->GetGfxContext();
  const GuiHdr guiHdr = gfx.GetGuiHdr();
  const bool pqOutput = gfx.IsTransferPQ();
  key.limitedRange = winSystem->UseLimitedColor();
  key.srgbDecode = winSystem->GetGuiSrgbDecode() > 0.5f;

  const auto settingsComp = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComp ? settingsComp->GetSettings() : nullptr;

  if (isHdrPqAuthored)
  {
    if (pqOutput)
    {
      key.mode = BDJ_COLOR::BakeMode::HDR_PGS_PQ_OUT;
      key.peak = settings
                     ? static_cast<float>(std::clamp(
                           settings->GetInt(CSettings::SETTING_VIDEOSCREEN_HDRPGSPEAKLUMINANCE), 0,
                           100)) /
                           50.0f
                     : 1.0f;
      key.saturation =
          settings ? static_cast<float>(std::clamp(
                         settings->GetInt(CSettings::SETTING_VIDEOSCREEN_HDRPGSSATURATION), 0,
                         100)) /
                         50.0f
                   : 1.0f;
    }
    else
    {
      key.mode = BDJ_COLOR::BakeMode::HDR_PGS_SDR_OUT;
    }
  }
  else
  {
    if (pqOutput)
    {
      key.mode = BDJ_COLOR::BakeMode::SDR_SUBS_PQ_OUT;
      key.peak = settings
                     ? static_cast<float>(std::clamp(
                           settings->GetInt(CSettings::SETTING_VIDEOSCREEN_SDRPGSPEAKLUMINANCE), 0,
                           100)) /
                           50.0f
                     : 1.0f;
      key.saturation =
          settings ? static_cast<float>(std::clamp(
                         settings->GetInt(CSettings::SETTING_VIDEOSCREEN_SDRPGSSATURATION), 0,
                         100)) /
                         50.0f
                   : 1.0f;
    }
    else if (guiHdr == GuiHdr::HDR)
    {
      key.mode = BDJ_COLOR::BakeMode::SDR_GUI_HDR_OUT;
      key.peak = winSystem->GetGuiSdrPeakLuminance();
      key.saturation = winSystem->GetGuiSdrSaturation();
    }
    else
    {
      key.mode = BDJ_COLOR::BakeMode::PASSTHROUGH_SDR;
    }
  }
  return key;
}
} // namespace

static void LoadTexture(GLenum target,
                        GLsizei width,
                        GLsizei height,
                        GLsizei stride,
                        GLfloat* u,
                        GLfloat* v,
                        bool alpha,
                        const GLvoid* pixels)
{
  int width2 = width;
  int height2 = height;
  char* pixelVector = nullptr;
  const GLvoid* pixelData = pixels;

  GLenum internalFormat = alpha ? GL_ALPHA : GL_RGBA;
  GLenum externalFormat = alpha ? GL_ALPHA : GL_RGBA;

  int bytesPerPixel = KODI::UTILS::GL::glFormatElementByteCount(externalFormat);

  bool bgraSupported = false;
  auto renderSystem =
      dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());

  if (!alpha)
  {
    if (renderSystem->IsExtSupported("GL_EXT_texture_format_BGRA8888") ||
        renderSystem->IsExtSupported("GL_IMG_texture_format_BGRA8888"))
    {
      bgraSupported = true;
      internalFormat = externalFormat = GL_BGRA_EXT;
    }
    else if (renderSystem->IsExtSupported("GL_APPLE_texture_format_BGRA8888"))
    {
      // Apple's implementation does not conform to spec. Instead, they require
      // differing format/internalformat, more like GL.
      bgraSupported = true;
      externalFormat = GL_BGRA_EXT;
    }
  }

  int bytesPerLine = bytesPerPixel * width;

  if (!alpha && !bgraSupported)
  {
    pixelVector = (char*)malloc(bytesPerLine * height);

    auto src = (const char*)pixels;
    char* dst = pixelVector;
    for (int y = 0; y < height; ++y)
    {
      src = (const char*)pixels + y * stride;
      dst = pixelVector + y * bytesPerLine;

      for (GLsizei i = 0; i < width; i++, src += 4, dst += 4)
      {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = src[3];
      }
    }

    pixelData = pixelVector;
    stride = width;
  }
  /** OpenGL ES does not support strided texture input. Make a copy without stride **/
  else if (stride != bytesPerLine)
  {
    pixelVector = (char*)malloc(bytesPerLine * height);

    auto src = (const char*)pixels;
    char* dst = pixelVector;
    for (int y = 0; y < height; ++y)
    {
      memcpy(dst, src, bytesPerLine);
      src += stride;
      dst += bytesPerLine;
    }

    pixelData = pixelVector;
    stride = bytesPerLine;
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  glTexImage2D(target, 0, internalFormat, width2, height2, 0, externalFormat, GL_UNSIGNED_BYTE,
  nullptr);

  glTexSubImage2D(target, 0, 0, 0, width, height, externalFormat, GL_UNSIGNED_BYTE, pixelData);

  if (height < height2)
    glTexSubImage2D(target, 0, 0, height, width, 1, externalFormat, GL_UNSIGNED_BYTE,
                    (const unsigned char*)pixelData + stride * (height - 1));

  if (width < width2)
    glTexSubImage2D(target, 0, width, 0, 1, height, externalFormat, GL_UNSIGNED_BYTE,
                    (const unsigned char*)pixelData + bytesPerPixel * (width - 1));

  free(pixelVector);

  *u = (GLfloat)width / width2;
  *v = (GLfloat)height / height2;
}

std::shared_ptr<COverlay> COverlay::Create(const CDVDOverlayImage& o, CRect& rSource)
{
  return std::make_shared<COverlayTextureGLES>(o, rSource);
}

COverlayTextureGLES::COverlayTextureGLES(const CDVDOverlayImage& o, CRect& rSource)
{
  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  if (o.m_isPipVideo)
  {
    m_isPipVideo = true;
    m_pma = true; // The secondary video pixels have either zero or full alpha.
    m_hasMipmaps = false;
    m_bgraSupported = QueryBgraTextureFormat(m_internalFormat, m_externalFormat);
    m_u = 1.0f;
    m_v = 1.0f;
    UploadPipImage(o);
  }
  else if (o.m_persistentSurface)
  {
    auto& surface = *o.m_persistentSurface;
    m_pma = true;
    m_hasMipmaps = false;
    m_bgraSupported = QueryBgraTextureFormat(m_internalFormat, m_externalFormat);

    const bool isHdrPq = surface.isHdrPq.load(std::memory_order_relaxed);
    const BDJ_COLOR::BakeKey desired = QueryCurrentOverlayBakeKey(isHdrPq);

    std::lock_guard<std::mutex> surfLock(surface.mutex);
    const int w = surface.width;
    const int h = surface.height;
    m_texWidth = w;
    m_texHeight = h;
    m_u = 1.0f;
    m_v = 1.0f;
    m_isHdrPqAuthored = isHdrPq;
    surface.desiredKey = desired;
    if (desired.mode != BDJ_COLOR::BakeMode::NONE && surface.bakedKey != desired &&
        surface.pixelsRaw.size() >= static_cast<size_t>(w) * static_cast<size_t>(h))
    {
      BDJ_COLOR::BakeFullSurface(surface.pixelsRaw, surface.pixelsPma, desired, &surface.bakePool);
      surface.bakedKey = desired;
    }
    m_bakedRaw = (desired.mode != BDJ_COLOR::BakeMode::NONE && surface.bakedKey == desired);
    m_uploadedBakeKey = surface.bakedKey;
    m_uploadedRevision = surface.revision.load(std::memory_order_acquire);
    surface.dirtyX0 = 0;
    surface.dirtyY0 = 0;
    surface.dirtyX1 = -1;
    surface.dirtyY1 = -1;

    if (w > 0 && h > 0 &&
        surface.pixelsPma.size() >= static_cast<size_t>(w) * static_cast<size_t>(h))
    {
      const uint32_t* uploadPtr = surface.pixelsPma.data();
      if (!m_bgraSupported)
      {
        const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (m_uploadScratch.size() < count)
          m_uploadScratch.resize(count);
        for (size_t i = 0; i < count; ++i)
          m_uploadScratch[i] = SwapRB(uploadPtr[i]);
        uploadPtr = m_uploadScratch.data();
      }
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, m_internalFormat, w, h, 0, m_externalFormat, GL_UNSIGNED_BYTE,
                   uploadPtr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  else
  {
    std::vector<uint32_t> rgbaCopy;
    const uint32_t* rgbaPtr = nullptr;
    if (o.palette.empty())
    {
      rgbaCopy.resize(o.width * o.height);
      m_pma = !!USE_PREMULTIPLIED_ALPHA;
      const uint32_t* src = reinterpret_cast<const uint32_t*>(o.pixels.data());
      const int srcStride = o.linesize / 4;
      if (m_pma)
      {
        for (int y = 0; y < o.height; ++y)
        {
          for (int x = 0; x < o.width; ++x)
          {
            const uint32_t px = src[y * srcStride + x];
            const uint32_t a = (px >> 24) & 0xff;
            if (a == 255)
              rgbaCopy[y * o.width + x] = px;
            else if (a == 0)
              rgbaCopy[y * o.width + x] = 0;
            else
            {
              const uint32_t r = ((px >> 16) & 0xff) * a / 255;
              const uint32_t g = ((px >> 8) & 0xff) * a / 255;
              const uint32_t b = ((px >> 0) & 0xff) * a / 255;
              rgbaCopy[y * o.width + x] =
                  (a << 24) | (r << 16) | (g << 8) | (b << 0);
            }
          }
        }
      }
      else
      {
        for (int y = 0; y < o.height; ++y)
          std::memcpy(rgbaCopy.data() + y * o.width, src + y * srcStride, o.width * 4);
      }
      rgbaPtr = rgbaCopy.data();
      LoadTexture(GL_TEXTURE_2D, o.width, o.height, o.width * 4, &m_u, &m_v, false, rgbaPtr);
    }
    else
    {
      rgbaCopy.resize(o.width * o.height);
      m_pma = !!USE_PREMULTIPLIED_ALPHA;
      convert_rgba(o, m_pma, rgbaCopy);
      rgbaPtr = rgbaCopy.data();
      LoadTexture(GL_TEXTURE_2D, o.width, o.height, o.width * 4, &m_u, &m_v, false, rgbaPtr);
    }

    // If the overlay is already authored as HDR PQ code values (e.g. UHD-BD PGS HDR subtitles),
    // we can bypass the GUI shader's SDR->PQ transfer stage during composition.
    // The final bypass decision is made at render time based on current output state.
    m_isHdrPqAuthored = o.m_isHdrPq;
    m_hasMipmaps = true;

    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  m_isBitmapOverlay = true;

  if (o.source_width > 0 && o.source_height > 0)
  {
    m_pos = POSITION_RELATIVE;
    m_x = (0.5f * o.width + o.x) / o.source_width;
    m_y = (0.5f * o.height + o.y) / o.source_height;

    const float subRatio{static_cast<float>(o.source_width) / o.source_height};
    const float vidRatio{rSource.Width() / rSource.Height()};

    // We always consider aligning 4/3 subtitles to the video,
    // for example SD DVB subtitles (4/3) must be stretched on fullhd video

    if (std::fabs(subRatio - vidRatio) < 0.001f || IsSquareResolution(subRatio))
    {
      m_align = ALIGN_VIDEO;
      m_width = static_cast<float>(o.width) / o.source_width;
      m_height = static_cast<float>(o.height) / o.source_height;
    }
    else
    {
      // We should have a re-encoded/cropped (removed black bars) video source.
      // Then we cannot align to video otherwise the subtitles will be deformed
      // better align to screen by keeping the aspect-ratio.
      m_align = ALIGN_SCREEN_AR;
      m_width = static_cast<float>(o.width);
      m_height = static_cast<float>(o.height);
      m_source_width = static_cast<float>(o.source_width);
      m_source_height = static_cast<float>(o.source_height);
    }
  }
  else
  {
    m_align = ALIGN_VIDEO;
    m_pos = POSITION_ABSOLUTE;
    m_x = static_cast<float>(o.x);
    m_y = static_cast<float>(o.y);
    m_width = static_cast<float>(o.width);
    m_height = static_cast<float>(o.height);
  }
}

std::shared_ptr<COverlay> COverlay::Create(const CDVDOverlaySpu& o)
{
  return std::make_shared<COverlayTextureGLES>(o);
}

COverlayTextureGLES::COverlayTextureGLES(const CDVDOverlaySpu& o)
{
  int min_x, max_x, min_y, max_y;
  std::vector<uint32_t> rgba(o.width * o.height);

  convert_rgba(o, USE_PREMULTIPLIED_ALPHA, min_x, max_x, min_y, max_y, rgba);

  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  LoadTexture(GL_TEXTURE_2D, max_x - min_x, max_y - min_y, o.width * 4, &m_u, &m_v, false,
              rgba.data() + min_x + min_y * o.width);

  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);

  m_align = ALIGN_VIDEO;
  m_pos = POSITION_ABSOLUTE;
  m_x = static_cast<float>(min_x + o.x);
  m_y = static_cast<float>(min_y + o.y);
  m_width = static_cast<float>(max_x - min_x);
  m_height = static_cast<float>(max_y - min_y);
  m_pma = !!USE_PREMULTIPLIED_ALPHA;
  m_isBitmapOverlay = true;
}

std::shared_ptr<COverlay> COverlay::Create(ASS_Image* images, float width, float height)
{
  return std::make_shared<COverlayGlyphGLES>(images, width, height);
}

COverlayGlyphGLES::COverlayGlyphGLES(ASS_Image* images, float width, float height)
{
  m_width = 1.0;
  m_height = 1.0;
  m_align = ALIGN_SCREEN;
  m_pos = POSITION_RELATIVE;
  m_x = 0.0f;
  m_y = 0.0f;

  GLint maxTextureSize = 2048;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  if (maxTextureSize <= 0)
    maxTextureSize = 2048;

  std::vector<SQuads> pages;
  if (!convert_quads(images, pages, maxTextureSize))
    return;

  const float scale_x = 1.0f / width;
  const float scale_y = 1.0f / height;

  m_pages.reserve(pages.size());

  for (SQuads& quads : pages)
  {
    Page page;
    glGenTextures(1, &page.texture);
    glBindTexture(GL_TEXTURE_2D, page.texture);

    float u = 0.0f;
    float v = 0.0f;
    LoadTexture(GL_TEXTURE_2D, quads.size_x, quads.size_y, quads.size_x, &u, &v, true,
                quads.texture.data());

    const float scale_u = u / quads.size_x;
    const float scale_v = v / quads.size_y;

    page.vertex.resize(quads.quad.size() * 4);

    VERTEX* vt = page.vertex.data();
    SQuad* vs = quads.quad.data();

    for (size_t i = 0; i < quads.quad.size(); i++)
    {
      for (int s = 0; s < 4; s++)
      {
        vt[s].a = vs->a;
        vt[s].r = vs->r;
        vt[s].g = vs->g;
        vt[s].b = vs->b;

        vt[s].x = scale_x;
        vt[s].y = scale_y;
        vt[s].z = 0.0f;
        vt[s].u = scale_u;
        vt[s].v = scale_v;
      }

      vt[0].x *= vs->x;
      vt[0].u *= vs->u;
      vt[0].y *= vs->y;
      vt[0].v *= vs->v;

      vt[1].x *= vs->x;
      vt[1].u *= vs->u;
      vt[1].y *= vs->y + vs->h;
      vt[1].v *= vs->v + vs->h;

      vt[2].x *= vs->x + vs->w;
      vt[2].u *= vs->u + vs->w;
      vt[2].y *= vs->y;
      vt[2].v *= vs->v;

      vt[3].x *= vs->x + vs->w;
      vt[3].u *= vs->u + vs->w;
      vt[3].y *= vs->y + vs->h;
      vt[3].v *= vs->v + vs->h;

      vs += 1;
      vt += 4;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    m_pages.push_back(std::move(page));
  }
}

COverlayGlyphGLES::~COverlayGlyphGLES()
{
  for (Page& page : m_pages)
    glDeleteTextures(1, &page.texture);
}

void COverlayGlyphGLES::Render(SRenderState& state)
{
  if (m_pages.empty())
    return;

  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  glMatrixModview.Push();
  glMatrixModview->Translatef(state.x, state.y, 0.0f);
  glMatrixModview->Scalef(state.width, state.height, 1.0f);
  glMatrixModview.Load();

  auto renderSystem =
      dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());
  renderSystem->EnableGUIShader(ShaderMethodGLES::SM_FONTS);

  GLint posLoc = renderSystem->GUIShaderGetPos();
  GLint colLoc = renderSystem->GUIShaderGetCol();
  GLint tex0Loc = renderSystem->GUIShaderGetCoord0();
  GLint depthLoc = renderSystem->GUIShaderGetDepth();
  GLint matrixUniformLoc = renderSystem->GUIShaderGetMatrix();

  CMatrixGL matrix = glMatrixProject.Get();
  matrix.MultMatrixf(glMatrixModview.Get());
  glUniformMatrix4fv(matrixUniformLoc, 1, GL_FALSE, matrix);

  glUniform1f(depthLoc, -1.0f);

  for (const Page& page : m_pages)
  {
    if (page.texture == 0 || page.vertex.empty())
      continue;

    glBindTexture(GL_TEXTURE_2D, page.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    std::vector<VERTEX> vecVertices(6 * page.vertex.size() / 4);
    VERTEX* vertices = vecVertices.data();

    for (size_t i = 0; i < page.vertex.size(); i += 4)
    {
      *vertices++ = page.vertex[i];
      *vertices++ = page.vertex[i + 1];
      *vertices++ = page.vertex[i + 2];

      *vertices++ = page.vertex[i + 1];
      *vertices++ = page.vertex[i + 3];
      *vertices++ = page.vertex[i + 2];
    }

    vertices = vecVertices.data();

    glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(VERTEX),
                          (char*)vertices + offsetof(VERTEX, x));
    glVertexAttribPointer(colLoc, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VERTEX),
                          (char*)vertices + offsetof(VERTEX, r));
    glVertexAttribPointer(tex0Loc, 2, GL_FLOAT, GL_FALSE, sizeof(VERTEX),
                          (char*)vertices + offsetof(VERTEX, u));

    glEnableVertexAttribArray(posLoc);
    glEnableVertexAttribArray(colLoc);
    glEnableVertexAttribArray(tex0Loc);

    glDrawArrays(GL_TRIANGLES, 0, vecVertices.size());

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(colLoc);
    glDisableVertexAttribArray(tex0Loc);
  }

  renderSystem->DisableGUIShader();

  glMatrixModview.PopLoad();

  glDisable(GL_BLEND);

  glBindTexture(GL_TEXTURE_2D, 0);
}

COverlayTextureGLES::~COverlayTextureGLES()
{
  glDeleteTextures(1, &m_texture);
}

void COverlayTextureGLES::UploadPipImage(const CDVDOverlayImage& o)
{
  const size_t count = static_cast<size_t>(o.width) * o.height;
  const uint8_t* pixels = o.pixels.data();
  if (!m_bgraSupported)
  {
    m_uploadScratch.resize(count);
    uint8_t* rgba = reinterpret_cast<uint8_t*>(m_uploadScratch.data());
    for (size_t i = 0; i < count * 4; i += 4)
    {
      rgba[i] = pixels[i + 2];
      rgba[i + 1] = pixels[i + 1];
      rgba[i + 2] = pixels[i];
      rgba[i + 3] = pixels[i + 3];
    }
    pixels = rgba;
  }

  glBindTexture(GL_TEXTURE_2D, m_texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (o.width != m_texWidth || o.height != m_texHeight)
  {
    glTexImage2D(GL_TEXTURE_2D, 0, m_internalFormat, o.width, o.height, 0, m_externalFormat,
                 GL_UNSIGNED_BYTE, pixels);
    m_texWidth = o.width;
    m_texHeight = o.height;
  }
  else
  {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, o.width, o.height, m_externalFormat, GL_UNSIGNED_BYTE,
                    pixels);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void COverlayTextureGLES::UpdateFromImage(const CDVDOverlayImage& o)
{
  if (m_isPipVideo)
  {
    UploadPipImage(o);
    return;
  }
  if (!o.m_persistentSurface || m_texture == 0)
    return;

  auto& surface = *o.m_persistentSurface;
  const bool isHdrPq = surface.isHdrPq.load(std::memory_order_relaxed);
  m_isHdrPqAuthored = isHdrPq;
  const BDJ_COLOR::BakeKey desired = QueryCurrentOverlayBakeKey(isHdrPq);

  const uint64_t rev = surface.revision.load(std::memory_order_acquire);
  if (rev == m_uploadedRevision && desired == m_uploadedBakeKey)
    return;

  std::lock_guard<std::mutex> surfLock(surface.mutex);
  const int w = surface.width;
  const int h = surface.height;
  const uint64_t lockedRev = surface.revision.load(std::memory_order_acquire);
  if (w <= 0 || h <= 0 ||
      surface.pixelsPma.size() < static_cast<size_t>(w) * static_cast<size_t>(h))
  {
    m_uploadedRevision = lockedRev;
    return;
  }

  surface.desiredKey = desired;
  bool rebakedFull = false;
  if (desired.mode != BDJ_COLOR::BakeMode::NONE && surface.bakedKey != desired &&
      surface.pixelsRaw.size() >= static_cast<size_t>(w) * static_cast<size_t>(h))
  {
    BDJ_COLOR::BakeFullSurface(surface.pixelsRaw, surface.pixelsPma, desired, &surface.bakePool);
    surface.bakedKey = desired;
    rebakedFull = true;
  }
  m_bakedRaw = (desired.mode != BDJ_COLOR::BakeMode::NONE && surface.bakedKey == desired);
  m_uploadedBakeKey = surface.bakedKey;

  if (w != m_texWidth || h != m_texHeight || rebakedFull)
  {
    const uint32_t* uploadPtr = surface.pixelsPma.data();
    if (!m_bgraSupported)
    {
      const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
      if (m_uploadScratch.size() < count)
        m_uploadScratch.resize(count);
      for (size_t i = 0; i < count; ++i)
        m_uploadScratch[i] = SwapRB(uploadPtr[i]);
      uploadPtr = m_uploadScratch.data();
    }
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (w != m_texWidth || h != m_texHeight)
    {
      glTexImage2D(GL_TEXTURE_2D, 0, m_internalFormat, w, h, 0, m_externalFormat, GL_UNSIGNED_BYTE,
                   uploadPtr);
      m_texWidth = w;
      m_texHeight = h;
    }
    else
    {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, m_externalFormat, GL_UNSIGNED_BYTE, uploadPtr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    surface.dirtyX0 = 0;
    surface.dirtyY0 = 0;
    surface.dirtyX1 = -1;
    surface.dirtyY1 = -1;
    m_uploadedRevision = lockedRev;
    return;
  }

  int x0 = surface.dirtyX0;
  int y0 = surface.dirtyY0;
  int x1 = surface.dirtyX1;
  int y1 = surface.dirtyY1;
  surface.dirtyX0 = 0;
  surface.dirtyY0 = 0;
  surface.dirtyX1 = -1;
  surface.dirtyY1 = -1;
  m_uploadedRevision = lockedRev;

  if (x1 < x0 || y1 < y0)
    return;

  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(w - 1, x1);
  y1 = std::min(h - 1, y1);
  if (x1 < x0 || y1 < y0)
    return;

  const int subW = x1 - x0 + 1;
  const int subH = y1 - y0 + 1;
  const bool fullSpan = m_bgraSupported && subW >= (w / 2);

  glBindTexture(GL_TEXTURE_2D, m_texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  if (fullSpan)
  {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, subH, m_externalFormat, GL_UNSIGNED_BYTE,
                    surface.pixelsPma.data() + static_cast<size_t>(y0) * static_cast<size_t>(w));
  }
  else
  {
    const size_t needed = static_cast<size_t>(subW) * static_cast<size_t>(subH);
    if (m_uploadScratch.size() < needed)
      m_uploadScratch.resize(needed);
    for (int row = 0; row < subH; ++row)
    {
      const uint32_t* srcRow = surface.pixelsPma.data() +
                               static_cast<size_t>(y0 + row) * static_cast<size_t>(w) +
                               static_cast<size_t>(x0);
      uint32_t* dstRow =
          m_uploadScratch.data() + static_cast<size_t>(row) * static_cast<size_t>(subW);
      if (m_bgraSupported)
      {
        std::memcpy(dstRow, srcRow, static_cast<size_t>(subW) * sizeof(uint32_t));
      }
      else
      {
        for (int col = 0; col < subW; ++col)
          dstRow[col] = SwapRB(srcRow[col]);
      }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, subW, subH, m_externalFormat, GL_UNSIGNED_BYTE,
                    m_uploadScratch.data());
  }

  glBindTexture(GL_TEXTURE_2D, 0);

  if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
      CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
  {
    static uint32_t s_updates = 0;
    static uint32_t s_fullSpans = 0;
    static uint64_t s_pixels = 0;
    static auto s_window = std::chrono::steady_clock::now();
    ++s_updates;
    if (fullSpan)
      ++s_fullSpans;
    s_pixels += static_cast<uint64_t>(fullSpan ? w : subW) * static_cast<uint64_t>(subH);
    const auto now = std::chrono::steady_clock::now();
    if (now - s_window >= std::chrono::seconds(1))
    {
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "bdjTexSubImage: updates={} fullSpans={} avgKB={} last={}x{} bgra={}",
                    s_updates, s_fullSpans,
                    s_updates ? (s_pixels * 4u / 1024u / s_updates) : 0u, subW, subH,
                    m_bgraSupported);
      s_updates = 0;
      s_fullSpans = 0;
      s_pixels = 0;
      s_window = now;
    }
  }
}

void COverlayTextureGLES::Render(SRenderState& state)
{
  glEnable(GL_BLEND);

  glBindTexture(GL_TEXTURE_2D, m_texture);
  if (m_pma)
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  else
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  (m_isBitmapOverlay && m_hasMipmaps) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);

  CRect rd;
  if (m_pos == POSITION_RELATIVE)
  {
    float top = state.y - state.height * 0.5f;
    float bottom = state.y + state.height * 0.5f;
    float left = state.x - state.width * 0.5f;
    float right = state.x + state.width * 0.5f;

    rd.SetRect(left, top, right, bottom);
  }
  else
  {
    float top = state.y;
    float bottom = state.y + state.height;
    float left = state.x;
    float right = state.x + state.width;

    rd.SetRect(left, top, right, bottom);
  }

  auto renderSystem =
      dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());

  ShaderMethodGLES texMethod;
  if (m_isPipVideo)
    texMethod = ShaderMethodGLES::SM_TEXTURE_NOBLEND_PMA;
  else if (m_bakedRaw)
    texMethod = ShaderMethodGLES::SM_TEXTURE_NOBLEND_RAW;
  else
    texMethod = GetOverlayTextureShaderMethod(m_isHdrPqAuthored, m_pma);
  if (m_bakedRaw)
  {
    LOG_THROTTLE_ONCHANGE(LOGDEBUG, LOGVIDEO,
                          (static_cast<int>(m_uploadedBakeKey.mode) << 2) |
                              (m_isHdrPqAuthored ? 2 : 0) |
                              (m_uploadedBakeKey.limitedRange ? 1 : 0),
                          1000,
                          "overlay shader -> raw-baked (mode={} isHdrPqAuthored={} limited={})",
                          static_cast<int>(m_uploadedBakeKey.mode), m_isHdrPqAuthored,
                          m_uploadedBakeKey.limitedRange);
  }
  renderSystem->EnableGUIShader(texMethod);

  GLint posLoc = renderSystem->GUIShaderGetPos();
  GLint tex0Loc = renderSystem->GUIShaderGetCoord0();
  GLint depthLoc = renderSystem->GUIShaderGetDepth();

  GLfloat ver[4][2];
  GLfloat tex[4][2];
  GLubyte idx[4] = {0, 1, 3, 2}; //determines order of triangle strip

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, 0, 0, ver);
  glVertexAttribPointer(tex0Loc, 2, GL_FLOAT, 0, 0, tex);

  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(tex0Loc);

  glUniform1f(depthLoc, 1.0f);
  // Setup vertex position values
  ver[0][0] = ver[3][0] = rd.x1;
  ver[0][1] = ver[1][1] = rd.y1;
  ver[1][0] = ver[2][0] = rd.x2;
  ver[2][1] = ver[3][1] = rd.y2;

  // Setup texture coordinates
  tex[0][0] = tex[0][1] = tex[1][1] = tex[3][0] = 0.0f;
  tex[1][0] = tex[2][0] = m_u;
  tex[2][1] = tex[3][1] = m_v;

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(tex0Loc);

  renderSystem->DisableGUIShader();

  glDisable(GL_BLEND);

  glBindTexture(GL_TEXTURE_2D, 0);
}
