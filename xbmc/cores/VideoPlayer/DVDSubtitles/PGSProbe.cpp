/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Bitmap evidence adapted from mpv's LGPL-2.1-or-later subtitle identifier.
 * See PGS-OCR.md for source provenance and limits.
 */
#include "PGSProbe.h"

#include "PGSActivity.h"
#include "PGSOcr.h"
#include "PGSLayout.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#if defined(TARGET_LINUX)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
}

namespace
{
constexpr size_t MAX_TRACKS = 16;
constexpr size_t MAX_QUEUE = 1024;
constexpr size_t MAX_BYTES = 4 * 1024 * 1024;
constexpr int MAX_ATTEMPTS = 96;
constexpr int MAX_BILINGUAL_CAPTIONS = 24;
constexpr int MAX_DECODE_CAPTIONS = 96;
constexpr int MAX_DISPLAY_SETS = 4096;
constexpr size_t RECENT_BITMAPS = 128;
struct Bounds
{
  int x0{}, y0{}, x1{}, y1{}, pixels{};
};
struct Evidence
{
  pgs_sub_script_evidence script{};
  pgs_sub_bilingual_evidence bilingual{};
  std::array<uint64_t, RECENT_BITMAPS> recent{};
  unsigned numBitmaps{};
  int attempts{}, decoded{}, displaySets{};
  double lastPts{NAN}, lastOcrPts{NAN};
  uint64_t ocrMs{};
};
enum class CandidateLanguage
{
  Other,
  Chinese,
  Unknown,
};

Bounds BitmapBounds(const AVSubtitleRect& r)
{
  Bounds b{r.w, r.h};
  if (r.type != SUBTITLE_BITMAP || r.w <= 0 || r.h <= 0 || r.w > 4096 || r.h > 2160 || !r.data[0] ||
      !r.data[1] || r.linesize[0] < r.w || r.nb_colors < 1 || r.nb_colors > 256)
    return {};
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x)
    {
      const int index = r.data[0][y * r.linesize[0] + x];
      if (index >= r.nb_colors)
        continue;
      const uint32_t c = AV_RN32(r.data[1] + index * 4);
      const int hi = std::max({int((c >> 16) & 255), int((c >> 8) & 255), int(c & 255)});
      if (hi * (c >> 24) <= 6000)
        continue;
      b.x0 = std::min(b.x0, x);
      b.x1 = std::max(b.x1, x + 1);
      b.y0 = std::min(b.y0, y);
      b.y1 = std::max(b.y1, y + 1);
      ++b.pixels;
    }
  return b;
}
bool NewCaption(Evidence& e, const AVSubtitle& sub, const std::array<Bounds, 4>& bounds)
{
  uint64_t hash = 14695981039346656037ULL;
  bool haveInk = false;
  for (unsigned n = 0; n < sub.num_rects && n < bounds.size(); ++n)
  {
    const auto& r = *sub.rects[n];
    const auto& b = bounds[n];
    if (!b.pixels)
      continue;
    haveInk = true;
    hash = (hash ^ (b.x1 - b.x0)) * 1099511628211ULL;
    hash = (hash ^ (b.y1 - b.y0)) * 1099511628211ULL;
    for (int y = b.y0; y < b.y1; ++y)
      for (int x = b.x0; x < b.x1; ++x)
      {
        const int index = r.data[0][y * r.linesize[0] + x];
        const uint32_t c = index < r.nb_colors ? AV_RN32(r.data[1] + index * 4) : 0;
        const int hi = std::max({int((c >> 16) & 255), int((c >> 8) & 255), int(c & 255)});
        hash = (hash ^ (hi * (c >> 24) > 6000)) * 1099511628211ULL;
      }
  }
  if (!haveInk)
    return false;
  for (size_t i = 0; i < std::min<size_t>(e.numBitmaps, e.recent.size()); ++i)
    if (e.recent[i] == hash)
      return false;
  e.recent[e.numBitmaps++ % e.recent.size()] = hash;
  return true;
}
void ObserveWord(void* opaque, const char* text)
{
  pgs_sub_word_language(text, static_cast<pgs_sub_caption_language*>(opaque));
}
void ObserveLine(void* opaque, int line, const char* text, int minimumConfidence)
{
  pgs_sub_line_language(text, line, minimumConfidence,
                        static_cast<pgs_sub_caption_language*>(opaque));
}
} // namespace

