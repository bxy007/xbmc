/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "WinSystemAmlogicGLESContext.h"
#include "ServiceBroker.h"
#include "platform/linux/SysfsPath.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/GuiActivity.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "system_gl.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

#include <chrono>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC_64
#define FBIO_WAITFORVSYNC_64 _IOW('F', 0x21, unsigned int)
#endif

using namespace KODI;
using namespace KODI::WINDOWING::AML;

void CWinSystemAmlogicGLESContext::Register()
{
  KODI::WINDOWING::CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "aml");
}

std::unique_ptr<CWinSystemBase> CWinSystemAmlogicGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemAmlogicGLESContext>();
}

bool CWinSystemAmlogicGLESContext::InitWindowSystem()
{
  if (!CWinSystemAmlogic::InitWindowSystem())
  {
    return false;
  }

  if (!m_pGLContext.CreateDisplay(m_nativeDisplay))
  {
    return false;
  }

  if (!m_pGLContext.InitializeDisplay(EGL_OPENGL_ES_API))
  {
    return false;
  }

  if (!m_pGLContext.ChooseConfig(EGL_OPENGL_ES2_BIT))
  {
    return false;
  }

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, 2}});

  if (!m_pGLContext.CreateContext(contextAttribs))
  {
    return false;
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindowSystem()
{
  if (m_vsyncFd >= 0)
  {
    close(m_vsyncFd);
    m_vsyncFd = -1;
  }
  m_pGLContext.DestroyContext();
  m_pGLContext.Destroy();
  return CWinSystemAmlogic::DestroyWindowSystem();
}

bool CWinSystemAmlogicGLESContext::BindTextureUploadContext()
{
  return m_pGLContext.BindTextureUploadContext();
}

bool CWinSystemAmlogicGLESContext::UnbindTextureUploadContext()
{
  return m_pGLContext.UnbindTextureUploadContext();
}

bool CWinSystemAmlogicGLESContext::CreateOverlayContext()
{
  return m_pGLContext.CreateOverlayContext();
}

bool CWinSystemAmlogicGLESContext::BindOverlayContext()
{
  return m_pGLContext.BindOverlayContext();
}

bool CWinSystemAmlogicGLESContext::UnbindOverlayContext()
{
  return m_pGLContext.UnbindOverlayContext();
}

void CWinSystemAmlogicGLESContext::DestroyOverlayContext()
{
  m_pGLContext.DestroyOverlayContext();
}

bool CWinSystemAmlogicGLESContext::HasContext()
{
  return m_pGLContext.HasContext();
}

bool CWinSystemAmlogicGLESContext::CreateNewWindow(const std::string& name,
                                               bool fullScreen,
                                               RESOLUTION_INFO& res)
{
  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  RENDER_STEREO_MODE stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  // check for frac_rate_policy change
  int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  int cur_fractional_rate = fractional_rate;
  if (aml_has_frac_rate_policy())
  {
    CSysfsPath amhdmitx0_frac_rate_policy{"/sys/class/amhdmitx/amhdmitx0/frac_rate_policy"};
    cur_fractional_rate = amhdmitx0_frac_rate_policy.Get<int>().value();
  }

  // Content HDR can change while global DV keeps the same HDMI output. Only
  // a change of output signalling requires the extra DV mode switch.
  const StreamHdrType hdrType = GetGfxContext().GetHDRType();
  const StreamHdrType outputHdrType = aml_get_output_hdr_type(hdrType);
  bool force_mode_switch_by_dv =
         ((outputHdrType != m_outputHdrType) &&
          ((outputHdrType == StreamHdrType::HDR_TYPE_DOLBYVISION) || (m_outputHdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)) &&
       (aml_dv_mode() != DV_MODE_OFF));

  // get current used resolution
  if (!aml_get_native_resolution(current_resolution))
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext::{}: failed to receive current resolution", __FUNCTION__);
    return false;
  }

  const std::string new_hdrStr = CStreamDetails::HdrTypeToString(hdrType);
  const std::string output_hdrStr = CStreamDetails::HdrTypeToString(outputHdrType);
  const std::string old_hdrStr = CStreamDetails::HdrTypeToString(m_outputHdrType);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "m_bWindowCreated: {}, "
    "frac rate {:d}({:d}), "
    "content HDR: {}, output HDR: {}({}), force mode switch: {}",
    __FUNCTION__,
    m_bWindowCreated,
    fractional_rate, cur_fractional_rate,
    new_hdrStr.empty() ? "none" : new_hdrStr,
    output_hdrStr.empty() ? "none" : output_hdrStr,
    old_hdrStr.empty() ? "none" : old_hdrStr, force_mode_switch_by_dv);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "cur: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}",
    __FUNCTION__,
    current_resolution.iWidth, current_resolution.iHeight, current_resolution.iScreenWidth, current_resolution.iScreenHeight,
    current_resolution.fRefreshRate, current_resolution.dwFlags);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "res: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}",
    __FUNCTION__,
    res.iWidth, res.iHeight, res.iScreenWidth, res.iScreenHeight, res.fRefreshRate, res.dwFlags);

  // check if mode switch is needed
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      m_bFullScreen == fullScreen && current_resolution.fRefreshRate == res.fRefreshRate &&
      (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
      m_stereo_mode == stereo_mode && m_bWindowCreated &&
      !force_mode_switch_by_dv &&
      (fractional_rate == cur_fractional_rate))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: No need to create a new window", __FUNCTION__);
    return true;
  }

  // destroy old window, then create a new one
  DestroyWindow();

  // check if a forced mode switch is required
  if (((current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
        current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
        current_resolution.fRefreshRate == res.fRefreshRate) &&
       (force_mode_switch_by_dv ||
       (fractional_rate != cur_fractional_rate))) ||
       (m_stereo_mode != stereo_mode))
  {
    m_force_mode_switch = true;
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: force mode switch", __FUNCTION__);
  }

  if ((current_resolution.dwFlags & D3DPRESENTFLAG_MODE3DFP) !=
      (res.dwFlags & D3DPRESENTFLAG_MODE3DFP))
    m_force_mode_switch = true;

  // refresh backup data
  m_stereo_mode = stereo_mode;
  m_bFullScreen = fullScreen;

  if (!CWinSystemAmlogic::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  if (!m_pGLContext.CreateSurface(static_cast<EGLNativeWindowType>(m_nativeWindow)))
  {
    EndModeSwitch();
    return false;
  }

  if (!m_pGLContext.BindContext())
  {
    EndModeSwitch();
    return false;
  }

  if (!m_delayDispReset)
  {
    std::lock_guard lock(m_resourceSection);

    // tell any shared resources
    for (auto i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();

    EndModeSwitch();
  }

  // Deferred DV activation is applied by the native mode write above.
  m_outputHdrType = aml_get_output_hdr_type(hdrType);
  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindow()
{
  m_pGLContext.DestroySurface();
  return CWinSystemAmlogic::DestroyWindow();
}

bool CWinSystemAmlogicGLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemAmlogicGLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  CreateNewWindow("", fullScreen, res);
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

void CWinSystemAmlogicGLESContext::SetVSyncImpl(bool enable)
{
  if (!m_pGLContext.SetVSync(enable))
  {
    CLog::Log(LOGERROR, "{},Could not set egl vsync", __FUNCTION__);
  }
}

void CWinSystemAmlogicGLESContext::PresentRenderImpl(bool rendered)
{
  ProcessPendingVideoModeSwitch();
  aml_dv_display_trigger_tick();
  if (m_delayDispReset && m_dispResetTimer.IsTimePast())
  {
    m_delayDispReset = false;

    std::lock_guard lock(m_resourceSection);

    // tell any shared resources
    for (auto i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();

    EndModeSwitch();
  }
  if (!rendered)
    return;

  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings =
      settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;

  static uint32_t s_presentN = 0;
  static int64_t s_swapUsTotal = 0, s_swapUsMax = 0, s_gapUsTotal = 0, s_gapUsMin = 0;
  static int64_t s_vsyncWaitUsTotal = 0, s_vsyncWaitUsMax = 0;
  static int64_t s_gpuLateUsTotal = 0, s_gpuLateUsMax = 0;
  static uint32_t s_gpuLateN = 0;
  static uint32_t s_sameIntvN = 0;
  static int64_t s_swapEdgePrev = 0;
  static uint64_t s_vsyncTsPrev = 0, s_vsyncTsLast = 0;
  static int64_t s_vsIntvMin = 0, s_vsIntvMax = 0, s_vsIntvSum = 0, s_vsToSwapSum = 0;
  static uint32_t s_vsIntvN = 0, s_vsToSwapN = 0;
  static auto s_presentWindow = std::chrono::steady_clock::now();
  static auto s_lastPresent = std::chrono::steady_clock::time_point{};
  static bool s_presentProbeArmed = false;

  const bool presentProbe = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                            CServiceBroker::GetLogging().CanLogComponent(LOGWINDOWING);

  void* presentFence = nullptr;
  bool latchAfterSwap = false;
  if (advancedSettings && advancedSettings->m_guiWaitGpuBeforeSwap > 0 &&
      advancedSettings->m_guiWaitVsyncBeforeSwap && !m_vsyncAlignFailed && !m_delayDispReset)
  {
    if (advancedSettings->m_guiWaitGpuBeforeSwap == 2 && GetGfxContext().IsFullScreenVideo())
    {
      presentFence = CreateGuiRenderFence();
      latchAfterSwap = presentFence != nullptr;
    }
    glFlush();
  }

  if (advancedSettings && advancedSettings->m_guiWaitVsyncBeforeSwap && !m_vsyncAlignFailed &&
      !m_delayDispReset)
  {
    if (m_vsyncFd < 0)
    {
      m_vsyncFd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
      if (m_vsyncFd < 0)
      {
        m_vsyncAlignFailed = true;
        logM(LOGWARNING, "PresentRenderImpl - cannot open /dev/fb0, vsync alignment disabled");
      }
    }
    if (m_vsyncFd >= 0 && !latchAfterSwap)
    {
      uint64_t vsyncTs = 0;
      std::chrono::steady_clock::time_point t_vsyncWait;
      if (presentProbe)
        t_vsyncWait = std::chrono::steady_clock::now();
      if (ioctl(m_vsyncFd, FBIO_WAITFORVSYNC_64, &vsyncTs) != 0)
      {
        m_vsyncAlignFailed = true;
        logM(LOGWARNING,
             "PresentRenderImpl - FBIO_WAITFORVSYNC_64 failed, vsync alignment disabled");
      }
      if (presentProbe)
      {
        const int64_t vsyncWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - t_vsyncWait)
                                        .count();
        s_vsyncWaitUsTotal += vsyncWaitUs;
        if (vsyncWaitUs > s_vsyncWaitUsMax)
          s_vsyncWaitUsMax = vsyncWaitUs;
        if (s_presentProbeArmed && s_vsyncTsPrev && vsyncTs > s_vsyncTsPrev)
        {
          const int64_t vsIntvUs = static_cast<int64_t>((vsyncTs - s_vsyncTsPrev) / 1000);
          s_vsIntvSum += vsIntvUs;
          ++s_vsIntvN;
          if (!s_vsIntvMin || vsIntvUs < s_vsIntvMin)
            s_vsIntvMin = vsIntvUs;
          if (vsIntvUs > s_vsIntvMax)
            s_vsIntvMax = vsIntvUs;
        }
        s_vsyncTsPrev = vsyncTs;
        s_vsyncTsLast = vsyncTs;
      }
    }
  }

  if (presentFence)
  {
    const bool guiComplete = WaitGuiRenderFence(presentFence, true);
    if (!guiComplete)
    {
      std::chrono::steady_clock::time_point t_late;
      if (presentProbe)
        t_late = std::chrono::steady_clock::now();
      WaitGuiRenderFence(presentFence, false);
      if (presentProbe)
      {
        const int64_t gpuLateUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t_late)
                                      .count();
        ++s_gpuLateN;
        s_gpuLateUsTotal += gpuLateUs;
        if (gpuLateUs > s_gpuLateUsMax)
          s_gpuLateUsMax = gpuLateUs;
      }
    }
    DeleteGuiRenderFence(presentFence);
  }

  if (presentProbe)
  {
    int64_t swapEdgeNs = 0;
    if (aml_get_vsync_edge(swapEdgeNs))
    {
      if (s_swapEdgePrev != 0 && swapEdgeNs == s_swapEdgePrev)
        ++s_sameIntvN;
      s_swapEdgePrev = swapEdgeNs;
    }
  }

  int64_t edgeBeforeSwap = 0;
  bool haveEdgeBeforeSwap = false;
  if (latchAfterSwap && m_vsyncFd >= 0)
    haveEdgeBeforeSwap = aml_get_vsync_edge(edgeBeforeSwap);

  // Ignore errors - eglSwapBuffers() sometimes fails during modeswaps on AML,
  // there is probably nothing we can do about it
  const auto t_swap = std::chrono::steady_clock::now();
  if (!m_pGLContext.TrySwapBuffers())
    m_failedSwap = true;
  const auto t_done = std::chrono::steady_clock::now();
  const auto swap_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t_done - t_swap).count();
  if (swap_ms > 100)
    logM(LOGWARNING, "PresentRenderImpl - eglSwapBuffers blocked {}ms (post-modeswitch vsync stall?)", swap_ms);

  if (latchAfterSwap && m_vsyncFd >= 0 && !m_vsyncAlignFailed)
  {
    int64_t edgeAfterSwap = 0;
    const bool latched = haveEdgeBeforeSwap && aml_get_vsync_edge(edgeAfterSwap) &&
                         edgeAfterSwap != edgeBeforeSwap;
    if (!latched)
    {
      std::chrono::steady_clock::time_point t_latch;
      if (presentProbe)
        t_latch = std::chrono::steady_clock::now();
      uint64_t vsyncTs = 0;
      if (ioctl(m_vsyncFd, FBIO_WAITFORVSYNC_64, &vsyncTs) != 0)
      {
        m_vsyncAlignFailed = true;
        logM(LOGWARNING,
             "PresentRenderImpl - FBIO_WAITFORVSYNC_64 failed, vsync alignment disabled");
      }
      if (presentProbe)
      {
        const int64_t latchWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - t_latch)
                                        .count();
        s_vsyncWaitUsTotal += latchWaitUs;
        if (latchWaitUs > s_vsyncWaitUsMax)
          s_vsyncWaitUsMax = latchWaitUs;
        if (s_presentProbeArmed && s_vsyncTsPrev && vsyncTs > s_vsyncTsPrev)
        {
          const int64_t vsIntvUs = static_cast<int64_t>((vsyncTs - s_vsyncTsPrev) / 1000);
          s_vsIntvSum += vsIntvUs;
          ++s_vsIntvN;
          if (!s_vsIntvMin || vsIntvUs < s_vsIntvMin)
            s_vsIntvMin = vsIntvUs;
          if (vsIntvUs > s_vsIntvMax)
            s_vsIntvMax = vsIntvUs;
        }
        if (vsyncTs)
        {
          s_vsyncTsPrev = vsyncTs;
          s_vsyncTsLast = vsyncTs;
        }
      }
    }
  }

  if (!presentProbe)
  {
    s_presentProbeArmed = false;
    return;
  }

  if (!s_presentProbeArmed)
  {
    s_presentProbeArmed = true;
    s_lastPresent = std::chrono::steady_clock::time_point{};
    s_presentWindow = t_done;
    s_presentN = 0;
    s_sameIntvN = 0;
    s_swapEdgePrev = 0;
    s_swapUsTotal = s_swapUsMax = s_gapUsTotal = s_gapUsMin = 0;
    s_vsyncWaitUsTotal = s_vsyncWaitUsMax = 0;
    s_gpuLateUsTotal = s_gpuLateUsMax = 0;
    s_gpuLateN = 0;
    s_vsIntvMin = s_vsIntvMax = s_vsIntvSum = s_vsToSwapSum = 0;
    s_vsIntvN = s_vsToSwapN = 0;
  }

  if (s_vsyncTsLast)
  {
    const int64_t swapNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_swap.time_since_epoch()).count();
    const int64_t vsToSwapUs = (swapNs - static_cast<int64_t>(s_vsyncTsLast)) / 1000;
    if (vsToSwapUs > -1000000 && vsToSwapUs < 1000000)
    {
      s_vsToSwapSum += vsToSwapUs;
      ++s_vsToSwapN;
    }
  }

  const int64_t swapUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t_done - t_swap).count();
  s_swapUsTotal += swapUs;
  if (swapUs > s_swapUsMax)
    s_swapUsMax = swapUs;
  if (s_lastPresent.time_since_epoch().count())
  {
    const int64_t gapUs =
        std::chrono::duration_cast<std::chrono::microseconds>(t_swap - s_lastPresent).count();
    s_gapUsTotal += gapUs;
    if (!s_gapUsMin || gapUs < s_gapUsMin)
      s_gapUsMin = gapUs;
  }
  s_lastPresent = t_swap;
  ++s_presentN;

  if (t_done - s_presentWindow >= KODI::UTILS::GUIACTIVITY::HeartbeatInterval())
  {
    logComponentM(LOGDEBUG, LOGWINDOWING,
                  "present: n={} swapAvgUs={} swapMaxUs={} gapAvgUs={} gapMinUs={} sameIntv={} "
                  "vsyncWaitUs={} vsyncWaitMaxUs={} gpuLate={} gpuLateUs={} gpuLateMaxUs={} "
                  "vsIntvAvgUs={} vsIntvMinUs={} vsIntvMaxUs={} vsToSwapAvgUs={}",
                  s_presentN, s_presentN ? s_swapUsTotal / s_presentN : 0, s_swapUsMax,
                  s_presentN > 1 ? s_gapUsTotal / (s_presentN - 1) : 0, s_gapUsMin, s_sameIntvN,
                  s_vsyncWaitUsTotal, s_vsyncWaitUsMax, s_gpuLateN, s_gpuLateUsTotal,
                  s_gpuLateUsMax, s_vsIntvN ? s_vsIntvSum / s_vsIntvN : 0, s_vsIntvMin,
                  s_vsIntvMax, s_vsToSwapN ? s_vsToSwapSum / s_vsToSwapN : 0);
    s_presentN = s_sameIntvN = 0;
    s_swapUsTotal = s_swapUsMax = s_gapUsTotal = s_gapUsMin = 0;
    s_vsyncWaitUsTotal = s_vsyncWaitUsMax = 0;
    s_gpuLateUsTotal = s_gpuLateUsMax = 0;
    s_gpuLateN = 0;
    s_vsIntvMin = s_vsIntvMax = s_vsIntvSum = s_vsToSwapSum = 0;
    s_vsIntvN = s_vsToSwapN = 0;
    s_presentWindow = t_done;
  }
}

