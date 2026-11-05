/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

// A clock measured in uninterrupted advancing playback seconds, not demux
// read-ahead PTS or wall time. PCS activity establishes the peer-track quorum;
// only a new visible OCR candidate renews this track's sampling deadline.
struct PGSActivity
{
  static constexpr double WINDOW_SECONDS = 60.0;
  double started{}, lastData{}, lastSample{};
  std::array<double, 3> recent{};
  size_t count{};

  void Reset(double clock)
  {
    started = lastData = lastSample = clock;
    count = 0;
    recent = {};
  }
  void Observe(double clock)
  {
    std::rotate(recent.begin(), recent.begin() + 1, recent.end());
    recent.back() = lastData = clock;
    count = std::min(count + 1, recent.size());
  }
  void ObserveSample(double clock) { lastSample = clock; }
  bool Active(double clock) const
  {
    return clock - started >= WINDOW_SECONDS && count == recent.size() &&
           recent.front() >= clock - WINDOW_SECONDS &&
           lastData >= clock - WINDOW_SECONDS / 3;
  }
  bool Sparse(double clock) const
  {
    return clock - started >= WINDOW_SECONDS && clock - lastSample >= WINDOW_SECONDS;
  }
};