struct CPGSProbe::Impl
{
  struct Track
  {
    Key key;
    std::string language, title;
    bool forced{};
    pgs_subtitle_info info{}; // mutex protected
    unsigned generation{};
    bool finished{};
    bool sparseFinished{};
    PGSActivity activity; // mutex protected
    CandidateLanguage candidateLanguage{CandidateLanguage::Unknown};
    bool multiline{}; // mutex protected; learned from decoded layout, never guessed from language
    uint64_t lastScheduled{}; // mutex protected, round-robin within one priority
    std::chrono::steady_clock::time_point created{std::chrono::steady_clock::now()};
    Evidence evidence; // worker only
    AVCodecContext* codec{}; // worker only
    unsigned codecGeneration{};
    ~Track() { avcodec_free_context(&codec); }
  };
  struct Packet
  {
    std::shared_ptr<Track> track;
    std::vector<uint8_t> data; // includes AV_INPUT_BUFFER_PADDING_SIZE zero bytes
    size_t size;
    unsigned generation;
    uint64_t epoch;
    double pts;
  };
  struct Image
  {
    std::vector<uint8_t> pixels;
    int w{}, h{}, y{}, lineCount{};
  };
  struct Ready
  {
    Packet packet;
    Evidence evidence;
    pgs_subtitle_info info;
    std::vector<Image> images;
    size_t Size() const
    {
      size_t size = 0;
      for (const auto& image : images) size += image.pixels.size();
      return size;
    }
  };
  std::deque<Ready> ready; // mutex protected, at most one sample per track
  size_t readyBytes{};
  int preferredType{2};
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::map<Key, std::shared_ptr<Track>> tracks;
  std::deque<Packet> queue;
  size_t bytes{};
  uint64_t scheduleSerial{};
  bool enabled{}, started{};
  double sampleClock{}, playbackPosition{NAN}, nextMaintenance{1.0};
  bool playbackRunning{}, maintenance{};
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> epoch{0};
  std::thread thread;
  std::string modelDirectory;
  void* ocr{}; // worker only
  bool triedOcr{};