int CWinSystemAmlogicGLESContext::GetBufferAge()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings =
      settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
  if (!advancedSettings || advancedSettings->m_guiBufferAgePartialRedraw < 1)
    return 0;
  if (GetGfxContext().GetStereoMode() != RENDER_STEREO_MODE_OFF)
    return 0;
  if (m_failedSwap)
  {
    m_failedSwap = false;
    logComponentM(LOGDEBUG, LOGWINDOWING, "GetBufferAge: full redraw after failed swap");
    return 0;
  }
  if (!m_pGLContext.HasBufferAgeSupport())
    return 0;
  const int age = m_pGLContext.GetBufferAge();
  if (age < 0 || age > 8)
    return 0;
  return age;
}

EGLDisplay CWinSystemAmlogicGLESContext::GetEGLDisplay() const
{
  return m_pGLContext.GetEGLDisplay();
}

EGLSurface CWinSystemAmlogicGLESContext::GetEGLSurface() const
{
  return m_pGLContext.GetEGLSurface();
}

EGLContext CWinSystemAmlogicGLESContext::GetEGLContext() const
{
  return m_pGLContext.GetEGLContext();
}

EGLConfig  CWinSystemAmlogicGLESContext::GetEGLConfig() const
{
  return m_pGLContext.GetEGLConfig();
}

std::unique_ptr<CVideoSync> CWinSystemAmlogicGLESContext::GetVideoSync(CVideoReferenceClock *clock)
{
  std::unique_ptr<CVideoSync> pVSync(new CVideoSyncAML(clock));
  return pVSync;
}

bool CWinSystemAmlogicGLESContext::SupportsStereo(RENDER_STEREO_MODE mode) const
{
  if (aml_display_support_3d() &&
      mode == RENDER_STEREO_MODE_HARDWAREBASED) {
    // yes, we support hardware based MVC decoding
    return true;
  }

  return CRenderSystemGLES::SupportsStereo(mode);
}
