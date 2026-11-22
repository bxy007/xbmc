#pragma once

/*
*      Copyright (C) 2005-2013 Team XBMC
*      http://xbmc.org
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with XBMC; see the file COPYING.  If not, see
*  <http://www.gnu.org/licenses/>.
*
*/

#if defined(HAVE_LIBBLURAY)
#include "DVDInputStreams/DVDInputStreamBluray.h"
#endif
#include "DVDInputStreams/DVDInputStream.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

extern "C" {
#include "libavformat/avformat.h"
}

class CMVCSubtitleOffsets
{
public:
  int Get(double pts, int planeId) const;

private:
  friend class CDemuxStreamSSIF;

  struct Entry
  {
    double pts;
    std::vector<int8_t> planeOffsets;
  };

  mutable std::mutex m_mutex;
  std::vector<Entry> m_entries;
};

class CDemuxStreamSSIF
{
public:
  explicit CDemuxStreamSSIF(std::shared_ptr<CMVCSubtitleOffsets> subtitleOffsets = {})
    : m_subtitleOffsets(subtitleOffsets ? std::move(subtitleOffsets)
                                       : std::make_shared<CMVCSubtitleOffsets>())
  {
  }
  ~CDemuxStreamSSIF() { FreeQueuedPackets(); }

  DemuxPacket* AddPacket(DemuxPacket* &scrPkt);
  void Flush();
  void SetH264StreamId(int id) { m_h264StreamId = id; };
  void SetMVCStreamId(int id) { m_mvcStreamId = id; };
  int GetH264StreamId() const { return m_h264StreamId; };
  int GetMVCStreamId() const { return m_mvcStreamId; };
  void AddMVCExtPacket(DemuxPacket* &scrPkt);
  void SetBluRay(const std::shared_ptr<CDVDInputStream::IExtentionStream> &bluRay) { m_bluRay = bluRay; };
  bool IsBluRay() const { return m_bluRay != nullptr; };

private:
  void FreeQueuedPackets();
  void ParseOFMD(const uint8_t* data, int size, double pts);

  std::shared_ptr<CMVCSubtitleOffsets> m_subtitleOffsets;

  DemuxPacket* GetMVCPacket();
  DemuxPacket* MergePacket(DemuxPacket* &srcPkt, DemuxPacket* &appendPkt);
  bool FillMVCQueue(double dtsBase);
  void ResyncExtension(double dtsBase);

  std::shared_ptr<CDVDInputStream::IExtentionStream> m_bluRay = nullptr;
  std::queue<DemuxPacket*> m_H264queue;
  std::deque<DemuxPacket*> m_MVCqueue;
  int m_h264StreamId = -1;
  int m_mvcStreamId = -1;
  bool m_firstMatchLogged = false;
  bool m_wrapResyncPending = false;
  bool m_frontDtsValid = false;
  double m_lastFrontDts = 0.0;
  unsigned int m_baseDiscardsSinceMatch = 0;
  std::chrono::steady_clock::time_point m_lastExtRecovery{};
  std::chrono::steady_clock::time_point m_stitchAnchor{};
  std::chrono::steady_clock::time_point m_lastDiscardActivity{};
  std::chrono::steady_clock::time_point m_starveAnchor{};
  std::chrono::steady_clock::time_point m_lastStarveActivity{};
  bool m_stereoDeadSignaled = false;
};