  void Stop()
  {
    stopping = true;
    ++epoch;
    {
      std::lock_guard<std::mutex> lock(mutex);
#ifdef HAS_PGS_OCR
      if (ocr)
        pgs_ocr_cancel(ocr);
#endif
    }
    wake.notify_all();
    if (thread.joinable())
      thread.join();
    std::lock_guard<std::mutex> lock(mutex);
    queue.clear();
    tracks.clear();
    ready.clear();
    readyBytes = 0;
    bytes = 0;
    started = false;
    scheduleSerial = 0;
    sampleClock = 0;
    playbackPosition = NAN;
    playbackRunning = maintenance = false;
    nextMaintenance = 1.0;
  }
  void ResetActivity()
  {
    for (auto& entry : tracks)
    {
      auto& track = *entry.second;
      track.activity.Reset(sampleClock);
      // A seek starts a fresh observation window for a previously sparse
      // track. Confirmed language evidence is retained, as for other tracks.
      if (track.sparseFinished)
      {
        track.finished = track.info.sampled = track.sparseFinished = false;
      }
    }
    nextMaintenance = sampleClock + 1.0;
    maintenance = false;
  }
  void FinishSparseTracks()
  {
    size_t active = 0;
    for (const auto& entry : tracks)
      active += entry.second->activity.Active(sampleClock);
    if (active < 2 || active * 2 <= tracks.size())
      return;
    for (auto& entry : tracks)
    {
      auto& track = *entry.second;
      if (track.finished || !track.activity.Sparse(sampleClock))
        continue;
      const bool pending = std::any_of(queue.begin(), queue.end(), [&](const Packet& packet)
      {
        return packet.track == entry.second && packet.epoch == epoch &&
               packet.generation == track.generation;
      });
      const bool pendingImage = std::any_of(ready.begin(), ready.end(), [&](const Ready& sample)
      {
        return sample.packet.track == entry.second && sample.packet.epoch == epoch &&
               sample.packet.generation == track.generation;
      });
      if (pending || pendingImage)
        continue; // A low-priority backlog is not evidence of missing subtitles.
      ++track.generation;
      track.finished = track.info.sampled = track.sparseFinished = true;
      avcodec_free_context(&track.codec);
      CLog::Log(LOGDEBUG,
                "PGS identification: stream={} sampling finished: sparse, samples={} active={}/{}",
                track.key.id, track.info.samples, active, tracks.size());
    }
  }
  int Priority(const Track& track) const
  {
    return pgs_sub_priority(track.info, track.candidateLanguage == CandidateLanguage::Chinese,
                            track.multiline, preferredType);
  }
  void PruneReady()
  {
    for (auto it = ready.begin(); it != ready.end();)
    {
      const auto& q = it->packet;
      if (q.epoch != epoch || q.generation != q.track->generation || q.track->finished)
      {
        readyBytes -= it->Size();
        it = ready.erase(it);
      }
      else ++it;
    }
  }
  bool MakeRoom(const Track& incoming, size_t needed)
  {
    const auto full = [&] { return queue.size() >= MAX_QUEUE || needed > MAX_BYTES - bytes; };
    if (!full())
      return true;
    // Stale packets need no decoder state. A backlog from a lower-priority
    // track must not occupy the entire bounded queue and reject Chinese work.
    const auto discard = [&](const auto& predicate)
    {
      for (auto it = queue.begin(); it != queue.end();)
      {
        if (predicate(*it))
        {
          bytes -= it->data.size();
          it = queue.erase(it);
        }
        else
          ++it;
      }
    };
    discard([&](const Packet& packet)
    {
      return packet.epoch != epoch || packet.generation != packet.track->generation ||
             packet.track->finished;
    });
    while (full())
    {
      std::shared_ptr<Track> victim;
      for (const auto& packet : queue)
        if (Priority(*packet.track) > Priority(incoming) &&
            (!victim || Priority(*packet.track) > Priority(*victim)))
          victim = packet.track;
      if (!victim)
        return false;
      // Evict a whole track backlog rather than individual PGS segments; its
      // next accepted packet resets the incomplete decoder before reuse.
      ++victim->generation;
      discard([&](const Packet& packet) { return packet.track == victim; });
    }
    return true;
  }
  // Caller holds mutex. Compare every packet, but a track's rank is the same
  // for all its packets and ties keep the first: PGS segments never overtake
  // earlier segments of their own decoder, even when other tracks move ahead.
  Packet TakeNextPacket()
  {
    const auto rank = [this](const Track& t) { return std::make_pair(Priority(t), t.lastScheduled); };
    auto next = queue.end();
    for (auto it = queue.begin(); it != queue.end(); ++it)
    {
      // Decode another candidate before OCR, without overwriting a pending
      // sample or reordering the PGS segments within a track.
      if (std::any_of(ready.begin(), ready.end(), [&](const Ready& sample)
          { return sample.packet.track == it->track; }))
        continue;
      if (next == queue.end() || rank(*it->track) < rank(*next->track)) next = it;
    }
    if (next == queue.end()) return {};
    Packet packet = std::move(*next);
    queue.erase(next);
    bytes -= packet.data.size();
    packet.track->lastScheduled = ++scheduleSerial;
    return packet;
  }
  struct CancelContext
  {
    Impl& impl;
    uint64_t epoch;
  };
  static bool Cancel(void* opaque)
  {
    const auto& ctx = *static_cast<CancelContext*>(opaque);
    return ctx.impl.stopping || ctx.impl.epoch != ctx.epoch;
  }
  void Publish(const Packet& q, const Evidence& e, pgs_subtitle_info info)
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto& t = *q.track;
    if (stopping || q.epoch != epoch || q.generation != t.generation)
      return;
    info.forced |= t.info.forced;
    const bool changed = t.info.script != info.script || t.info.bilingual != info.bilingual;
    t.info = info;
    t.evidence = e;
    t.finished = info.sampled;
    if (t.finished)
      avcodec_free_context(&t.codec);
    if (changed)
    {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t.created).count();
      CLog::Log(LOGDEBUG,
                "PGS identification: stream={} script={} bilingual={} samples={} "
                "elapsed_ms={} ocr_ms={}",
                t.key.id, pgs_sub_script_name(info.script), info.bilingual,
                info.samples, elapsed, e.ocrMs);
    }
  }
