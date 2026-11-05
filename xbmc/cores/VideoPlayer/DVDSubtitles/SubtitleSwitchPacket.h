/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

extern "C"
{
#include <libavcodec/packet.h>
}

// The replay cache stores subtitle payload/timing, not AVPacket side data.
// MPEG-TS attaches the one-byte PES stream_id even to ordinary Blu-ray PGS
// packets. It identifies the transport stream, not subtitle decoder state;
// replay already targets the selected demux stream. Other side data (including
// new extradata or encryption metadata) must not silently be discarded.
inline bool SubtitleSwitchCanOmitSideData(const AVPacketSideData* sideData, int count)
{
  if (count < 0 || (count > 0 && !sideData))
    return false;
  for (int i = 0; i < count; ++i)
  {
    if (sideData[i].type != AV_PKT_DATA_MPEGTS_STREAM_ID || sideData[i].size != 1 ||
        !sideData[i].data)
      return false;
  }
  return true;
}
