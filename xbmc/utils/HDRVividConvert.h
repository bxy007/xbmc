/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "HevcSei.h"

#include <cstdint>
#include <vector>

// T/UWA 005.1 MaxRGB values are already PQ codes (num/4095). This fills
// Dolby L1 from those codes and L6 / source PQ from static mastering metadata.
// Vivid tone-mapping curves are not written as Dolby trims.
std::vector<uint8_t> create_dovi_rpu_nalu_from_vivid(
    const HdrVividStats& stats,
    const HDRStaticMetadataInfo& hdrStaticMetadataInfo);
