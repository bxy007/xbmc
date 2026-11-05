/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <tuple>
#include <vector>

// Player-thread-only compressed packet history. No decoder or OCR session per
// inactive track. The caller admits only Chinese/unknown main-demux subtitles.
class CSubtitleSwitchCache
{
public:
  struct Key
  {
    int64_t demuxer;
    int source;
    int id;
    bool operator<(const Key& b) const
    {
      return std::tie(demuxer, source, id) < std::tie(b.demuxer, b.source, b.id);
    }
  };
  struct Packet
  {
    std::vector<uint8_t> data;
    double pts, dts, duration, correction;
    int group;
    bool epoch{false};
  };
  static constexpr size_t MAX_BYTES = 16 * 1024 * 1024;
  static constexpr size_t MAX_TRACK_BYTES = 4 * 1024 * 1024;
  static constexpr size_t MAX_TRACKS = 32;
  static constexpr size_t MAX_PACKETS = 1024;

  void Clear() { m_tracks.clear(); m_bytes = 0; }
  size_t Bytes() const { return m_bytes; }
  void Erase(Key key)
  {
    const auto it = m_tracks.find(key);
    if (it != m_tracks.end()) { m_bytes -= it->second.bytes; m_tracks.erase(it); }
  }

  void Add(Key key, int codec, int revision, bool pgs, Packet packet, double now)
  {
    if (packet.data.empty() || packet.data.size() > MAX_TRACK_BYTES)
    {
      Erase(key);
      return;
    }
    if (pgs)
    {
      if (!ParsePGS(packet.data, packet.epoch)) { Erase(key); return; }
    }
    auto it = m_tracks.find(key);
    if (it != m_tracks.end() && (it->second.codec != codec || it->second.revision != revision))
    {
      Erase(key);
      it = m_tracks.end();
    }
    if (it == m_tracks.end())
    {
      // PGS palettes and objects may be reused. Never start replay mid-epoch.
      if (pgs && !packet.epoch) return;
      if (m_tracks.size() == MAX_TRACKS) EvictOldest();
      it = m_tracks.emplace(key, Track{}).first;
      it->second.codec = codec;
      it->second.revision = revision;
      it->second.pgs = pgs;
    }
    auto& track = it->second;
    track.used = ++m_serial;
    track.bytes += packet.data.size();
    m_bytes += packet.data.size();
    track.packets.push_back(std::move(packet));
    Prune(track, now);
    if (track.bytes > MAX_TRACK_BYTES || track.packets.size() > MAX_PACKETS)
      Erase(key); // Drop the whole dependency chain, never a partial PGS epoch.
    while (m_bytes > MAX_BYTES) EvictOldest();
  }

  std::vector<Packet> Get(Key key, int codec, int revision, double now)
  {
    const auto it = m_tracks.find(key);
    if (it == m_tracks.end()) return {};
    if (it->second.codec != codec || it->second.revision != revision)
    {
      Erase(key);
      return {};
    }
    Prune(it->second, now);
    return {it->second.packets.begin(), it->second.packets.end()};
  }

private:
  struct Track
  {
    int codec{0}, revision{0};
    bool pgs{false};
    size_t bytes{0};
    uint64_t used{0};
    std::deque<Packet> packets;
  };
  static bool ParsePGS(const std::vector<uint8_t>& data, bool& epoch)
  {
    size_t pos = 0;
    while (pos < data.size())
    {
      if (data.size() - pos < 3) return false;
      const unsigned type = data[pos];
      const size_t length = (data[pos + 1] << 8) | data[pos + 2];
      pos += 3;
      if (length > data.size() - pos) return false;
      if (type == 0x16 && length >= 11 && (data[pos + 7] & 0x80)) epoch = true;
      pos += length;
    }
    return true;
  }
  void Prune(Track& track, double now)
  {
    if (!std::isfinite(now)) return;
    size_t remove = 0;
    if (track.pgs)
    {
      // Demuxing runs ahead of display: keep the epoch containing the actual
      // visible time, not just the newest (possibly future) epoch.
      for (size_t i = 1; i < track.packets.size(); ++i)
        if (track.packets[i].epoch && track.packets[i].pts >= 0 &&
            track.packets[i].pts <= now - 1000000.0)
          remove = i;
    }
    else
    {
      while (remove < track.packets.size())
      {
        const auto& p = track.packets[remove];
        if (p.pts < 0 || p.duration <= 0 || p.pts + p.duration >= now - 120000000.0) break;
        ++remove;
      }
    }
    while (remove-- > 0)
    {
      const size_t size = track.packets.front().data.size();
      track.bytes -= size;
      m_bytes -= size;
      track.packets.pop_front();
    }
  }
  void EvictOldest()
  {
    if (m_tracks.empty()) return;
    const auto oldest = std::min_element(m_tracks.begin(), m_tracks.end(),
        [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
    Erase(oldest->first);
  }
  std::map<Key, Track> m_tracks;
  size_t m_bytes{0};
  uint64_t m_serial{0};
};
