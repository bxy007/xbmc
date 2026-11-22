/*
 *      Initial code sponsored by: Voddler Inc (voddler.com)
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "OverlayRenderer.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/BdjColorTransform.h"

#include "system_gl.h"

#include <cstdint>
#include <vector>

class CDVDOverlay;
class CDVDOverlayImage;
class CDVDOverlaySpu;
class CDVDOverlaySSA;

namespace OVERLAY
{

class COverlayTextureGLES : public COverlay
{
public:
  /*! \brief Create the overlay for rendering
     *  \param o The overlay image
     *  \param rSource The video source rect size
     */
  explicit COverlayTextureGLES(const CDVDOverlayImage& o, CRect& rSource);
  explicit COverlayTextureGLES(const CDVDOverlaySpu& o);
  ~COverlayTextureGLES() override;

  void Render(SRenderState& state) override;
  void UpdateFromImage(const CDVDOverlayImage& o) override;

  GLuint m_texture = 0;
  float m_u;
  float m_v;
  bool m_pma; /*< is alpha in texture premultiplied in the values */
  bool m_isHdrPqAuthored{false};

private:
  void UploadPipImage(const CDVDOverlayImage& o);

  bool m_isPipVideo{false};
  bool m_hasMipmaps{true};
  bool m_bgraSupported{false};
  bool m_bakedRaw{false};
  GLenum m_internalFormat{GL_RGBA};
  GLenum m_externalFormat{GL_RGBA};
  int m_texWidth{0};
  int m_texHeight{0};
  uint64_t m_uploadedRevision{0};
  BDJ_COLOR::BakeKey m_uploadedBakeKey{};
  std::vector<uint32_t> m_uploadScratch;
};

class COverlayGlyphGLES : public COverlay
{
public:
  COverlayGlyphGLES(ASS_Image* images, float width, float height);

  ~COverlayGlyphGLES() override;

  void Render(SRenderState& state) override;

  struct VERTEX
  {
    GLfloat u, v;
    GLubyte r, g, b, a;
    GLfloat x, y, z;
  };

private:
  struct Page
  {
    GLuint texture{0};
    std::vector<VERTEX> vertex;
  };

  std::vector<Page> m_pages;
};

} // namespace OVERLAY
