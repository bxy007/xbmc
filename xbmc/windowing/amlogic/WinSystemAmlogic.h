/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "platform/linux/input/LibInputHandler.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "threads/CriticalSection.h"
#include "windowing/WinSystem.h"
#include "threads/SystemClock.h"
#include "system_egl.h"
#include <EGL/fbdev_window.h>
#include "DolbyVisionAML.h"

#include <mutex>

class IDispResource;

class CWinSystemAmlogic : public CWinSystemBase
{
public:
  CWinSystemAmlogic();

  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;

  bool DestroyWindow() override;
  void UpdateResolutions() override;
  bool IsHDRDisplay() override;
  CHDRCapabilities GetDisplayHDRCapabilities() const override;
  float GetDisplayLatency() override;
  float GetGuiSdrPeakLuminance() const override;
  float GetGuiSdrSaturation() const override;
  float GetGuiSrgbDecode() const override;
  float GetGuiDither8Bit() const override;
  bool GuiPqIsFinalStage() const;

  bool Hide() override;
  bool Show(bool show = true) override;
  virtual void Register(IDispResource *resource);
  virtual void Unregister(IDispResource *resource);
  void PrepareVideoModeSwitch();
  bool DeferVideoModeSwitch();
  bool IsVideoModeSwitchPending();
  void CancelVideoModeSwitch();
protected:
  void BeginModeSwitchBlank();
  void EndModeSwitchBlank();
  void OsdReassertTick();

  std::string m_framebuffer_name;
  EGLDisplay m_nativeDisplay;
  fbdev_window *m_nativeWindow;

  RENDER_STEREO_MODE m_stereo_mode;

  bool m_delayDispReset;
  XbmcThreads::EndTime<> m_dispResetTimer;

  CCriticalSection m_resourceSection;
  std::vector<IDispResource*> m_resources;
  std::unique_ptr<CLibInputHandler> m_libinput;
  CHDRCapabilities m_hdr_caps;
  bool m_force_mode_switch;
  bool m_modeSwitchBlanked;
  int m_modeSwitchFb0Blank;
  int m_modeSwitchFb1Blank;
  int m_osdReassertFrames{0};

private:
  void BeginModeSwitchBlankLocked();
  void EndModeSwitchBlankLocked();
  std::mutex m_modeSwitchBlankMutex;
  bool m_videoModeSwitchPrepared{false};
  bool m_videoModeSwitchRefreshDeferred{false};
  XbmcThreads::EndTime<> m_videoModeSwitchBlankTimer;
  std::unique_ptr<CDolbyVisionAML> m_dolbyVisionAML;
};