#ifdef HAS_PGS_OCR
  Image PrepareImage(const AVSubtitleRect& r, Bounds b)
  {
    if (!b.pixels)
      return {};
    if (r.w <= 2048 && r.h <= 512)
    {
      b.x0 = b.y0 = 0;
      b.x1 = r.w;
      b.y1 = r.h;
    }
    else
    {
      b.x0 = std::max(0, b.x0 - 8);
      b.x1 = std::min(r.w, b.x1 + 8);
      b.y0 = std::max(0, b.y0 - 8);
      b.y1 = std::min(r.h, b.y1 + 8);
    }
    const int step = std::max({1, (b.x1 - b.x0 + 2047) / 2048, (b.y1 - b.y0 + 511) / 512});
    const int w = (b.x1 - b.x0 + step - 1) / step + 32, h = (b.y1 - b.y0 + step - 1) / step + 32;
    std::vector<uint8_t> image(w * h, 255);
    for (int y = b.y0; y < b.y1; y += step)
      for (int x = b.x0; x < b.x1; x += step)
      {
        const int index = r.data[0][y * r.linesize[0] + x];
        if (index >= r.nb_colors)
          continue;
        const uint32_t c = AV_RN32(r.data[1] + index * 4);
        const int v = std::max({int((c >> 16) & 255), int((c >> 8) & 255), int(c & 255)});
        image[((y - b.y0) / step + 16) * w + (x - b.x0) / step + 16] =
            255 - (v * (c >> 24) + 127) / 255;
      }
    const auto lineCount = PGSFindLines(image.data(), w, h).size();
    return {std::move(image), w, h, r.y + b.y0, static_cast<int>(lineCount)};
  }
#endif
  void Analyse(const Packet& q)
  {
#ifdef HAS_PGS_OCR
    auto& t = *q.track;
    pgs_subtitle_info info{};
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (q.epoch != epoch || q.generation != t.generation || t.finished)
        return;
      info = t.info;
    }
    Evidence e = t.evidence;
    if (e.decoded >= MAX_DECODE_CAPTIONS || e.displaySets >= MAX_DISPLAY_SETS)
    {
      info.sampled = true;
      Publish(q, e, info);
      return;
    }
    if (!t.codec)
    {
      const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HDMV_PGS_SUBTITLE);
      if (!codec)
      {
        info.sampled = true;
        Publish(q, e, info);
        return;
      }
      t.codec = avcodec_alloc_context3(codec);
      if (!t.codec)
      {
        info.sampled = true;
        Publish(q, e, info);
        return;
      }
      t.codec->max_pixels = 4096 * 2160;
      if (avcodec_open2(t.codec, codec, nullptr) < 0)
      {
        info.sampled = true;
        Publish(q, e, info);
        return;
      }
      t.codecGeneration = q.generation;
    }
    const bool jump = std::isfinite(q.pts) && std::isfinite(e.lastPts) && q.pts < e.lastPts;
    if (t.codecGeneration != q.generation || jump)
    {
      avcodec_flush_buffers(t.codec);
      t.codecGeneration = q.generation;
      e.lastPts = NAN;
    }
    AVPacket packet{};
    packet.data = const_cast<uint8_t*>(q.data.data());
    packet.size = static_cast<int>(q.size);
    packet.pts = AV_NOPTS_VALUE;
    AVSubtitle sub{};
    struct SubtitleGuard
    {
      AVSubtitle& sub;
      ~SubtitleGuard() { avsubtitle_free(&sub); }
    } guard{sub};
    int got = 0;
    const int ret = avcodec_decode_subtitle2(t.codec, &sub, &got, &packet);
    if (ret < 0 || !got || !sub.num_rects)
    {
      Publish(q, e, info);
      return;
    }
    ++e.displaySets;
    std::array<Bounds, 4> bounds{};
    for (unsigned n = 0; n < sub.num_rects && n < bounds.size(); ++n)
    {
      const auto& r = *sub.rects[n];
      info.forced |= (r.flags & AV_SUBTITLE_FLAG_FORCED) != 0;
      bounds[n] = BitmapBounds(r);
    }
    if (std::isfinite(q.pts))
      e.lastPts = q.pts;
    const bool due = !std::isfinite(e.lastOcrPts) || !std::isfinite(q.pts) ||
                     q.pts < e.lastOcrPts || q.pts - e.lastOcrPts >= 1.0;
    if (!due || !NewCaption(e, sub, bounds))
    {
      Publish(q, e, info);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (q.epoch != epoch || q.generation != t.generation || t.finished)
        return;
      // Transparent/clear images and duplicate ink never reach this point.
      // Keep their PCS traffic from extending the sparse sampling window.
      t.activity.ObserveSample(sampleClock);
    }
    Ready sample{{q.track, {}, 0, q.generation, q.epoch, q.pts}, e, info, {}};
    int lineCount = 0;
    for (unsigned n = 0; n < sub.num_rects && n < bounds.size(); ++n)
    {
      auto image = PrepareImage(*sub.rects[n], bounds[n]);
      if (!image.lineCount) continue;
      lineCount += image.lineCount;
      sample.images.push_back(std::move(image));
    }
    std::stable_sort(sample.images.begin(), sample.images.end(),
                     [](const Image& a, const Image& b) { return a.y < b.y; });
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (q.epoch != epoch || q.generation != t.generation || t.finished) return;
      t.multiline = lineCount > 1;
      readyBytes += sample.Size();
      ready.push_back(std::move(sample));
    }
