/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BdjColorTransform.h"
#include "DVDOverlay.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "PlatformDefs.h"

class CDVDOverlayImage : public CDVDOverlay
{
public:
  struct PersistentSurface
  {
    mutable std::mutex mutex;
    int width{0};
    int height{0};
    // Raw straight-alpha ARGB pixels from BD-J of size width * height (for rebaking on mode/slider change).
    std::vector<uint32_t> pixelsRaw;
    // Output-ready color-converted & premultiplied ARGB (LE BGRA) pixels of size width * height.
    std::vector<uint32_t> pixelsPma;
    BDJ_COLOR::BakeKey desiredKey{};
    BDJ_COLOR::BakeKey bakedKey{};
    BDJ_COLOR::BakePool bakePool;
    std::atomic<uint64_t> revision{0};
    std::atomic<bool> isHdrPq{false};
    // Inclusive dirty bounding box in pixel coordinates; dirtyX1 < dirtyX0 means clean.
    int dirtyX0{0};
    int dirtyY0{0};
    int dirtyX1{-1};
    int dirtyY1{-1};
  };

  CDVDOverlayImage() : CDVDOverlay(DVDOVERLAY_TYPE_IMAGE)
  {
  }

  CDVDOverlayImage(const CDVDOverlayImage& src, int sub_x, int sub_y, int sub_w, int sub_h)
  : CDVDOverlay(src)
  {
    int bpp;
    if (!src.palette.empty())
    {
      bpp = 1;
      palette = src.palette;
    }
    else
    {
      bpp = 4;
      palette.clear();
    }

    linesize = sub_w * bpp;
    x = sub_x;
    y = sub_y;
    width = sub_w;
    height = sub_h;
    source_width = src.source_width;
    source_height = src.source_height;
    m_isHdrPq = src.m_isHdrPq;

    pixels.resize(sub_h * linesize);

    uint8_t* s = src.data_at(sub_x, sub_y);
    uint8_t* t = pixels.data();

    for (int row = 0; row < sub_h; ++row)
    {
      memcpy(t, s, linesize);
      s += src.linesize;
      t += linesize;
    }

    m_textureid = 0;
  }

  ~CDVDOverlayImage() override = default;

  std::shared_ptr<CDVDOverlay> Clone() override
  {
    return std::make_shared<CDVDOverlayImage>(*this);
  }

  uint8_t* data_at(int sub_x, int sub_y) const
  {
    const int bpp = palette.empty() ? 4 : 1;
    return const_cast<uint8_t*>(pixels.data() + ((sub_y - y) * linesize + (sub_x - x) * bpp));
  }

  std::vector<uint8_t> pixels;
  std::vector<uint32_t> palette;
  std::shared_ptr<PersistentSurface> m_persistentSurface;

  // If true, pixel values are already intended for HDR PQ output (e.g. UHD-BD PGS
  // palettes authored as BT.2020 + ST2084 code values) and must NOT be converted
  // again by GUI shaders.
  bool m_isHdrPq{false};

  int linesize{0};
  int x{0};
  int y{0};
  int width{0};
  int height{0};
  int source_width{0};
  int source_height{0};
};
