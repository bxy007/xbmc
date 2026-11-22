/*
 *  Copyright (C) 2026 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "BlurayPip.h"

#include "DVDCodecs/Overlay/DVDOverlayImage.h"
#include "DVDDemuxers/DVDDemux.h"
#include "DVDMessage.h"
#include "DVDStreamInfo.h"
#include "Interface/DemuxPacket.h"
#include "utils/LogThrottle.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace
{
constexpr size_t MAX_PACKET_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_PACKETS = 4096;
constexpr size_t MAX_FRAMES = 6;
constexpr double MAX_FRAME_AGE = DVD_MSEC_TO_TIME(250);

struct PipFrameTiming
{
  std::shared_ptr<const BlurayPipLayout> layout;
  double clipOffset;
};

class PipDecoder
{
public:
  PipDecoder() = default;
  ~PipDecoder()
  {
    avcodec_free_context(&m_context);
    avfilter_graph_free(&m_graph);
    av_frame_free(&m_decoded);
    av_frame_free(&m_filtered);
    av_frame_free(&m_rgb);
    av_buffer_unref(&m_timing);
    sws_freeContext(m_scaler);
  }

  bool Open(const CDVDStreamInfo& format)
  {
    if (format.codec != AV_CODEC_ID_H264 && format.codec != AV_CODEC_ID_MPEG2VIDEO &&
        format.codec != AV_CODEC_ID_VC1)
      return false;

    const AVCodec* codec = avcodec_find_decoder(format.codec);
    if (!codec || !(m_context = avcodec_alloc_context3(codec)))
      return false;

    m_context->pkt_timebase = AV_TIME_BASE_Q;
    m_context->width = format.width;
    m_context->height = format.height;
    m_context->max_pixels = 2048 * 1088;
    m_context->thread_count = 2;
    m_context->thread_type = FF_THREAD_SLICE;
    m_context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    m_context->colorspace = format.colorSpace;
    m_context->color_range = format.colorRange;
    m_context->extradata_size = format.extradata.GetSize();
    if (m_context->extradata_size > 0)
    {
      m_context->extradata = static_cast<uint8_t*>(
          av_mallocz(m_context->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
      if (!m_context->extradata)
        return false;
      std::memcpy(m_context->extradata, format.extradata.GetData(), m_context->extradata_size);
    }

    m_decoded = av_frame_alloc();
    m_filtered = av_frame_alloc();
    m_rgb = av_frame_alloc();
    m_aspect = format.aspect;
    return m_decoded && m_filtered && m_rgb && avcodec_open2(m_context, codec, nullptr) == 0;
  }

  template<typename Output>
  void Decode(const DemuxPacket& packet,
              const std::shared_ptr<const BlurayPipLayout>& layout,
              double clipOffset,
              const Output& output)
  {
    const auto* current = m_timing ? reinterpret_cast<const PipFrameTiming*>(m_timing->data)
                                   : nullptr;
    if (!current || current->layout != layout || current->clipOffset != clipOffset)
    {
      auto timing = std::make_unique<PipFrameTiming>(PipFrameTiming{layout, clipOffset});
      AVBufferRef* ref = av_buffer_create(
          reinterpret_cast<uint8_t*>(timing.get()), sizeof(*timing),
          [](void*, uint8_t* data) { delete reinterpret_cast<PipFrameTiming*>(data); }, nullptr,
          AV_BUFFER_FLAG_READONLY);
      if (!ref)
        return;
      timing.release();
      av_buffer_unref(&m_timing);
      m_timing = ref;
    }

    AVPacket input{};
    input.data = packet.pData;
    input.size = packet.iSize;
    input.pts = ToAvPts(packet.pts);
    input.dts = ToAvPts(packet.dts);
    input.duration = std::max<int64_t>(0, ToAvPts(packet.duration));
    input.opaque_ref = m_timing;
    input.side_data = static_cast<AVPacketSideData*>(packet.pSideData);
    input.side_data_elems = packet.iSideDataElems;
    const int sent = avcodec_send_packet(m_context, &input);
    if (sent < 0)
    {
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGBLURAY, 1000, "Blu-ray PiP: send packet error {}", sent);
      return;
    }
    Receive(output);
  }

  template<typename Output>
  void Drain(const Output& output)
  {
    if (avcodec_send_packet(m_context, nullptr) >= 0)
      Receive(output);
    if (m_sink && av_buffersrc_add_frame_flags(m_source, nullptr, 0) >= 0)
      ReceiveFiltered(output);
  }

  float PixelAspect(const AVFrame& frame) const
  {
    if (frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den > 0)
      return static_cast<float>(av_q2d(frame.sample_aspect_ratio));
    return m_aspect > 0.0 ? static_cast<float>(m_aspect * frame.height / frame.width) : 1.0f;
  }

  std::shared_ptr<const CDVDOverlayImage> Convert(const AVFrame& frame, int lumaKey)
  {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    if (!desc || desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL) ||
        desc->comp[0].depth != 8 || desc->comp[0].plane != 0 || desc->comp[0].step != 1 ||
        frame.width <= 0 || frame.width > 1920 || frame.height <= 0 || frame.height > 1080)
      return nullptr;

    m_scaler = sws_getCachedContext(m_scaler, frame.width, frame.height, format,
                                   frame.width, frame.height, AV_PIX_FMT_BGRA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_scaler)
      return nullptr;

    const int colorspace = frame.colorspace == AVCOL_SPC_UNSPECIFIED
                               ? (frame.height <= 576 ? SWS_CS_ITU601 : SWS_CS_ITU709)
                               : static_cast<int>(frame.colorspace);
    const int* coeff = sws_getCoefficients(colorspace);
    sws_setColorspaceDetails(m_scaler, coeff, frame.color_range == AVCOL_RANGE_JPEG,
                             coeff, 1, 0, 1 << 16, 1 << 16);

    auto image = std::make_shared<CDVDOverlayImage>();
    image->m_isPipVideo = true;
    image->width = frame.width;
    image->height = frame.height;
    image->linesize = frame.width * 4;
    const size_t size = static_cast<size_t>(image->linesize) * frame.height;
    image->pixels.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    uint8_t* pixels = image->pixels.data();
    m_rgb->format = AV_PIX_FMT_BGRA;
    m_rgb->width = frame.width;
    m_rgb->height = frame.height;
    m_rgb->color_range = AVCOL_RANGE_JPEG;
    m_rgb->data[0] = pixels;
    m_rgb->linesize[0] = image->linesize;
    // The image owns the storage. A reference lets libswscale use it without a copy.
    m_rgb->buf[0] = av_buffer_create(pixels, image->pixels.size(), [](void*, uint8_t*) {}, nullptr, 0);
    if (!m_rgb->buf[0])
    {
      av_frame_unref(m_rgb);
      return nullptr;
    }
    const int scaled = sws_scale_frame(m_scaler, m_rgb, &frame);
    av_frame_unref(m_rgb);
    if (scaled < 0)
      return nullptr;

    if (lumaKey >= 0)
    {
      bool visible = false;
      for (int y = 0; y < frame.height; ++y)
      {
        const uint8_t* luma = frame.data[0] + y * frame.linesize[0];
        uint8_t* row = pixels + y * image->linesize;
        for (int x = 0; x < frame.width; ++x)
        {
          if (luma[x] <= lumaKey)
            std::memset(row + 4 * x, 0, 4);
          else
            visible = true;
        }
      }
      if (!visible)
        return nullptr;
    }
    return image;
  }

private:
  template<typename Output>
  void Receive(const Output& output)
  {
    while (avcodec_receive_frame(m_context, m_decoded) == 0)
    {
      if (m_decoded->flags & AV_FRAME_FLAG_CORRUPT ||
          m_decoded->best_effort_timestamp == AV_NOPTS_VALUE)
        continue;
      m_decoded->pts = m_decoded->best_effort_timestamp;

      if ((m_graph || m_decoded->flags & AV_FRAME_FLAG_INTERLACED) &&
          SetupFilter(*m_decoded))
      {
        const int filtered = av_buffersrc_add_frame_flags(m_source, m_decoded,
                                                          AV_BUFFERSRC_FLAG_KEEP_REF);
        if (filtered < 0)
        {
          LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGBLURAY, 1000, "Blu-ray PiP: filter error {}",
                                filtered);
          continue;
        }
        ReceiveFiltered(output);
      }
      else
        output(*m_decoded, m_decoded->pts);
    }
  }

  template<typename Output>
  void ReceiveFiltered(const Output& output)
  {
    const AVRational timeBase = av_buffersink_get_time_base(m_sink);
    while (av_buffersink_get_frame(m_sink, m_filtered) == 0)
    {
      if (m_filtered->pts != AV_NOPTS_VALUE)
        output(*m_filtered, av_rescale_q(m_filtered->pts, timeBase, AV_TIME_BASE_Q));
      av_frame_unref(m_filtered);
    }
  }

  static int64_t ToAvPts(double pts)
  {
    constexpr double limit = static_cast<double>(std::numeric_limits<int64_t>::max());
    if (!std::isfinite(pts) || pts <= -limit || pts >= limit)
      return AV_NOPTS_VALUE;
    return std::llround(pts);
  }

  bool SetupFilter(const AVFrame& frame)
  {
    if (m_graph && frame.width == m_filterWidth && frame.height == m_filterHeight &&
        frame.format == m_filterFormat)
      return m_sink != nullptr;

    avfilter_graph_free(&m_graph);
    m_source = nullptr;
    m_sink = nullptr;
    m_filterWidth = frame.width;
    m_filterHeight = frame.height;
    m_filterFormat = frame.format;
    m_graph = avfilter_graph_alloc();
    if (!m_graph)
      return false;

    m_graph->nb_threads = 1;
    const AVRational sar = frame.sample_aspect_ratio.den > 0 ? frame.sample_aspect_ratio
                                                           : AVRational{0, 1};
    std::string args = StringUtils::Format(
        "video_size={}x{}:pix_fmt={}:time_base=1/{}:pixel_aspect={}/{}", frame.width,
        frame.height, frame.format, DVD_TIME_BASE, sar.num, sar.den);
#if LIBAVFILTER_VERSION_MAJOR >= 11
    args += StringUtils::Format(":colorspace={}:range={}", static_cast<int>(frame.colorspace),
                                static_cast<int>(frame.color_range));
#endif
    AVFilterContext* deinterlace = nullptr;
    if (avfilter_graph_create_filter(&m_source, avfilter_get_by_name("buffer"), "in", args.c_str(),
                                     nullptr, m_graph) < 0 ||
        avfilter_graph_create_filter(&deinterlace, avfilter_get_by_name("bwdif"), "deinterlace",
                                     "mode=send_frame:parity=auto:deint=interlaced", nullptr,
                                     m_graph) < 0 ||
        avfilter_graph_create_filter(&m_sink, avfilter_get_by_name("buffersink"), "out",
                                     nullptr, nullptr, m_graph) < 0 ||
        avfilter_link(m_source, 0, deinterlace, 0) < 0 ||
        avfilter_link(deinterlace, 0, m_sink, 0) < 0 ||
        avfilter_graph_config(m_graph, nullptr) < 0)
    {
      m_sink = nullptr;
      return false;
    }
    return true;
  }

  AVCodecContext* m_context{nullptr};
  AVFrame* m_decoded{nullptr};
  AVFilterGraph* m_graph{nullptr};
  AVFilterContext* m_source{nullptr};
  AVFilterContext* m_sink{nullptr};
  AVFrame* m_filtered{nullptr};
  AVFrame* m_rgb{nullptr};
  AVBufferRef* m_timing{nullptr};
  int m_filterWidth{0};
  int m_filterHeight{0};
  int m_filterFormat{-1};
  SwsContext* m_scaler{nullptr};
  double m_aspect{0};
};
} // namespace

bool BlurayPipClip::HasSecondaryVideo(int pid) const
{
  return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

CBlurayPip::CBlurayPip() : CThread("BlurayPip")
{
}

CBlurayPip::~CBlurayPip()
{
  if (!m_started)
    return;
  {
    std::unique_lock lock(m_mutex);
    m_bStop = true;
  }
  m_ready.notify_one();
  StopThread();
}

void CBlurayPip::Select(BlurayPipSelection selection)
{
  std::unique_lock lock(m_mutex);
  const bool streamChanged = m_selection.stream != selection.stream;
  const bool visibilityChanged = m_selection.enabled != selection.enabled;
  if (!streamChanged && !visibilityChanged && m_selection.fullscreen == selection.fullscreen &&
      m_selection.trickPlay == selection.trickPlay)
    return;

  m_selection = selection;
  if (streamChanged)
    FlushLocked();
  else if (visibilityChanged)
  {
    if (!selection.enabled)
    {
      m_frames.clear();
      m_presented = {};
      ++m_epoch;
    }
    else if (m_presentPts != DVD_NOPTS_VALUE)
    {
      // The disc can enable PiP after this part of the clip was read. Use the
      // latest buffered random-access packet at or before the displayed picture.
      const auto first = std::find_if(m_packets.rbegin(), m_packets.rend(), [&](const auto& item) {
        const DemuxPacket& packet = *item.data->GetPacket();
        return packet.m_keyFrame && packet.pts != DVD_NOPTS_VALUE && packet.pts <= m_presentPts;
      });
      if (first != m_packets.rend())
      {
        auto skip = std::distance(first, m_packets.rend()) - 1;
        for (; skip > 0; --skip)
          PopPacket();
      }
    }
    SkipToKeyFrame();
  }

  if (streamChanged || visibilityChanged)
    logComponentM(LOGDEBUG, LOGBLURAY, "Blu-ray PiP: secondary stream {} {} (fullscreen {})",
                  selection.stream, selection.enabled ? "enabled" : "disabled",
                  selection.fullscreen);
  WakeDecoder();
  if (streamChanged || (visibilityChanged && !selection.enabled))
    m_ready.notify_one();
}

void CBlurayPip::FlushLocked()
{
  m_packets.clear();
  m_packetBytes = 0;
  m_format.reset();
  m_demuxerId = -1;
  m_hasStream.store(false, std::memory_order_relaxed);
  m_waitForKeyFrame = true;
  m_frames.clear();
  m_presented = {};
  m_presentPts = DVD_NOPTS_VALUE;
  ++m_epoch;
}

void CBlurayPip::Flush()
{
  std::unique_lock lock(m_mutex);
  FlushLocked();
  m_ready.notify_one();
}

CBlurayPip::Packet CBlurayPip::PopPacket()
{
  auto packet = std::move(m_packets.front());
  m_packetBytes -= packet.data->GetPacketSize();
  m_packets.pop_front();
  return packet;
}

void CBlurayPip::SkipToKeyFrame()
{
  while (!m_packets.empty() && !m_packets.front().data->GetPacket()->m_keyFrame)
    PopPacket();

  if (m_packets.empty())
    m_waitForKeyFrame = true;
  else
    m_packets.front().restartDecoder = true;
}

void CBlurayPip::WakeDecoder()
{
  if (!m_selection.enabled || m_packets.empty() || m_frames.size() >= MAX_FRAMES)
    return;
  if (!m_started)
  {
    Create();
    m_started = true;
  }
  m_ready.notify_one();
}

void CBlurayPip::Queue(std::shared_ptr<CDVDMsgDemuxerPacket> packet,
                       const CDemuxStreamVideo& stream,
                       double clipOffset)
{
  std::unique_lock lock(m_mutex);
  const auto& input = *packet->GetPacket();
  const auto& clip = input.m_blurayPipClip;
  if (!clip || m_selection.stream == 0 || m_selection.stream > clip->layouts.size())
    return;

  const auto& layout = clip->layouts[m_selection.stream - 1];
  if (!layout || layout->pid != stream.dvdNavId || layout->lumaKey >= 255)
    return;
  m_hasStream.store(true, std::memory_order_relaxed);
  const size_t size = packet->GetPacketSize();
  if (size == 0)
    return;
  if (size > MAX_PACKET_BYTES)
  {
    m_waitForKeyFrame = true;
    return;
  }
  if (m_waitForKeyFrame && !input.m_keyFrame)
    return;

  if (!m_format || m_demuxerId != stream.demuxerId || m_format->uniqueId != stream.uniqueId ||
      m_demuxChanges != stream.changes || m_format->codec != stream.codec ||
      m_format->width != stream.iWidth || m_format->height != stream.iHeight ||
      m_format->aspect != stream.fAspect || m_format->colorSpace != stream.colorSpace ||
      m_format->colorRange != stream.colorRange || !(m_format->extradata == stream.extraData))
  {
    m_format = std::make_shared<CDVDStreamInfo>(stream, true);
    m_demuxerId = stream.demuxerId;
    m_demuxChanges = stream.changes;
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "Blu-ray PiP: PID 0x{:04x} queue start codec {} size {}x{} PTS {:.3f} offset {:.3f}",
                  stream.dvdNavId, static_cast<int>(m_format->codec), m_format->width,
                  m_format->height, input.pts / DVD_TIME_BASE, clipOffset / DVD_TIME_BASE);
  }

  m_packetBytes += size;
  m_packets.push_back(
      {std::move(packet), m_format, layout, clipOffset, std::exchange(m_waitForKeyFrame, false)});
  while (m_packets.size() > MAX_PACKETS || m_packetBytes > MAX_PACKET_BYTES)
  {
    // Keep already decoded pictures. Carry the reset on the first retained
    // keyframe so a full decoded queue neither wakes nor reopens the decoder.
    PopPacket();
    SkipToKeyFrame();
  }

  WakeDecoder();
}

BlurayPipPicture CBlurayPip::Present(double pts)
{
  std::unique_lock lock(m_mutex);
  if (m_selection.stream == 0 || pts == DVD_NOPTS_VALUE)
    return {};

  m_presentPts = pts;
  if (!m_selection.enabled)
    return {};

  // The MPEG clock has 90 kHz ticks, while Kodi's clock is in microseconds.
  if (!m_frames.empty() && m_frames.front().pts <= pts + DVD_MSEC_TO_TIME(1))
  {
    do
    {
      m_presented = std::move(m_frames.front());
      m_frames.pop_front();
    } while (!m_frames.empty() && m_frames.front().pts <= pts + DVD_MSEC_TO_TIME(1));
    WakeDecoder();
  }

  LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGBLURAY, 1000,
                        "Blu-ray PiP: present {:.3f} decoded {:.3f} image {} next {:.3f} packets {}",
                        pts / DVD_TIME_BASE,
                        m_presented.pts == DVD_NOPTS_VALUE ? -1.0 : m_presented.pts / DVD_TIME_BASE,
                        !!m_presented.image,
                        m_frames.empty() ? -1.0 : m_frames.front().pts / DVD_TIME_BASE,
                        m_packets.size());

  if (!m_presented.layout)
    return {};

  const auto& layout = *m_presented.layout;
  const double clipPts = pts - m_presented.clipOffset + DVD_MSEC_TO_TIME(1);
  if (pts - m_presented.pts > MAX_FRAME_AGE || clipPts >= layout.outPts)
  {
    m_presented = {};
    return {};
  }
  if (!m_presented.image || m_presented.pts > pts + DVD_MSEC_TO_TIME(1) || clipPts < layout.inPts ||
      (m_selection.trickPlay && !layout.trickPlay))
    return {};

  auto next = std::upper_bound(layout.placements.begin(), layout.placements.end(), clipPts,
                               [](double time, const BlurayPipPlacement& placement)
                               { return time < placement.clipPts; });
  if (next == layout.placements.begin())
    return {};

  const auto& placement = *std::prev(next);
  return {m_presented.image, m_presented.pixelAspect, placement.x, placement.y, placement.scale,
          placement.fullscreen || m_selection.fullscreen};
}

void CBlurayPip::Process()
{
  std::unique_ptr<PipDecoder> decoder;
  std::shared_ptr<const CDVDStreamInfo> format;
  uint64_t epoch = 0;
  bool needKeyFrame = true;
  bool loggedFrame = false;

  while (true)
  {
    std::unique_lock lock(m_mutex);
    m_ready.wait(lock, [&] {
      return m_bStop || epoch != m_epoch ||
             (m_selection.enabled && !m_packets.empty() && m_frames.size() < MAX_FRAMES);
    });
    if (m_bStop)
      break;

    if (epoch != m_epoch)
    {
      epoch = m_epoch;
      lock.unlock();
      decoder.reset();
      format.reset();
      needKeyFrame = true;
      loggedFrame = false;
      continue;
    }

    auto packet = PopPacket();
    lock.unlock();

    const auto output = [&](const AVFrame& frame, double pts) {
      if (!frame.opaque_ref)
        return;
      // The codec and the deinterlacer can both return pictures of earlier
      // packets after a PlayItem change. Follow the frame's own timestamps.
      const auto& timing = *reinterpret_cast<const PipFrameTiming*>(frame.opaque_ref->data);
      if (pts - timing.clipOffset >= timing.layout->outPts)
        return;

      {
        std::unique_lock outputLock(m_mutex);
        if (!loggedFrame)
        {
          logComponentM(LOGDEBUG, LOGBLURAY, "Blu-ray PiP: first decoded PTS {:.3f}, screen {:.3f}",
                        pts / DVD_TIME_BASE,
                        m_presentPts == DVD_NOPTS_VALUE ? -1.0 : m_presentPts / DVD_TIME_BASE);
          loggedFrame = true;
        }
        if (m_bStop || m_epoch != epoch ||
            (m_presentPts != DVD_NOPTS_VALUE && pts < m_presentPts - MAX_FRAME_AGE) ||
            (!m_frames.empty() && pts <= m_frames.back().pts))
          return;
      }
      auto image = decoder->Convert(frame, timing.layout->lumaKey);
      const float sampleAspect = decoder->PixelAspect(frame);

      std::unique_lock outputLock(m_mutex);
      if (!m_bStop && m_epoch == epoch)
        m_frames.push_back({std::move(image), timing.layout, timing.clipOffset, pts, sampleAspect});
    };

    if (format != packet.format || packet.restartDecoder)
    {
      if (decoder && !packet.restartDecoder)
        decoder->Drain(output);
      decoder.reset();
      format = packet.format;
      needKeyFrame = true;
      loggedFrame = false;
    }

    const auto& input = *packet.data->GetPacket();
    if (needKeyFrame && input.m_keyFrame)
    {
      needKeyFrame = false;
      decoder = std::make_unique<PipDecoder>();
      if (!decoder->Open(*format))
      {
        decoder.reset();
        CLog::Log(LOGWARNING, "Blu-ray picture-in-picture decoder unavailable");
      }
    }
    if (decoder)
      decoder->Decode(input, packet.layout, packet.clipOffset, output);
  }
}