#endif
  }
  void RecogniseReady(Ready sample)
  {
#ifdef HAS_PGS_OCR
    const auto& q = sample.packet;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (q.epoch != epoch || q.generation != q.track->generation || q.track->finished) return;
    }
    auto e = sample.evidence;
    auto info = sample.info;
    if (!triedOcr)
    {
      triedOcr = true;
      void* created = pgs_ocr_create(modelDirectory.c_str());
      {
        std::lock_guard<std::mutex> lock(mutex);
        ocr = created;
      }
      CLog::Log(ocr ? LOGDEBUG : LOGWARNING, "PGS identification: CPU recognizer {}",
                ocr ? "ready" : "unavailable");
    }
    ++e.attempts;
    ++e.decoded;
    if (std::isfinite(q.pts))
      e.lastOcrPts = q.pts;
    CancelContext cancel{*this, q.epoch};
    pgs_sub_caption_language caption{};
    std::string text;
    int confidence = 100;
    const auto ocrBegin = std::chrono::steady_clock::now();
    int lineOffset = 0;
    for (const auto& image : sample.images)
    {
      if (!ocr || Cancel(&cancel)) break;
      int conf = 0;
      struct Lines { pgs_sub_caption_language& caption; int offset; } lines{caption, lineOffset};
      char* part = pgs_ocr_recognise(ocr, image.pixels.data(), image.w, image.h, &conf,
          Cancel, &cancel, ObserveWord, &caption,
          [](void* ctx, int line, const char* raw, int minimumConfidence)
          {
            const auto& lines = *static_cast<Lines*>(ctx);
            ObserveLine(&lines.caption, line + lines.offset, raw, minimumConfidence);
          }, &lines);
      if (part && *part && conf >= 55)
      {
        text += part;
        confidence = std::min(confidence, conf);
      }
      pgs_ocr_free_text(part);
      lineOffset += image.lineCount;
    }
    e.ocrMs += std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ocrBegin).count();
    if (Cancel(&cancel))
      return;
    if (!text.empty() && !info.script_from_title)
      pgs_sub_script_feed(&e.script, text.c_str(), confidence);
    if (e.script.confirmed != PGS_SUB_SCRIPT_UNKNOWN)
      info.script = pgs_sub_script_result(&e.script);
    pgs_sub_bilingual_feed(&e.bilingual, &caption, &info);
    info.samples = e.decoded;
    info.ocr_available = ocr != nullptr;
    const bool scriptDone = info.script_from_title || e.script.confirmed != PGS_SUB_SCRIPT_UNKNOWN;
    const bool bilingualDone = info.bilingual || e.decoded >= MAX_BILINGUAL_CAPTIONS;
    info.sampled = (scriptDone && bilingualDone) || (!ocr && e.displaySets >= MAX_DECODE_CAPTIONS) ||
                   e.attempts >= MAX_ATTEMPTS;
    Publish(q, e, info);
