/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/EGLUtils.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/GlobalsHandling.h"
#include "utils/StreamDetails.h"
#include "WinSystemAmlogic.h"

namespace KODI
{
namespace WINDOWING
{
namespace AML
{

class CWinSystemAmlogicGLESContext : public CWinSystemAmlogic, public CRenderSystemGLES
{
public:
  CWinSystemAmlogicGLESContext() = default;
  virtual ~CWinSystemAmlogicGLESContext() = default;

  using CWinSystemAmlogic::Register;
  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemAmlogic
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  bool SetDirtyRegions(const CDirtyRegionList& dirtyRegions) override
  {
    return m_pGLContext.SetDamagedRegions(dirtyRegions, m_height);
  }
  int GetBufferAge() override;

  bool BindTextureUploadContext() override;
  bool UnbindTextureUploadContext() override;
  bool CreateOverlayContext() override;
  bool BindOverlayContext() override;
  bool UnbindOverlayContext() override;
  void DestroyOverlayContext() override;
  bool HasContext() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  virtual std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock *clock) override;

  bool SupportsStereo(RENDER_STEREO_MODE mode) const override;

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig  GetEGLConfig() const;
protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override;

private:
  CEGLContextUtils m_pGLContext;
  StreamHdrType m_outputHdrType = StreamHdrType::HDR_TYPE_NONE;
  bool m_failedSwap{false};
  int m_vsyncFd{-1};
  bool m_vsyncAlignFailed{false};
};

}
}
}
