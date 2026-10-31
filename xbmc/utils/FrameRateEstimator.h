/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

// Like CSystemGUIInfo::UpdateFPS, update once per second using frame counts and
// elapsed wall time. Here the counts belong to the video sink, not the GUI.
class CFrameRateEstimator
{
public:
  using Clock = std::chrono::steady_clock;
  struct Rates
  {
    double input;
    double output;
    double difference;
  };

  void Reset()
  {
    m_initialized = false;
    m_rates.reset();
  }

  std::optional<Rates> Sample(Clock::time_point now, uint32_t input, uint32_t output)
  {
    if (m_initialized)
    {
      // Small unsigned deltas also handle counter wrap. Large backwards jumps
      // are provider resets, rather than billions of newly displayed frames.
      if (now <= m_previous.time || now - m_previous.time > std::chrono::seconds(2) ||
          input - m_previous.input > std::numeric_limits<uint32_t>::max() / 2 ||
          output - m_previous.output > std::numeric_limits<uint32_t>::max() / 2)
        Reset();
    }

    m_previous = {now, input, output};
    if (!m_initialized)
    {
      m_first = m_previous;
      m_initialized = true;
      return std::nullopt;
    }

    if (now - m_first.time < std::chrono::seconds(1))
      return m_rates;

    const double seconds = std::chrono::duration<double>(now - m_first.time).count();
    const double inputRate = static_cast<uint32_t>(input - m_first.input) / seconds;
    const double outputRate = static_cast<uint32_t>(output - m_first.output) / seconds;
    m_rates = Rates{inputRate, outputRate, std::max(0.0, inputRate - outputRate)};
    m_first = m_previous;
    return m_rates;
  }

private:
  struct SamplePoint
  {
    Clock::time_point time;
    uint32_t input;
    uint32_t output;
  };

  SamplePoint m_first{};
  SamplePoint m_previous{};
  bool m_initialized{false};
  std::optional<Rates> m_rates;
};