#endif
  }
  void Worker()
  {
#if defined(TARGET_LINUX)
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
    try
    {
      unsigned decodedBurst = 0;
      while (!stopping)
      {
        Packet q{};
        Ready sample{};
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock, [this] { return stopping || !enabled || maintenance || !queue.empty() || !ready.empty(); });
          if (stopping)
            break;
          if (!enabled)
          {
#ifdef HAS_PGS_OCR
            if (ocr)
            {
              pgs_ocr_destroy(ocr);
              ocr = nullptr;
              triedOcr = false;
            }
#endif
            wake.wait(lock, [this] { return stopping || enabled; });
            continue;
          }
          if (maintenance)
          {
            maintenance = false;
            FinishSparseTracks();
          }
          PruneReady();
          // Discover layouts cheaply before choosing expensive OCR work.
          // A burst and memory cap prevent unbounded decode-ahead or starvation.
          if (!queue.empty() && decodedBurst < 64 && readyBytes < 8 * 1024 * 1024)
          {
            q = TakeNextPacket();
            if (q.track) ++decodedBurst;
          }
          if (!q.track && !ready.empty())
          {
            auto next = ready.begin();
            for (auto it = std::next(next); it != ready.end(); ++it)
              if (std::make_pair(Priority(*it->packet.track), it->packet.track->lastScheduled) <
                  std::make_pair(Priority(*next->packet.track), next->packet.track->lastScheduled))
                next = it;
            readyBytes -= next->Size();
            sample = std::move(*next);
            ready.erase(next);
            sample.packet.track->lastScheduled = ++scheduleSerial;
            decodedBurst = 0;
          }
          else if (!q.track) decodedBurst = 0;
        }
        if (q.track)
          Analyse(q);
        if (sample.packet.track)
          RecogniseReady(std::move(sample));
#ifdef HAS_PGS_OCR
        std::lock_guard<std::mutex> lock(mutex);
        bool allFinished = !tracks.empty();
        for (const auto& entry : tracks)
          allFinished &= entry.second->finished;
        if (allFinished && ocr)
        {
          pgs_ocr_destroy(ocr);
          ocr = nullptr;
          triedOcr = false;
          CLog::Log(LOGDEBUG, "PGS identification: CPU recognizer released (sampling complete)");
        }
#endif
      }
    }
    catch (const std::exception& e)
    {
      CLog::Log(LOGWARNING, "PGS identification stopped: {}", e.what());
      std::lock_guard<std::mutex> lock(mutex);
      enabled = false;
      queue.clear();
      bytes = 0;
    }
#ifdef HAS_PGS_OCR
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (ocr)
      {
        pgs_ocr_destroy(ocr);
        ocr = nullptr;
      }
    }
#endif
  }
};
CPGSProbe::CPGSProbe() : m_impl(std::make_unique<Impl>())
{
}
CPGSProbe::~CPGSProbe()
{
  m_impl->Stop();
}
void CPGSProbe::Reset(bool enabled, const std::string& modelDirectory)
{
  m_impl->Stop();
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  m_impl->enabled = enabled;
  m_impl->modelDirectory = modelDirectory;
  m_impl->triedOcr = false;
  m_impl->stopping = false;
}
void CPGSProbe::SetPreferredType(int preference)
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  m_impl->preferredType = std::clamp(preference, 0, 3);
}
void CPGSProbe::Flush()
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  ++m_impl->epoch;
#ifdef HAS_PGS_OCR
  if (m_impl->ocr)
    pgs_ocr_cancel(m_impl->ocr);
#endif
  m_impl->queue.clear();
  m_impl->ready.clear();
  m_impl->readyBytes = 0;
  m_impl->bytes = 0;
  for (auto& entry : m_impl->tracks)
    ++entry.second->generation;
  m_impl->ResetActivity();
  m_impl->playbackPosition = NAN;
  m_impl->wake.notify_all();
}
void CPGSProbe::SetEnabled(bool enabled)
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  if (m_impl->enabled == enabled)
    return;
  m_impl->enabled = enabled;
  ++m_impl->epoch;
#ifdef HAS_PGS_OCR
  if (m_impl->ocr)
    pgs_ocr_cancel(m_impl->ocr);
