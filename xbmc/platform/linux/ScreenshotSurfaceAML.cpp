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

    // get current viewport
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    m_width = viewport[2] - viewport[0];
    m_height = viewport[3] - viewport[1];
    m_stride = m_width * 4;
    auto surface = new unsigned char[m_stride * m_height];

    // read pixels from the backbuffer
#if HAS_GLES >= 2
    glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGBA, GL_UNSIGNED_BYTE,
                 (GLvoid*)surface);
#else
    glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_BGRA, GL_UNSIGNED_BYTE,
                 (GLvoid*)surface);
#endif

    guiState.guiHdr = winSystem->GetGfxContext().GetGuiHdr();
    guiState.sdrPeakLuminance = winSystem->GetGuiSdrPeakLuminance();
    guiState.sdrSaturation = winSystem->GetGuiSdrSaturation();
    guiState.limitedColor = winSystem->UseLimitedColor();

    // make a new buffer and copy the read image to it with the Y axis inverted
    m_buffer = new unsigned char[m_stride * m_height];
    for (int y = 0; y < m_height; y++)
    {
#ifdef HAS_GLES
      // we need to save in BGRA order so swap RGBA -> BGRA
      unsigned char* swap_pixels = surface + (m_height - y - 1) * m_stride;
      for (int x = 0; x < m_width; x++, swap_pixels += 4)
      {
        std::swap(swap_pixels[0], swap_pixels[2]);
      }
#endif
      std::memcpy(m_buffer + y * m_stride, surface + (m_height - y - 1) * m_stride, m_stride);
    }

    delete[] surface;
  }

  // Composites the current visible hardware video plane under m_buffer and decodes HDR/DV/PQ GUI.
  CScreenshotAML::CompositeScreenshot(m_buffer, m_width, m_height, guiState, png);
  return true;
}
