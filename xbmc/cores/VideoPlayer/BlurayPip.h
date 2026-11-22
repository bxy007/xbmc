/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "Interface/TimingConstants.h"
#include "threads/Thread.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class CDemuxStreamVideo;
class CDVDMsgDemuxerPacket;
class CDVDOverlayImage;
class CDVDStreamInfo;

struct BlurayPipPlacement
{
  double clipPts{0};
  unsigned int x{0};
  unsigned int y{0};
  float scale{1};
  bool fullscreen{false};
};

struct BlurayPipLayout
{
  int pid{-1};
  double inPts{0};
  double outPts{0};
  int lumaKey{-1};
  bool trickPlay{false};
  std::vector<BlurayPipPlacement> placements;
};

// Describes the bytes read from one PlayItem, including unsupported secondary
// streams that must stay out of the list of primary video tracks.
struct BlurayPipClip
{
  std::vector<int> pids;
  std::vector<std::shared_ptr<const BlurayPipLayout>> layouts;

  bool HasSecondaryVideo(int pid) const;
};

struct BlurayPipSelection
{
  unsigned int stream{0};
  bool enabled{false};
  bool fullscreen{false};
  bool trickPlay{false};
};

struct BlurayPipPicture
{
  std::shared_ptr<const CDVDOverlayImage> image;
  float pixelAspect{1};
  unsigned int x{0};
  unsigned int y{0};
  float scale{1};
  bool fullscreen{false};

  bool operator==(const BlurayPipPicture& other) const
  {
    return image == other.image && pixelAspect == other.pixelAspect && x == other.x &&
           y == other.y && scale == other.scale && fullscreen == other.fullscreen;
  }
};

// Software decoding for the disc's in-mux, synchronous secondary video. The compressed
// queue follows demux read-ahead, while a small decoded queue follows the presentation clock.
class CBlurayPip : private CThread
{
public:
  CBlurayPip();
  ~CBlurayPip() override;

  void Select(BlurayPipSelection selection);
  void Queue(std::shared_ptr<CDVDMsgDemuxerPacket> packet,
             const CDemuxStreamVideo& stream,
             double clipOffset);
  void Flush();
  bool HasStream() const { return m_hasStream.load(std::memory_order_relaxed); }
  BlurayPipPicture Present(double pts);

private:
  struct Packet
  {
    std::shared_ptr<CDVDMsgDemuxerPacket> data;
    std::shared_ptr<const CDVDStreamInfo> format;
    std::shared_ptr<const BlurayPipLayout> layout;
    double clipOffset{0};
    bool restartDecoder{false};
  };

  struct Frame
  {
    std::shared_ptr<const CDVDOverlayImage> image;
    std::shared_ptr<const BlurayPipLayout> layout;
    double clipOffset{0};
    double pts{DVD_NOPTS_VALUE};
    float pixelAspect{1};
  };

  void Process() override;
  void FlushLocked();
  Packet PopPacket();
  void SkipToKeyFrame();
  void WakeDecoder();

  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::atomic_bool m_hasStream{false};
  bool m_started{false};
  BlurayPipSelection m_selection;
  std::deque<Packet> m_packets;
  size_t m_packetBytes{0};
  std::shared_ptr<const CDVDStreamInfo> m_format;
  int64_t m_demuxerId{-1};
  int m_demuxChanges{-1};
  bool m_waitForKeyFrame{true};
  uint64_t m_epoch{0};
  std::deque<Frame> m_frames;
  Frame m_presented;
  double m_presentPts{DVD_NOPTS_VALUE};
};
