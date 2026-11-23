/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  Copyright (C) 2020-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ScreenshotSurfaceAML.h"

#include "ServiceBroker.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "system_gl.h"
#include "threads/SingleLock.h"
#include "utils/Screenshot.h"
#include "utils/ScreenshotAML.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

void CScreenshotSurfaceAML::Register()
{
  CScreenShot::Register(CScreenshotSurfaceAML::CreateSurface);
}

std::unique_ptr<IScreenshotSurface> CScreenshotSurfaceAML::CreateSurface()
{
  return std::make_unique<CScreenshotSurfaceAML>();
}

bool CScreenshotSurfaceAML::Capture()
{
  return CaptureSurface(nullptr);
}

bool CScreenshotSurfaceAML::CapturePng()
{
  return CaptureSurface(&m_png);
}

bool CScreenshotSurfaceAML::CaptureSurface(ScreenshotPng* png)
{
  CGUIComponent* gui = CServiceBroker::GetGUI();
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  if (!gui || !winSystem)
    return false;

  if (!gui->GetWindowManager().BeginRenderExclusion())
    return false;

  AMLScreenshotGuiState guiState;
  {
    std::lock_guard lock(winSystem->GetGfxContext());

    gui->GetWindowManager().Render(true);

    gui->GetWindowManager().EndRenderExclusion();

#ifndef HAS_GLES
    glReadBuffer(GL_BACK);
#endif

    // GL_VIEWPORT returns x, y, width and height. In frame packing, y can
    // be greater than the height of one eye.
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);

    const int width = viewport[2];
    const int height = viewport[3];
    constexpr int bytesPerPixel = 4;
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / bytesPerPixel ||
        static_cast<size_t>(width) * bytesPerPixel >
            std::numeric_limits<size_t>::max() / static_cast<size_t>(height))
    {
      CLog::Log(LOGERROR, "CScreenshotSurfaceAML: invalid viewport {}x{}", width, height);
      return false;
    }

    const int stride = width * bytesPerPixel;
    const size_t size = static_cast<size_t>(stride) * height;
    const std::unique_ptr<unsigned char[]> surface(new (std::nothrow) unsigned char[size]);
    if (!surface)
      return false;
    std::unique_ptr<unsigned char[]> buffer(new (std::nothrow) unsigned char[size]);
    if (!buffer)
      return false;

    // read pixels from the backbuffer
#if HAS_GLES >= 2
    glReadPixels(viewport[0], viewport[1], width, height, GL_RGBA, GL_UNSIGNED_BYTE, surface.get());
#else
    glReadPixels(viewport[0], viewport[1], width, height, GL_BGRA, GL_UNSIGNED_BYTE, surface.get());
#endif

    guiState.guiHdr = winSystem->GetGfxContext().GetGuiHdr();
    guiState.sdrPeakLuminance = winSystem->GetGuiSdrPeakLuminance();
    guiState.sdrSaturation = winSystem->GetGuiSdrSaturation();
    guiState.limitedColor = winSystem->UseLimitedColor();

    // Copy the read image with the Y axis inverted.
    for (int y = 0; y < height; y++)
    {
      unsigned char* row = surface.get() + static_cast<size_t>(height - y - 1) * stride;
#ifdef HAS_GLES
      // we need to save in BGRA order so swap RGBA -> BGRA
      for (unsigned char* pixel = row; pixel < row + stride; pixel += bytesPerPixel)
      {
        std::swap(pixel[0], pixel[2]);
      }
#endif
      std::memcpy(buffer.get() + static_cast<size_t>(y) * stride, row, stride);
    }
    delete[] m_buffer;
    m_buffer = buffer.release();
    m_width = width;
    m_height = height;
    m_stride = stride;
  }

  // Composites the current visible hardware video plane under m_buffer and decodes HDR/DV/PQ GUI.
  CScreenshotAML::CompositeScreenshot(m_buffer, m_width, m_height, guiState, png);
  return true;
}
