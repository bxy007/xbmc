/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * See LICENSES/README.md for more information.
 */
#pragma once

#include "PGSIdentify.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

// Passive, per-file analysis. Owns packet copies; never owns a demuxer or track.
class CPGSProbe
{
public:
  struct Key
  {
    int64_t demuxer;
    int source;
    int id;
    bool operator<(const Key& other) const
    {
      return std::tie(demuxer, source, id) < std::tie(other.demuxer, other.source, other.id);
    }
  };
  CPGSProbe();
  ~CPGSProbe();
  void Reset(bool enabled, const std::string& modelDirectory);
  void SetEnabled(bool enabled);
  void SetPreferredType(int preference);
  void Flush();
  // Feed real playback progression; paused/buffering time must not age samples.
  void UpdatePlayback(double positionSeconds, bool playing);
  // A null/empty packet registers metadata without decoding or activity.
  void Feed(Key key,
            const std::string& language,
            const std::string& title,
            const uint8_t* data,
            size_t size,
            double pts,
            bool forced = false);
  pgs_subtitle_info Get(Key key) const;

private:
  struct Impl;
  const std::unique_ptr<Impl> m_impl;
};
