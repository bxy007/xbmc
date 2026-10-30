/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIWindowDebugInfo.h"

#include "CompileInfo.h"
#include "GUIInfoManager.h"
#include "ServiceBroker.h"
#include "addons/Skin.h"
#include "cores/DataCacheCore.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIControlFactory.h"
#include "guilib/GUIControlProfiler.h"
#include "guilib/GUIFontManager.h"
#include "guilib/GUITextLayout.h"
#include "guilib/GUIWindowManager.h"
#include "input/WindowTranslator.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/CPUInfo.h"
#include "utils/MemUtils.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"

#include <inttypes.h>

CGUIWindowDebugInfo::CGUIWindowDebugInfo(void)
  : CGUIDialog(WINDOW_DEBUG_INFO, "", DialogModalityType::MODELESS)
{
  m_needsScaling = false;
  m_layout = nullptr;
  m_renderOrder = RENDER_ORDER_WINDOW_DEBUG;
}

CGUIWindowDebugInfo::~CGUIWindowDebugInfo(void) = default;

void CGUIWindowDebugInfo::UpdateVisibility()
{
  if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_showOnScreenDebugInfo || g_SkinInfo->IsDebugging())
    Open();
  else
    Close();
}

bool CGUIWindowDebugInfo::OnMessage(CGUIMessage &message)
{
  if (message.GetMessage() == GUI_MSG_WINDOW_DEINIT)
  {
    delete m_layout;
    m_layout = nullptr;
  }
  else if (message.GetMessage() == GUI_MSG_REFRESH_TIMER)
    MarkDirtyRegion();

  return CGUIDialog::OnMessage(message);
}

void CGUIWindowDebugInfo::Process(unsigned int currentTime, CDirtyRegionList &dirtyregions)
{
  CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderingResolution(CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(), false);

  CServiceBroker::GetCPUInfo()->GetUsedPercentage(); // must call it to recalculate pct values

  static int yShift = 20;
  static int xShift = 40;
  static unsigned int lastShift = time(nullptr);
  time_t now = time(nullptr);
  if (now - lastShift > 10)
  {
    yShift *= -1;
    if (now % 5 == 0)
      xShift *= -1;
    lastShift = now;
    MarkDirtyRegion();
  }

  if (!m_layout)
  {
    CGUIFont *font13 = g_fontManager.GetDefaultFont();
    CGUIFont *font13border = g_fontManager.GetDefaultFont(true);
    if (font13)
      m_layout = new CGUITextLayout(font13, true, 0, font13border);
  }
  if (!m_layout)
    return;

  std::string info;
  if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_showOnScreenDebugInfo)
  {
    KODI::MEMORY::MemoryStatus stat;
    KODI::MEMORY::GetMemoryStatus(&stat);
    std::string profiling = CGUIControlProfiler::IsRunning() ? " (profiling)" : "";
    std::string strCores;
    if (CServiceBroker::GetCPUInfo()->SupportsCPUUsage())
      strCores = CServiceBroker::GetCPUInfo()->GetCoresUsageString();
    else
      strCores = "N/A";
    std::string lcAppName = CCompileInfo::GetAppName();
    StringUtils::ToLower(lcAppName);

    const float videoFps = CServiceBroker::GetDataCacheCore().GetVideoFps();
    const float guiFps = CServiceBroker::GetGUI()
                             ->GetInfoManager()
                             .GetInfoProviders()
                             .GetSystemInfoProvider()
                             .GetFPS();
    std::string fpsInfo;
    if (videoFps > 0.0f)
      fpsInfo = StringUtils::Format("FPS: {:.3f} RM: {:.3f}", videoFps, guiFps / videoFps);
    else
      fpsInfo = StringUtils::Format("FPS: {:.3f} (GUI) RM: -", guiFps);
#if !defined(TARGET_POSIX)
    info = StringUtils::Format("LOG: {}{}.log\nMEM: {}/{} KB - {}\nCPU: {}{}",
                               CSpecialProtocol::TranslatePath("special://logpath"), lcAppName,
                               stat.availPhys / 1024, stat.totalPhys / 1024,
                               fpsInfo,
                               strCores, profiling);
#else
    double dCPU = m_resourceCounter.GetCPUUsage();
    std::string ucAppName = lcAppName;
    StringUtils::ToUpper(ucAppName);
    info = StringUtils::Format("LOG: {}{}.log\n"
                               "MEM: {}/{} KB - {}\n"
                               "CPU: {} (CPU-{} {:4.2f}%{})",
                               CSpecialProtocol::TranslatePath("special://logpath"), lcAppName,
                               stat.availPhys / 1024, stat.totalPhys / 1024,
                               fpsInfo,
                               strCores, ucAppName, dCPU, profiling);
#endif
  }

  // render the skin debug info
  if (g_SkinInfo->IsDebugging())
  {
    if (!info.empty())
      info += "\n";
    CGUIWindow *window = CServiceBroker::GetGUI()->GetWindowManager().GetWindow(CServiceBroker::GetGUI()->GetWindowManager().GetActiveWindowOrDialog());
    CGUIWindow *pointer = CServiceBroker::GetGUI()->GetWindowManager().GetWindow(WINDOW_DIALOG_POINTER);
    CPoint point;
    if (pointer)
      point = CPoint(pointer->GetXPosition(), pointer->GetYPosition());
    if (window)
    {
      std::string windowName = CWindowTranslator::TranslateWindow(window->GetID());
      if (!windowName.empty())
        windowName += " (" + window->GetProperty("xmlfile").asString() + ")";
      else
        windowName = window->GetProperty("xmlfile").asString();
      info += "Window: " + windowName + "\n";
      // transform the mouse coordinates to this window's coordinates
      CServiceBroker::GetWinSystem()->GetGfxContext().SetScalingResolution(window->GetCoordsRes(), true);
      point.x *= CServiceBroker::GetWinSystem()->GetGfxContext().GetGUIScaleX();
      point.y *= CServiceBroker::GetWinSystem()->GetGfxContext().GetGUIScaleY();
      CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderingResolution(CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(), false);
    }
    info += StringUtils::Format("Mouse: ({},{})  ", static_cast<int>(point.x),
                                static_cast<int>(point.y));
    if (window)
    {
      CGUIControl *control = window->GetFocusedControl();
      if (control)
        info += StringUtils::Format(
            "Focused: {} ({})", control->GetID(),
            CGUIControlFactory::TranslateControlType(control->GetControlType()));
    }
  }

  float w, h;
  if (m_layout->Update(info))
    MarkDirtyRegion();
  m_layout->GetTextExtent(w, h);

  float x = xShift + 0.04f * CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth();
  float y = yShift + 0.04f * CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight();
  m_renderRegion.SetRect(x, y, x+w, y+h);
}

void CGUIWindowDebugInfo::Render()
{
  RENDER_ORDER renderOrder = CServiceBroker::GetWinSystem()->GetGfxContext().GetRenderOrder();
  if (renderOrder == RENDER_ORDER_FRONT_TO_BACK)
    return;
  else if (renderOrder == RENDER_ORDER_BACK_TO_FRONT)
    CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderOrder(RENDER_ORDER_ALL_BACK_TO_FRONT);
  CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderingResolution(CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(), false);
  if (m_layout)
    m_layout->RenderOutline(m_renderRegion.x1, m_renderRegion.y1, 0xffffffff, 0xff000000, 0, 0);
  CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderOrder(renderOrder);
}
