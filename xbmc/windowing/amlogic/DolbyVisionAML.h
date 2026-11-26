/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "interfaces/IAnnouncer.h"
#include "settings/lib/ISettingCallback.h"

#include <atomic>
#include <chrono>

class CDolbyVisionAML : public ANNOUNCEMENT::IAnnouncer, // Application callback
                        public ISettingCallback          // Settings callback
{
public:
  CDolbyVisionAML();
  ~CDolbyVisionAML() override;

  // Setup
  bool Setup();
  static void ValidateOutputSettings();
  void ProcessStdOutputCheck();

  // implementation of IAnnouncer
  void Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                const std::string& sender,
                const std::string& message,
                const CVariant& data) override;

  // implementation of ISettingCallback
  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

private:
  static void ApplyStdOutputSetting();
  bool CheckStdOutputStatus(int mode);

  // Announcements and settings callbacks publish the latest request. Only the
  // application thread owns the deadline and reads playback status.
  std::atomic<bool> m_stdOutputDirty{false};
  enum class PlaybackCheck { None, Start, Stop };
  std::atomic<PlaybackCheck> m_requestedCheck{PlaybackCheck::None};
  int m_checkMode{0};
  unsigned int m_checksLeft{0};
  std::chrono::steady_clock::time_point m_nextCheck;
};
