/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <vector>

struct ScreenshotPng
{
  enum class Transfer
  {
    PQ,
    HLG,
  };

  std::vector<uint16_t> sdr; // Top-down full-range RGBA, native byte order.
  std::vector<uint16_t> hdr; // Top-down full-range BT.2020 RGB, big endian.
  Transfer hdrTransfer{Transfer::PQ};
};

class IScreenshotSurface
{
public:
  IScreenshotSurface() = default;
  IScreenshotSurface(const IScreenshotSurface&) = delete;
  IScreenshotSurface& operator=(const IScreenshotSurface&) = delete;
  virtual ~IScreenshotSurface() { delete[] m_buffer; }
  virtual bool Capture() { return false; }
  virtual bool CapturePng() { return Capture(); }
  virtual void CaptureVideo(bool blendToBuffer) {}

  int GetWidth() const { return m_width; }
  int GetHeight() const { return m_height; }
  int GetStride() const { return m_stride; }
  unsigned char* GetBuffer() const { return m_buffer; }
  const ScreenshotPng& GetPng() const { return m_png; }

protected:
  int m_width{0};
  int m_height{0};
  int m_stride{0};
  unsigned char* m_buffer{nullptr};
  ScreenshotPng m_png;
};
