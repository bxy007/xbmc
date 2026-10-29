/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HDRVividConvert.h"

#include "DoViRpuWriter.h"
#include "HDR10PlusConvert.h"
#include "utils/LogThrottle.h"
#include "utils/log.h"

#include <cstdint>

namespace
{
constexpr uint16_t kPq4000Nits = 3696;
constexpr uint16_t kSourceMinPq0001Nits = 7;

uint16_t SourceMaxPq(uint32_t maxLum)
{
  switch (maxLum)
  {
    case 0:
      return kPq4000Nits; // T/UWA 005.1-2024 7.2.3 default master
    case 1000:
      return 3079;
    case 2000:
      return 3388;
    case 4000:
      return kPq4000Nits;
    case 10000:
      return 4095;
    default:
      return static_cast<uint16_t>(nits_to_max_pq(static_cast<int>(maxLum)));
  }
}

uint16_t SourceMinPq(uint32_t minLum)
{
  if (minLum == 0)
    return kSourceMinPq0001Nits;
  if (minLum < 2)
    return 7;
  if (minLum < 5)
    return 10;
  if (minLum < 10)
    return 17;
  if (minLum < 20)
    return 26;
  if (minLum < 50)
    return 38;
  return 62;
}

std::vector<uint8_t> lastRpu;
VdrDmData lastVdr{};
bool lastValid = false;
} // namespace

std::vector<uint8_t> create_dovi_rpu_nalu_from_vivid(
    const HdrVividStats& stats,
    const HDRStaticMetadataInfo& hdrStaticMetadataInfo)
{
  if (stats.minimum_maxrgb > stats.average_maxrgb ||
      stats.average_maxrgb > stats.maximum_maxrgb ||
      stats.maximum_maxrgb > 4095)
    return {};

  VdrDmData vdr{};
  vdr.source_min_pq = SourceMinPq(hdrStaticMetadataInfo.min_lum);
  vdr.source_max_pq = SourceMaxPq(hdrStaticMetadataInfo.max_lum);
  vdr.min_pq = stats.minimum_maxrgb;
  vdr.max_pq = stats.maximum_maxrgb;
  vdr.avg_pq = stats.average_maxrgb;
  vdr.max_display_mastering_luminance =
      hdrStaticMetadataInfo.max_lum ? static_cast<uint16_t>(hdrStaticMetadataInfo.max_lum)
                                    : 4000;
  vdr.min_display_mastering_luminance =
      hdrStaticMetadataInfo.min_lum ? static_cast<uint16_t>(hdrStaticMetadataInfo.min_lum) : 1;
  vdr.max_content_light_level = hdrStaticMetadataInfo.max_cll;
  vdr.max_frame_average_light_level = hdrStaticMetadataInfo.max_fall;

  if (!lastValid ||
      lastVdr.source_min_pq != vdr.source_min_pq ||
      lastVdr.source_max_pq != vdr.source_max_pq ||
      lastVdr.min_pq != vdr.min_pq ||
      lastVdr.max_pq != vdr.max_pq ||
      lastVdr.avg_pq != vdr.avg_pq ||
      lastVdr.max_display_mastering_luminance != vdr.max_display_mastering_luminance ||
      lastVdr.min_display_mastering_luminance != vdr.min_display_mastering_luminance ||
      lastVdr.max_content_light_level != vdr.max_content_light_level ||
      lastVdr.max_frame_average_light_level != vdr.max_frame_average_light_level)
  {
    lastRpu = create_dovi_rpu_nalu(vdr);
    lastVdr = vdr;
    lastValid = !lastRpu.empty();
    LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                          "HDR Vivid L1 min [{}] max [{}] avg [{}] source_max_pq [{}]",
                          vdr.min_pq, vdr.max_pq, vdr.avg_pq, vdr.source_max_pq);
  }

  return lastRpu;
}
