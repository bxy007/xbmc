/*
 *  Copyright (C) 2015-2018 Team Kodi
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "windowing/GraphicContext.h"

struct ScreenshotPng;

struct AMLScreenshotGuiState
{
  GuiHdr guiHdr{GuiHdr::SDR};
  float sdrPeakLuminance{1.0f};
  float sdrSaturation{1.0f};
  bool limitedColor{false};
};

class CScreenshotAML
{
public:
  // Copies the current video frame as BGRA for the renderer's capture API.
  static bool CaptureVideoFrame(unsigned char* buffer, int iWidth, int iHeight);

  // Composites the captured hardware video plane (via /dev/vdin1 10-bit 4:2:2 Full-Pack)
  // under the premultiplied top-down BGRA GLES GUI buffer, decoding SDR / HDR10 / HLG /
  // Dolby Vision and inverting PQ / HLG / limited-range GUI coding cleanly to SDR BGRA.
  static void CompositeScreenshot(unsigned char* guiBuffer,
                                  int iWidth,
                                  int iHeight,
                                  const AMLScreenshotGuiState& guiState,
                                  ScreenshotPng* png = nullptr);
};