#endif
  m_impl->queue.clear();
  m_impl->ready.clear();
  m_impl->readyBytes = 0;
  m_impl->bytes = 0;
  for (auto& entry : m_impl->tracks)
    ++entry.second->generation;
  m_impl->ResetActivity();
  m_impl->wake.notify_all();
}
void CPGSProbe::UpdatePlayback(double positionSeconds, bool playing)
{
#ifdef HAS_PGS_OCR
  auto& p = *m_impl;
  std::lock_guard<std::mutex> lock(p.mutex);
  if (!p.enabled || p.stopping || !std::isfinite(positionSeconds))
    return;
  if (std::isfinite(p.playbackPosition))
  {
    const double delta = positionSeconds - p.playbackPosition;
    if (delta < -1.0 || delta > 5.0)
    {
      p.ResetActivity(); // Also protect against discontinuities without Flush.
      p.playbackPosition = positionSeconds;
    }
    else if (playing && p.playbackRunning && delta > 0)
    {
      p.sampleClock += delta;
      p.playbackPosition = positionSeconds;
    }
    else if (!playing || !p.playbackRunning)
      p.playbackPosition = positionSeconds;
    // Small clock corrections keep the previous high-water mark. They must
    // neither restart a full observation window nor count the same time twice.
  }
  else
    p.playbackPosition = positionSeconds;
  p.playbackRunning = playing;
  if (p.ocr && p.sampleClock >= p.nextMaintenance)
  {
    p.nextMaintenance = p.sampleClock + 1.0;
    p.maintenance = true;
    p.wake.notify_one();
  }
#endif
}
void CPGSProbe::Feed(Key key,
                     const std::string& language,
                     const std::string& title,
                     const uint8_t* data,
                     size_t size,
                     double pts,
                     bool forced)
{
#ifdef HAS_PGS_OCR
  auto& p = *m_impl;
  std::lock_guard<std::mutex> lock(p.mutex);
  if (!p.enabled || p.stopping)
    return;
  try
  {
    auto it = p.tracks.find(key);
    if (it != p.tracks.end() && (it->second->language != language || it->second->title != title ||
                                 it->second->forced != forced))
    {
      // A live metadata update takes precedence over previous OCR evidence.
      ++it->second->generation;
      it->second->finished = true;
      p.tracks.erase(it);
      it = p.tracks.end();
    }
    if (it == p.tracks.end())
    {
      const auto candidateLanguage = static_cast<CandidateLanguage>(pgs_sub_candidate_language(language, title));
      if (candidateLanguage == CandidateLanguage::Other || p.tracks.size() >= MAX_TRACKS)
        return;
      auto t = std::make_shared<Impl::Track>();
      t->key = key;
      t->language = language;
      t->title = title;
      t->forced = forced;
      t->candidateLanguage = candidateLanguage;
      t->activity.Reset(p.sampleClock);
      pgs_subtitle_info_from_metadata(&t->info, language.c_str(), title.c_str(), forced);
      t->finished = t->info.descriptive_title;
      t->info.sampled = t->finished;
      it = p.tracks.emplace(key, t).first;
    }
    auto& t = it->second;
    // Metadata-only registration includes silent tracks in the candidate set,
    // without opening a decoder, starting OCR or counting packet activity.
    if (!data || !size) return;
    if (size > MAX_BYTES - AV_INPUT_BUFFER_PADDING_SIZE)
    {
      ++t->generation;
      return;
    }
    t->info.forced |= pgs_packet_has_forced(data, size);
    if (pgs_packet_has_objects(data, size))
      t->activity.Observe(p.sampleClock);
    if (t->finished)
      return;
    if (!p.MakeRoom(*t, size + AV_INPUT_BUFFER_PADDING_SIZE))
    {
      ++t->generation; // the next accepted packet must reset incomplete PGS state
      return;
    }
    Impl::Packet q{t,       std::vector<uint8_t>(size + AV_INPUT_BUFFER_PADDING_SIZE, 0),
                   size,    t->generation,
                   p.epoch, pts};
    std::copy_n(data, size, q.data.begin());
    p.queue.push_back(std::move(q));
    p.bytes += size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (!p.started)
    {
      p.thread = std::thread([&p] { p.Worker(); });
      p.started = true;
    }
    p.wake.notify_one();
  }
  catch (const std::exception& e)
  {
    p.enabled = false;
    p.queue.clear();
    p.bytes = 0;
    CLog::Log(LOGWARNING, "PGS identification disabled: {}", e.what());
  }
#endif
}
pgs_subtitle_info CPGSProbe::Get(Key key) const
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  const auto it = m_impl->tracks.find(key);
  return it == m_impl->tracks.end() ? pgs_subtitle_info{} : it->second->info;
}
