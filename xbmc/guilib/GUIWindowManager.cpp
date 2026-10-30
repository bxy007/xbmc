/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIWindowManager.h"

#include "GUIAudioManager.h"
#include "GUIDialog.h"
#include "GUIInfoManager.h"
#include "GUIPassword.h"
#include "GUITexture.h"
#include "ServiceBroker.h"
#include "WindowIDs.h"
#include "addons/Skin.h"
#include "addons/gui/GUIWindowAddonBrowser.h"
#include "addons/interfaces/gui/Window.h"
#include "application/Application.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "events/windows/GUIWindowEventLog.h"
#include "favourites/GUIWindowFavourites.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "messaging/helpers/DialogHelper.h"
#include "music/dialogs/GUIDialogInfoProviderSettings.h"
#include "music/dialogs/GUIDialogMusicInfo.h"
#include "music/windows/GUIWindowMusicNav.h"
#include "music/windows/GUIWindowMusicPlaylist.h"
#include "music/windows/GUIWindowMusicPlaylistEditor.h"
#include "music/windows/GUIWindowVisualisation.h"
#include "pictures/GUIWindowPictures.h"
#include "pictures/GUIWindowSlideShow.h"
#include "profiles/windows/GUIWindowSettingsProfile.h"
#include "programs/GUIWindowPrograms.h"
#include "rendering/RenderSystem.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "settings/windows/GUIWindowSettings.h"
#include "settings/windows/GUIWindowSettingsCategory.h"
#include "settings/windows/GUIWindowSettingsScreenCalibration.h"
#include "threads/Event.h"
#include "threads/SingleLock.h"
#include "threads/Thread.h"
#include "utils/AMLUtils.h"
#include "utils/ScopeGuard.h"
#include "utils/StringUtils.h"
#include "windowing/WinSystem.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/LogThrottle.h"
#include "utils/log.h"
#include "video/dialogs/GUIDialogVideoInfo.h"
#include "video/dialogs/GUIDialogVideoManagerExtras.h"
#include "video/dialogs/GUIDialogVideoManagerVersions.h"
#include "video/dialogs/GUIDialogVideoOSD.h"
#include "video/windows/GUIWindowFullScreen.h"
#include "video/windows/GUIWindowVideoNav.h"
#include "video/windows/GUIWindowVideoPlaylist.h"
#include "weather/GUIWindowWeather.h"
#include "windows/GUIWindowDebugInfo.h"
#include "windows/GUIWindowFileManager.h"
#include "windows/GUIWindowHome.h"
#include "windows/GUIWindowLoginScreen.h"
#include "windows/GUIWindowPointer.h"
#include "windows/GUIWindowScreensaver.h"
#include "windows/GUIWindowScreensaverDim.h"
#include "windows/GUIWindowSplash.h"
#include "windows/GUIWindowStartup.h"
#include "windows/GUIWindowSystemInfo.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>

// Dialog includes
#include "music/dialogs/GUIDialogMusicOSD.h"
#include "music/dialogs/GUIDialogVisualisationPresetList.h"
#include "dialogs/GUIDialogTextViewer.h"
#include "network/GUIDialogNetworkSetup.h"
#include "dialogs/GUIDialogMediaSource.h"
#if defined(HAS_GL) || defined(HAS_DX)
#include "video/dialogs/GUIDialogCMSSettings.h"
#endif
#include "addons/gui/GUIDialogAddonInfo.h"
#include "addons/gui/GUIDialogAddonSettings.h"
#include "dialogs/GUIDialogBusy.h"
#include "dialogs/GUIDialogBusyNoCancel.h"
#include "dialogs/GUIDialogButtonMenu.h"
#include "dialogs/GUIDialogColorPicker.h"
#include "dialogs/GUIDialogContextMenu.h"
#include "dialogs/GUIDialogExtendedProgressBar.h"
#include "dialogs/GUIDialogGamepad.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "dialogs/GUIDialogKeyboardGeneric.h"
#include "dialogs/GUIDialogKeyboardTouch.h"
#include "dialogs/GUIDialogNumeric.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogPlayerControls.h"
#include "dialogs/GUIDialogPlayerProcessInfo.h"
#include "dialogs/GUIDialogProgress.h"
#include "dialogs/GUIDialogSeekBar.h"
#include "dialogs/GUIDialogSelect.h"
#include "dialogs/GUIDialogSmartPlaylistEditor.h"
#include "dialogs/GUIDialogSmartPlaylistRule.h"
#include "dialogs/GUIDialogSubMenu.h"
#include "dialogs/GUIDialogVolumeBar.h"
#include "dialogs/GUIDialogYesNo.h"
#include "music/dialogs/GUIDialogSongInfo.h"
#include "pictures/GUIDialogPictureInfo.h"
#include "profiles/dialogs/GUIDialogLockSettings.h"
#include "profiles/dialogs/GUIDialogProfileSettings.h"
#include "settings/dialogs/GUIDialogContentSettings.h"
#include "settings/dialogs/GUIDialogLibExportSettings.h"
#include "video/dialogs/GUIDialogAudioSettings.h"
#include "video/dialogs/GUIDialogSubtitleSettings.h"
#include "video/dialogs/GUIDialogVideoBookmarks.h"
#include "video/dialogs/GUIDialogVideoSettings.h"

/* PVR related include Files */
#include "pvr/dialogs/GUIDialogPVRChannelGuide.h"
#include "pvr/dialogs/GUIDialogPVRChannelManager.h"
#include "pvr/dialogs/GUIDialogPVRChannelsOSD.h"
#include "pvr/dialogs/GUIDialogPVRClientPriorities.h"
#include "pvr/dialogs/GUIDialogPVRGroupManager.h"
#include "pvr/dialogs/GUIDialogPVRGuideControls.h"
#include "pvr/dialogs/GUIDialogPVRGuideInfo.h"
#include "pvr/dialogs/GUIDialogPVRGuideSearch.h"
#include "pvr/dialogs/GUIDialogPVRRadioRDSInfo.h"
#include "pvr/dialogs/GUIDialogPVRRecordingInfo.h"
#include "pvr/dialogs/GUIDialogPVRRecordingSettings.h"
#include "pvr/dialogs/GUIDialogPVRTimerSettings.h"
#include "pvr/windows/GUIWindowPVRChannels.h"
#include "pvr/windows/GUIWindowPVRGuide.h"
#include "pvr/windows/GUIWindowPVRRecordings.h"
#include "pvr/windows/GUIWindowPVRSearch.h"
#include "pvr/windows/GUIWindowPVRTimerRules.h"
#include "pvr/windows/GUIWindowPVRTimers.h"

#include "video/dialogs/GUIDialogTeletext.h"
#include "dialogs/GUIDialogSlider.h"
#ifdef HAS_OPTICAL_DRIVE
#include "dialogs/GUIDialogPlayEject.h"
#endif
#include "dialogs/GUIDialogMediaFilter.h"
#include "video/dialogs/GUIDialogSubtitles.h"

#include "peripherals/dialogs/GUIDialogPeripherals.h"
#include "peripherals/dialogs/GUIDialogPeripheralSettings.h"

/* Game related include files */
#include "cores/RetroPlayer/guiwindows/GameWindowFullScreen.h"
#include "games/agents/windows/GUIAgentWindow.h"
#include "games/controllers/windows/GUIControllerWindow.h"
#include "games/dialogs/osd/DialogGameAdvancedSettings.h"
#include "games/dialogs/osd/DialogGameOSD.h"
#include "games/dialogs/osd/DialogGameSaves.h"
#include "games/dialogs/osd/DialogGameStretchMode.h"
#include "games/dialogs/osd/DialogGameVideoFilter.h"
#include "games/dialogs/osd/DialogGameVideoRotation.h"
#include "games/dialogs/osd/DialogGameVolume.h"
#include "games/dialogs/osd/DialogInGameSaves.h"
#include "games/ports/windows/GUIPortWindow.h"
#include "games/windows/GUIWindowGames.h"

using namespace KODI;
using namespace PVR;
using namespace PERIPHERALS;

CGUIWindowManager::CGUIWindowManager()
{
  m_pCallback = nullptr;
  m_iNested = 0;
  m_initialized = false;
}

void CGUIWindowManager::Initialize()
{
  m_tracker.SelectAlgorithm();

  m_initialized = true;

  LoadNotOnDemandWindows();

  CServiceBroker::GetAppMessenger()->RegisterReceiver(this);
}

void CGUIWindowManager::CreateWindows()
{
  Add(new CGUIWindowHome);
  Add(new CGUIWindowPrograms);
  Add(new CGUIWindowPictures);
  Add(new CGUIWindowFileManager);
  Add(new CGUIWindowSettings);
  Add(new CGUIWindowSystemInfo);
  Add(new CGUIWindowSettingsScreenCalibration);
  Add(new CGUIWindowSettingsCategory);
  Add(new CGUIWindowVideoNav);
  Add(new CGUIWindowVideoPlaylist);
  Add(new CGUIWindowLoginScreen);
  Add(new CGUIWindowSettingsProfile);
  Add(new CGUIWindow(WINDOW_SKIN_SETTINGS, "SkinSettings.xml"));
  Add(new CGUIWindowAddonBrowser);
  Add(new CGUIWindowScreensaverDim);
  Add(new CGUIWindowDebugInfo);
  Add(new CGUIWindowPointer);
  Add(new CGUIDialogYesNo);
  Add(new CGUIDialogProgress);
  Add(new CGUIDialogExtendedProgressBar);
  Add(new CGUIDialogKeyboardGeneric);
  Add(new CGUIDialogKeyboardTouch);
  Add(new CGUIDialogVolumeBar);
  Add(new CGUIDialogSeekBar);
  Add(new CGUIDialogSubMenu);
  Add(new CGUIDialogContextMenu);
  Add(new CGUIDialogKaiToast);
  Add(new CGUIDialogNumeric);
  Add(new CGUIDialogGamepad);
  Add(new CGUIDialogButtonMenu);
  Add(new CGUIDialogPlayerControls);
  Add(new CGUIDialogPlayerProcessInfo);
  Add(new CGUIDialogSlider);
  Add(new CGUIDialogMusicOSD);
  Add(new CGUIDialogVisualisationPresetList);
#if defined(HAS_GL) || defined(HAS_DX)
  Add(new CGUIDialogCMSSettings);
#endif
  Add(new CGUIDialogVideoSettings);
  Add(new CGUIDialogAudioSettings);
  Add(new CGUIDialogSubtitleSettings);
  Add(new CGUIDialogVideoBookmarks);
  // Don't add the filebrowser dialog - it's created and added when it's needed
  Add(new CGUIDialogNetworkSetup);
  Add(new CGUIDialogMediaSource);
  Add(new CGUIDialogProfileSettings);
  Add(new CGUIDialogSongInfo);
  Add(new CGUIDialogSmartPlaylistEditor);
  Add(new CGUIDialogSmartPlaylistRule);
  Add(new CGUIDialogBusy);
  Add(new CGUIDialogBusyNoCancel);
  Add(new CGUIDialogPictureInfo);
  Add(new CGUIDialogAddonInfo);
  Add(new CGUIDialogAddonSettings);

  Add(new CGUIDialogLockSettings);

  Add(new CGUIDialogContentSettings);

  Add(new CGUIDialogLibExportSettings);

  Add(new CGUIDialogInfoProviderSettings);

#ifdef HAS_OPTICAL_DRIVE
  Add(new CGUIDialogPlayEject);
#endif

  Add(new CGUIDialogPeripherals);
  Add(new CGUIDialogPeripheralSettings);

  Add(new CGUIDialogMediaFilter);
  Add(new CGUIDialogSubtitles);

  Add(new CGUIWindowMusicPlayList);
  Add(new CGUIWindowMusicNav);
  Add(new CGUIWindowMusicPlaylistEditor);

  /* Load PVR related Windows and Dialogs */
  Add(new CGUIDialogTeletext);
  Add(new CGUIWindowPVRTVChannels);
  Add(new CGUIWindowPVRTVRecordings);
  Add(new CGUIWindowPVRTVGuide);
  Add(new CGUIWindowPVRTVTimers);
  Add(new CGUIWindowPVRTVTimerRules);
  Add(new CGUIWindowPVRTVSearch);
  Add(new CGUIWindowPVRRadioChannels);
  Add(new CGUIWindowPVRRadioRecordings);
  Add(new CGUIWindowPVRRadioGuide);
  Add(new CGUIWindowPVRRadioTimers);
  Add(new CGUIWindowPVRRadioTimerRules);
  Add(new CGUIWindowPVRRadioSearch);
  Add(new CGUIDialogPVRRadioRDSInfo);
  Add(new CGUIDialogPVRGuideInfo);
  Add(new CGUIDialogPVRRecordingInfo);
  Add(new CGUIDialogPVRTimerSettings);
  Add(new CGUIDialogPVRGroupManager);
  Add(new CGUIDialogPVRChannelManager);
  Add(new CGUIDialogPVRGuideSearch);
  Add(new CGUIDialogPVRChannelsOSD);
  Add(new CGUIDialogPVRChannelGuide);
  Add(new CGUIDialogPVRRecordingSettings);
  Add(new CGUIDialogPVRClientPriorities);
  Add(new CGUIDialogPVRGuideControls);

  Add(new CGUIDialogSelect);
  Add(new CGUIDialogColorPicker);
  Add(new CGUIDialogMusicInfo);
  Add(new CGUIDialogOK);
  Add(new CGUIDialogVideoInfo);
  Add(new CGUIDialogVideoManagerVersions);
  Add(new CGUIDialogVideoManagerExtras);
  Add(new CGUIDialogSelect(WINDOW_DIALOG_SELECT_VIDEO_VERSION));
  Add(new CGUIDialogSelect(WINDOW_DIALOG_SELECT_VIDEO_EXTRA));
  Add(new CGUIDialogSelect(WINDOW_DIALOG_SELECT_VIDEO_STREAM));
  Add(new CGUIDialogSelect(WINDOW_DIALOG_SELECT_AUDIO_STREAM));
  Add(new CGUIDialogSelect(WINDOW_DIALOG_SELECT_SUBTITLE_STREAM));

  Add(new CGUIDialogTextViewer);
  Add(new CGUIWindowFullScreen);
  Add(new CGUIWindowVisualisation);
  Add(new CGUIWindowSlideShow);

  Add(new CGUIDialogVideoOSD);
  Add(new CGUIWindowScreensaver);
  Add(new CGUIWindowWeather);
  Add(new CGUIWindowStartup);
  Add(new CGUIWindowSplash);

  Add(new CGUIWindowEventLog);

  Add(new CGUIWindowFavourites);

  Add(new GAME::CGUIControllerWindow);
  Add(new GAME::CGUIPortWindow);
  Add(new GAME::CGUIWindowGames);
  Add(new GAME::CDialogGameOSD);
  Add(new GAME::CDialogGameSaves);
  Add(new GAME::CDialogGameVideoFilter);
  Add(new GAME::CDialogGameStretchMode);
  Add(new GAME::CDialogGameVolume);
  Add(new GAME::CDialogGameAdvancedSettings);
  Add(new GAME::CDialogGameVideoRotation);
  Add(new GAME::CDialogInGameSaves);
  Add(new GAME::CGUIAgentWindow);
  Add(new RETRO::CGameWindowFullScreen);
}

bool CGUIWindowManager::DestroyWindows()
{
  try
  {
    DestroyWindow(WINDOW_SPLASH);
    DestroyWindow(WINDOW_MUSIC_PLAYLIST);
    DestroyWindow(WINDOW_MUSIC_PLAYLIST_EDITOR);
    DestroyWindow(WINDOW_MUSIC_NAV);
    DestroyWindow(WINDOW_DIALOG_MUSIC_INFO);
    DestroyWindow(WINDOW_DIALOG_VIDEO_INFO);
    DestroyWindow(WINDOW_DIALOG_SELECT_VIDEO_EXTRA);
    DestroyWindow(WINDOW_DIALOG_SELECT_VIDEO_VERSION);
    DestroyWindow(WINDOW_DIALOG_MANAGE_VIDEO_EXTRAS);
    DestroyWindow(WINDOW_DIALOG_MANAGE_VIDEO_VERSIONS);
    DestroyWindow(WINDOW_VIDEO_PLAYLIST);
    DestroyWindow(WINDOW_VIDEO_NAV);
    DestroyWindow(WINDOW_FILES);
    DestroyWindow(WINDOW_DIALOG_YES_NO);
    DestroyWindow(WINDOW_DIALOG_PROGRESS);
    DestroyWindow(WINDOW_DIALOG_NUMERIC);
    DestroyWindow(WINDOW_DIALOG_GAMEPAD);
    DestroyWindow(WINDOW_DIALOG_SUB_MENU);
    DestroyWindow(WINDOW_DIALOG_BUTTON_MENU);
    DestroyWindow(WINDOW_DIALOG_CONTEXT_MENU);
    DestroyWindow(WINDOW_DIALOG_PLAYER_CONTROLS);
    DestroyWindow(WINDOW_DIALOG_PLAYER_PROCESS_INFO);
    DestroyWindow(WINDOW_DIALOG_MUSIC_OSD);
    DestroyWindow(WINDOW_DIALOG_VIS_PRESET_LIST);
    DestroyWindow(WINDOW_DIALOG_SELECT);
    DestroyWindow(WINDOW_DIALOG_OK);
    DestroyWindow(WINDOW_DIALOG_KEYBOARD);
    DestroyWindow(WINDOW_DIALOG_KEYBOARD_TOUCH);
    DestroyWindow(WINDOW_FULLSCREEN_VIDEO);
    DestroyWindow(WINDOW_DIALOG_PROFILE_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_LOCK_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_NETWORK_SETUP);
    DestroyWindow(WINDOW_DIALOG_MEDIA_SOURCE);
    DestroyWindow(WINDOW_DIALOG_CMS_OSD_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_VIDEO_OSD_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_AUDIO_OSD_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_SUBTITLE_OSD_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_VIDEO_BOOKMARKS);
    DestroyWindow(WINDOW_DIALOG_CONTENT_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_INFOPROVIDER_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_LIBEXPORT_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_SONG_INFO);
    DestroyWindow(WINDOW_DIALOG_SMART_PLAYLIST_EDITOR);
    DestroyWindow(WINDOW_DIALOG_SMART_PLAYLIST_RULE);
    DestroyWindow(WINDOW_DIALOG_BUSY);
    DestroyWindow(WINDOW_DIALOG_BUSY_NOCANCEL);
    DestroyWindow(WINDOW_DIALOG_PICTURE_INFO);
    DestroyWindow(WINDOW_DIALOG_ADDON_INFO);
    DestroyWindow(WINDOW_DIALOG_ADDON_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_SLIDER);
    DestroyWindow(WINDOW_DIALOG_MEDIA_FILTER);
    DestroyWindow(WINDOW_DIALOG_SUBTITLES);
    DestroyWindow(WINDOW_DIALOG_SELECT_VIDEO_STREAM);
    DestroyWindow(WINDOW_DIALOG_SELECT_AUDIO_STREAM);
    DestroyWindow(WINDOW_DIALOG_SELECT_SUBTITLE_STREAM);
    DestroyWindow(WINDOW_DIALOG_COLOR_PICKER);

    /* Delete PVR related windows and dialogs */
    DestroyWindow(WINDOW_TV_CHANNELS);
    DestroyWindow(WINDOW_TV_RECORDINGS);
    DestroyWindow(WINDOW_TV_GUIDE);
    DestroyWindow(WINDOW_TV_TIMERS);
    DestroyWindow(WINDOW_TV_TIMER_RULES);
    DestroyWindow(WINDOW_TV_SEARCH);
    DestroyWindow(WINDOW_RADIO_CHANNELS);
    DestroyWindow(WINDOW_RADIO_RECORDINGS);
    DestroyWindow(WINDOW_RADIO_GUIDE);
    DestroyWindow(WINDOW_RADIO_TIMERS);
    DestroyWindow(WINDOW_RADIO_TIMER_RULES);
    DestroyWindow(WINDOW_RADIO_SEARCH);
    DestroyWindow(WINDOW_DIALOG_PVR_GUIDE_INFO);
    DestroyWindow(WINDOW_DIALOG_PVR_RECORDING_INFO);
    DestroyWindow(WINDOW_DIALOG_PVR_TIMER_SETTING);
    DestroyWindow(WINDOW_DIALOG_PVR_GROUP_MANAGER);
    DestroyWindow(WINDOW_DIALOG_PVR_CHANNEL_MANAGER);
    DestroyWindow(WINDOW_DIALOG_PVR_GUIDE_SEARCH);
    DestroyWindow(WINDOW_DIALOG_PVR_CHANNEL_SCAN);
    DestroyWindow(WINDOW_DIALOG_PVR_RADIO_RDS_INFO);
    DestroyWindow(WINDOW_DIALOG_PVR_UPDATE_PROGRESS);
    DestroyWindow(WINDOW_DIALOG_PVR_OSD_CHANNELS);
    DestroyWindow(WINDOW_DIALOG_PVR_CHANNEL_GUIDE);
    DestroyWindow(WINDOW_DIALOG_OSD_TELETEXT);
    DestroyWindow(WINDOW_DIALOG_PVR_RECORDING_SETTING);
    DestroyWindow(WINDOW_DIALOG_PVR_CLIENT_PRIORITIES);
    DestroyWindow(WINDOW_DIALOG_PVR_GUIDE_CONTROLS);

    DestroyWindow(WINDOW_DIALOG_TEXT_VIEWER);
#ifdef HAS_OPTICAL_DRIVE
    DestroyWindow(WINDOW_DIALOG_PLAY_EJECT);
#endif
    DestroyWindow(WINDOW_STARTUP_ANIM);
    DestroyWindow(WINDOW_LOGIN_SCREEN);
    DestroyWindow(WINDOW_VISUALISATION);
    DestroyWindow(WINDOW_SETTINGS_MENU);
    DestroyWindow(WINDOW_SETTINGS_PROFILES);
    DestroyWindow(WINDOW_SCREEN_CALIBRATION);
    DestroyWindow(WINDOW_SYSTEM_INFORMATION);
    DestroyWindow(WINDOW_SCREENSAVER);
    DestroyWindow(WINDOW_DIALOG_VIDEO_OSD);
    DestroyWindow(WINDOW_SLIDESHOW);
    DestroyWindow(WINDOW_ADDON_BROWSER);
    DestroyWindow(WINDOW_SKIN_SETTINGS);

    DestroyWindow(WINDOW_HOME);
    DestroyWindow(WINDOW_PROGRAMS);
    DestroyWindow(WINDOW_PICTURES);
    DestroyWindow(WINDOW_WEATHER);
    DestroyWindow(WINDOW_DIALOG_GAME_CONTROLLERS);
    DestroyWindow(WINDOW_DIALOG_GAME_PORTS);
    DestroyWindow(WINDOW_GAMES);
    DestroyWindow(WINDOW_DIALOG_GAME_OSD);
    DestroyWindow(WINDOW_DIALOG_GAME_SAVES);
    DestroyWindow(WINDOW_DIALOG_GAME_VIDEO_FILTER);
    DestroyWindow(WINDOW_DIALOG_GAME_STRETCH_MODE);
    DestroyWindow(WINDOW_DIALOG_GAME_VOLUME);
    DestroyWindow(WINDOW_DIALOG_GAME_ADVANCED_SETTINGS);
    DestroyWindow(WINDOW_DIALOG_GAME_VIDEO_ROTATION);
    DestroyWindow(WINDOW_DIALOG_IN_GAME_SAVES);
    DestroyWindow(WINDOW_DIALOG_GAME_AGENTS);
    DestroyWindow(WINDOW_FULLSCREEN_GAME);

    Remove(WINDOW_SETTINGS_SERVICE);
    Remove(WINDOW_SETTINGS_MYPVR);
    Remove(WINDOW_SETTINGS_PLAYER);
    Remove(WINDOW_SETTINGS_MEDIA);
    Remove(WINDOW_SETTINGS_INTERFACE);
    Remove(WINDOW_SETTINGS_MYGAMES);
    DestroyWindow(WINDOW_SETTINGS_SYSTEM);  // all the settings categories

    Remove(WINDOW_DIALOG_KAI_TOAST);
    Remove(WINDOW_DIALOG_SEEK_BAR);
    Remove(WINDOW_DIALOG_VOLUME_BAR);

    DestroyWindow(WINDOW_EVENT_LOG);

    DestroyWindow(WINDOW_FAVOURITES);

    DestroyWindow(WINDOW_DIALOG_PERIPHERALS);
    DestroyWindow(WINDOW_DIALOG_PERIPHERAL_SETTINGS);
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "Exception in CGUIWindowManager::DestroyWindows()");
    return false;
  }

  return true;
}

void CGUIWindowManager::DestroyWindow(int id)
{
  std::unique_lock<CCriticalSection> lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  auto it = m_mapWindows.find(id);
  if (it != m_mapWindows.end())
  {
    std::shared_ptr<CGUIWindow> pWindow = it->second;
    Remove(id);
    pWindow->FreeResources(true);
  }
}

bool CGUIWindowManager::SendMessage(int message, int senderID, int destID, int param1, int param2)
{
  CGUIMessage msg(message, senderID, destID, param1, param2);
  return SendMessage(msg);
}

bool CGUIWindowManager::SendMessage(CGUIMessage& message)
{
  bool handled = false;
  //  CLog::Log(LOGDEBUG,"SendMessage: mess={} send={} control={} param1={}", message.GetMessage(), message.GetSenderId(), message.GetControlId(), message.GetParam1());
  // Send the message to all none window targets
  for (int i = 0; i < int(m_vecMsgTargets.size()); i++)
  {
    IMsgTargetCallback* pMsgTarget = m_vecMsgTargets[i];

    if (pMsgTarget)
    {
      if (pMsgTarget->OnMessage( message )) handled = true;
    }
  }

  //  A GUI_MSG_NOTIFY_ALL is send to any active modal dialog
  //  and all windows whether they are active or not
  if (message.GetMessage()==GUI_MSG_NOTIFY_ALL)
  {
    std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

    // make copy of vector as OnMessage may modify m_activeDialogs (e.g., via DeInit)
    auto activeDialogs = m_activeDialogs;
    for (auto it = activeDialogs.rbegin(); it != activeDialogs.rend(); ++it)
    {
      (*it)->OnMessage(message);
    }

    for (const auto& entry : m_mapWindows)
    {
      entry.second->OnMessage(message);
    }

    return true;
  }

  // Normal messages are sent to:
  // 1. All active modeless dialogs
  // 2. The topmost dialog that accepts the message
  // 3. The underlying window (only if it is the sender or receiver if a modal dialog is active)

  bool hasModalDialog(false);
  bool modalAcceptedMessage(false);
  // don't use an iterator for this loop, as some messages mean that m_activeDialogs is altered,
  // which will invalidate any iterator
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  size_t topWindow = m_activeDialogs.size();
  while (topWindow)
  {
    auto dialog = m_activeDialogs[--topWindow];
    if (!modalAcceptedMessage && dialog->IsModalDialog())
    { // modal window
      hasModalDialog = true;
      if (!modalAcceptedMessage && dialog->OnMessage( message ))
      {
        modalAcceptedMessage = handled = true;
      }
    }
    else if (!dialog->IsModalDialog())
    { // modeless
      if (dialog->OnMessage( message ))
        handled = true;
    }

    if (topWindow > m_activeDialogs.size())
      topWindow = m_activeDialogs.size();
  }

  // now send to the underlying window
  CGUIWindow* window = GetWindow(GetActiveWindow());
  if (window)
  {
    if (hasModalDialog)
    {
      // only send the message to the underlying window if it's the recipient
      // or sender (or we have no sender)
      if (message.GetSenderId() == window->GetID() ||
          message.GetControlId() == window->GetID() ||
          message.GetSenderId() == 0 )
      {
        if (window->OnMessage(message)) handled = true;
      }
    }
    else
    {
      if (window->OnMessage(message)) handled = true;
    }
  }
  return handled;
}

bool CGUIWindowManager::SendMessage(CGUIMessage& message, int window)
{
  if (window == 0)
    // send to no specified windows.
    return SendMessage(message);
  CGUIWindow* pWindow = GetWindow(window);
  if(pWindow)
    return pWindow->OnMessage(message);
  else
    return false;
}

void CGUIWindowManager::AddUniqueInstance(CGUIWindow *window)
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  // increment our instance (upper word of windowID)
  // until we get a window we don't have
  int instance = 0;
  while (GetWindow(window->GetID()))
    window->SetID(window->GetID() + (++instance << 16));
  Add(window);
}

bool CGUIWindowManager::Add(CGUIWindow* pWindow)
{
  if (!pWindow)
  {
    CLog::Log(LOGERROR, "Attempted to add a NULL window pointer to the window manager.");
    return false;
  }
  // push back all the windows if there are more than one covered by this class
  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  std::shared_ptr<CGUIWindow> windowPtr(pWindow);

  for (int id : pWindow->GetIDRange())
  {
    auto it = m_mapWindows.find(id);
    if (it != m_mapWindows.end())
    {
      CLog::Log(LOGERROR,
                "Error, trying to add a second window with id {} "
                "to the window manager",
                id);
      return false;
    }

    m_mapWindows.insert(std::make_pair(id, windowPtr));
  }

  // Keep a dedicated list of dialog-like windows for per-frame processing.
  // Note: Python WindowXMLDialog instances live in the WINDOW_PYTHON_* range but
  // may not report IsDialog()==true (they are mapped to CGUIMediaWindow).
  if (pWindow->IsDialog() || IsPythonWindow(pWindow->GetID()))
    m_dialogWindows.emplace_back(windowPtr);

  return true;
}

void CGUIWindowManager::AddCustomWindow(CGUIWindow* pWindow)
{
  if (!pWindow)
    return;

  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  int windowId = pWindow->GetID(); // Get ID before Add() takes ownership
  if (!Add(pWindow))
    return; // Add() failed, window was deleted

  auto it = m_mapWindows.find(windowId);
  if (it != m_mapWindows.end())
    m_vecCustomWindows.emplace_back(it->second);
}

void CGUIWindowManager::RegisterDialog(CGUIWindow* dialog)
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  // only add the window if it does not exists
  for (const auto& window : m_activeDialogs)
  {
    if (window->GetID() == dialog->GetID())
      return;
  }
  auto it = m_mapWindows.find(dialog->GetID());
  if (it != m_mapWindows.end())
  {
    m_activeDialogs.emplace_back(it->second);
  }
}

void CGUIWindowManager::Remove(int id)
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  auto it = m_mapWindows.find(id);
  if (it != m_mapWindows.end())
  {
    CGUIWindow* window = it->second.get();
    m_windowHistory.erase(std::remove_if(m_windowHistory.begin(),
                                         m_windowHistory.end(),
                                         [id](int winId){ return winId == id; }),
                          m_windowHistory.end());
    m_activeDialogs.erase(std::remove_if(m_activeDialogs.begin(), m_activeDialogs.end(),
                                         [window](const std::shared_ptr<CGUIWindow>& w)
                                         { return w.get() == window; }),
                          m_activeDialogs.end());

    m_dialogWindows.erase(std::remove_if(m_dialogWindows.begin(), m_dialogWindows.end(),
                                         [window](const std::shared_ptr<CGUIWindow>& w)
                                         { return w.get() == window; }),
                          m_dialogWindows.end());

    m_mapWindows.erase(it);
  }
  else
  {
    CLog::Log(LOGWARNING,
              "Attempted to remove window {} "
              "from the window manager when it didn't exist",
              id);
  }
}

// Removes and deletes the window. Should only be called
// from the class that created the window and transferred
// ownership to CGUIWindowManager via Add.
void CGUIWindowManager::Delete(int id)
{
  std::unique_lock<CCriticalSection> lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  auto it = m_mapWindows.find(id);
  if (it != m_mapWindows.end())
  {
    std::shared_ptr<CGUIWindow> pWindow = it->second;
    Remove(id);
    m_deleteWindows.emplace_back(std::move(pWindow));
  }
}

void CGUIWindowManager::PreviousWindow()
{
  // deactivate any window
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  CLog::Log(LOGDEBUG,"CGUIWindowManager::PreviousWindow: Deactivate");
  int currentWindow = GetActiveWindow();
  CGUIWindow *pCurrentWindow = GetWindow(currentWindow);
  if (!pCurrentWindow)
    return;     // no windows or window history yet

  // check to see whether our current window has a <previouswindow> tag
  if (pCurrentWindow->GetPreviousWindow() != WINDOW_INVALID)
  {
    //! @todo we may need to test here for the
    //!       whether our history should be changed

    // don't reactivate the previouswindow if it is ourselves.
    if (currentWindow != pCurrentWindow->GetPreviousWindow())
      ActivateWindow(pCurrentWindow->GetPreviousWindow());
    return;
  }
  // get the previous window in our stack
  if (m_windowHistory.size() < 2)
  {
    // no previous window history yet - check if we should just activate home
    if (GetActiveWindow() != WINDOW_INVALID && GetActiveWindow() != WINDOW_HOME)
    {
      CloseWindowSync(pCurrentWindow);
      ClearWindowHistory();
      ActivateWindow(WINDOW_HOME);
    }
    return;
  }
  m_windowHistory.pop_back();
  int previousWindow = GetActiveWindow();
  m_windowHistory.emplace_back(currentWindow);

  CGUIWindow *pNewWindow = GetWindow(previousWindow);
  if (!pNewWindow)
  {
    CLog::Log(LOGERROR, "Unable to activate the previous window");
    CloseWindowSync(pCurrentWindow);
    ClearWindowHistory();
    ActivateWindow(WINDOW_HOME);
    return;
  }

  // ok to go to the previous window now

  // tell our info manager which window we are going to
  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetNextWindow(previousWindow);

  // deinitialize our window
  CloseWindowSync(pCurrentWindow);

  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetNextWindow(WINDOW_INVALID);
  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetPreviousWindow(currentWindow);

  // remove the current window off our window stack
  m_windowHistory.pop_back();

  // ok, initialize the new window
  CLog::Log(LOGDEBUG,"CGUIWindowManager::PreviousWindow: Activate new");
  CGUIMessage msg2(GUI_MSG_WINDOW_INIT, 0, 0, WINDOW_INVALID, GetActiveWindow());
  pNewWindow->OnMessage(msg2);

  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetPreviousWindow(WINDOW_INVALID);
}

void CGUIWindowManager::ChangeActiveWindow(int newWindow, const std::string& strPath)
{
  std::vector<std::string> params;
  if (!strPath.empty())
    params.emplace_back(strPath);
  ActivateWindow(newWindow, params, true);
}

void CGUIWindowManager::ActivateWindow(int iWindowID, const std::string& strPath)
{
  std::vector<std::string> params;
  if (!strPath.empty())
    params.emplace_back(strPath);
  ActivateWindow(iWindowID, params, false);
}

void CGUIWindowManager::ForceActivateWindow(int iWindowID, const std::string& strPath)
{
  std::vector<std::string> params;
  if (!strPath.empty())
    params.emplace_back(strPath);
  ActivateWindow(iWindowID, params, false, true);
}

void CGUIWindowManager::ActivateWindow(int iWindowID, const std::vector<std::string>& params, bool swappingWindows /* = false */, bool force /* = false */)
{
  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    // make sure graphics lock is not held
    CSingleExit leaveIt(CServiceBroker::GetWinSystem()->GetGfxContext());
    CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_ACTIVATE_WINDOW, iWindowID,
                                               swappingWindows ? 1 : 0, nullptr, "", params);
  }
  else
  {
    std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

    ActivateWindow_Internal(iWindowID, params, swappingWindows, force);
  }
}

void CGUIWindowManager::ActivateWindow_Internal(int iWindowID, const std::vector<std::string>& params, bool swappingWindows, bool force /* = false */)
{
  // translate virtual windows
  if (iWindowID == WINDOW_START)
  { // virtual start window
    iWindowID = g_SkinInfo->GetStartWindow();
  }

  // debug
  CLog::Log(LOGDEBUG, "Activating window ID: {}", iWindowID);

  // make sure we check mediasources from home
  if (GetActiveWindow() == WINDOW_HOME)
  {
    g_passwordManager.SetMediaSourcePath(!params.empty() ? params[0] : "");
  }
  else
  {
    g_passwordManager.SetMediaSourcePath("");
  }

  if (!g_passwordManager.CheckMenuLock(iWindowID))
  {
    CLog::Log(LOGERROR,
              "MasterCode or MediaSource-code is wrong: Window with id {} will not be loaded! "
              "Enter a correct code!",
              iWindowID);
    if (GetActiveWindow() == WINDOW_INVALID && iWindowID != WINDOW_HOME)
      ActivateWindow(WINDOW_HOME);
    return;
  }

  // first check existence of the window we wish to activate.
  CGUIWindow *pNewWindow = GetWindow(iWindowID);
  if (!pNewWindow)
  { // nothing to see here - move along
    CLog::Log(LOGERROR, "Unable to locate window with id {}.  Check skin files",
              iWindowID - WINDOW_HOME);
    if (IsWindowActive(WINDOW_STARTUP_ANIM))
      ActivateWindow(WINDOW_HOME);
    return ;
  }
  else if (!pNewWindow->CanBeActivated())
  {
    if (IsWindowActive(WINDOW_STARTUP_ANIM))
      ActivateWindow(WINDOW_HOME);
    return;
  }
  else if (pNewWindow->IsDialog())
  { // if we have a dialog, we do a DoModal() rather than activate the window
    if (!pNewWindow->IsDialogRunning())
    {
      CSingleExit exitit(CServiceBroker::GetWinSystem()->GetGfxContext());
      static_cast<CGUIDialog *>(pNewWindow)->Open(params.size() > 0 ? params[0] : "");
      // Invalidate underlying windows after closing a modal dialog
      MarkDirty();
    }
    return;
  }

  // don't activate a window if there are active modal dialogs of type MODAL
  if (!force && HasModalDialog(true))
  {
    CLog::Log(LOGINFO, "Activate of window '{}' refused because there are active modal dialogs",
              iWindowID);
    CServiceBroker::GetGUI()->GetAudioManager().PlayActionSound(CAction(ACTION_ERROR));
    return;
  }

  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetNextWindow(iWindowID);

  // deactivate any window
  int currentWindow = GetActiveWindow();
  CGUIWindow *pWindow = GetWindow(currentWindow);
  if (pWindow)
  {
    if (iWindowID == WINDOW_SCREENSAVER)
    {
      pWindow->Close(true, iWindowID);
    }
    else
    {
      CloseWindowSync(pWindow, iWindowID);
    }
  }
  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetNextWindow(WINDOW_INVALID);

  // Add window to the history list (we must do this before we activate it,
  // as all messages done in WINDOW_INIT will want to be sent to the new
  // topmost window).  If we are swapping windows, we pop the old window
  // off the history stack
  if (swappingWindows && !m_windowHistory.empty())
    m_windowHistory.pop_back();
  AddToWindowHistory(iWindowID);

  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetPreviousWindow(currentWindow);
  // Send the init message
  CGUIMessage msg(GUI_MSG_WINDOW_INIT, 0, 0, currentWindow, iWindowID);
  msg.SetStringParams(params);
  pNewWindow->OnMessage(msg);
//  CServiceBroker::GetGUI()->GetInfoManager().GetInfoProviders().GetGUIControlsInfoProvider().SetPreviousWindow(WINDOW_INVALID);
}

void CGUIWindowManager::CloseDialogs(bool forceClose) const
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  //This is to avoid an assert about out of bounds iterator
  //when m_activeDialogs happens to be empty
  if (m_activeDialogs.empty())
    return;

  auto activeDialogs = m_activeDialogs;
  for (const auto& window : activeDialogs)
  {
    if (window->IsModalDialog())
      window->Close(forceClose);
  }
}

void CGUIWindowManager::CloseInternalModalDialogs(bool forceClose) const
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  if (m_activeDialogs.empty())
    return;

  auto activeDialogs = m_activeDialogs;
  for (const auto& window : activeDialogs)
  {
    if (window->IsModalDialog() && !IsAddonWindow(window->GetID()) && !IsPythonWindow(window->GetID()))
      window->Close(forceClose);
  }
}

// SwitchToFullScreen() returns true if a switch is made, else returns false
bool CGUIWindowManager::SwitchToFullScreen(bool force /* = false */)
{
  // don't switch if the slideshow is active
  if (IsWindowActive(WINDOW_SLIDESHOW))
    return false;

  // if playing from the video info window, close it first!
  if (IsModalDialogTopmost(WINDOW_DIALOG_VIDEO_INFO))
  {
    CGUIDialogVideoInfo* pDialog = GetWindow<CGUIDialogVideoInfo>(WINDOW_DIALOG_VIDEO_INFO);
    if (pDialog)
      pDialog->Close(true);
  }

  // if playing from the album info window, close it first!
  if (IsModalDialogTopmost(WINDOW_DIALOG_MUSIC_INFO))
  {
    CGUIDialogVideoInfo* pDialog = GetWindow<CGUIDialogVideoInfo>(WINDOW_DIALOG_MUSIC_INFO);
    if (pDialog)
      pDialog->Close(true);
  }

  // if playing from the song info window, close it first!
  if (IsModalDialogTopmost(WINDOW_DIALOG_SONG_INFO))
  {
    CGUIDialogVideoInfo* pDialog = GetWindow<CGUIDialogVideoInfo>(WINDOW_DIALOG_SONG_INFO);
    if (pDialog)
      pDialog->Close(true);
  }

  const int activeWindowID = GetActiveWindow();
  int windowID = WINDOW_INVALID;

  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();

  // See if we're playing a game
  if (activeWindowID != WINDOW_FULLSCREEN_GAME && appPlayer->IsPlayingGame())
    windowID = WINDOW_FULLSCREEN_GAME;

  // See if we're playing a video
  else if (activeWindowID != WINDOW_FULLSCREEN_VIDEO && appPlayer->IsPlayingVideo())
    windowID = WINDOW_FULLSCREEN_VIDEO;

  // See if we're playing an audio song
  if (activeWindowID != WINDOW_VISUALISATION && appPlayer->IsPlayingAudio())
    windowID = WINDOW_VISUALISATION;

  if (windowID != WINDOW_INVALID && (force || windowID != activeWindowID))
  {
    aml_reset_audio_from_window_home();
    if (force)
      ForceActivateWindow(windowID);
    else
      ActivateWindow(windowID);

    return true;
  }

  return false;
}

void CGUIWindowManager::OnApplicationMessage(ThreadMessage* pMsg)
{
  switch (pMsg->dwMessage)
  {
  case TMSG_GUI_DIALOG_OPEN:
  {
    if (pMsg->lpVoid)
      static_cast<CGUIDialog*>(pMsg->lpVoid)->Open(pMsg->param2, pMsg->strParam);
    else
    {
      auto pDialog = static_cast<CGUIDialog*>(GetWindow(pMsg->param1));
      if (pDialog)
        pDialog->Open(pMsg->strParam);
    }
  }
  break;

  case TMSG_GUI_WINDOW_CLOSE:
  {
    auto window = static_cast<CGUIWindow *>(pMsg->lpVoid);
    if (window)
      window->Close((pMsg->param1 & 0x1) ? true : false, pMsg->param1, (pMsg->param1 & 0x2) ? true : false);
  }
  break;

  case TMSG_GUI_ACTIVATE_WINDOW:
  {
    ActivateWindow(pMsg->param1, pMsg->params, pMsg->param2 > 0);
  }
  break;

  case TMSG_GUI_PREVIOUS_WINDOW:
  {
    PreviousWindow();
  }
  break;

  case TMSG_GUI_ADDON_DIALOG:
  {
    if (pMsg->lpVoid)
    {
      static_cast<ADDON::CGUIAddonWindowDialog*>(pMsg->lpVoid)->Show_Internal(pMsg->param2 > 0);
    }
  }
  break;

#ifdef HAS_PYTHON
  case TMSG_GUI_PYTHON_DIALOG:
  {
    // This hack is not much better but at least I don't need to make ApplicationMessenger
    //  know about Addon (Python) specific classes.
    CAction caction(pMsg->param1);
    static_cast<CGUIWindow*>(pMsg->lpVoid)->OnAction(caction);
  }
  break;
#endif

  case TMSG_GUI_ACTION:
  {
    if (pMsg->lpVoid)
    {
      auto action = static_cast<CAction *>(pMsg->lpVoid);
      if (pMsg->param1 == WINDOW_INVALID)
        g_application.OnAction(*action);
      else
      {
        CGUIWindow *pWindow = GetWindow(pMsg->param1);
        if (pWindow)
          pWindow->OnAction(*action);
        else
          CLog::Log(LOGWARNING, "Failed to get window with ID {} to send an action to",
                    pMsg->param1);
      }
      delete action;
    }
  }
  break;

  case TMSG_GUI_MESSAGE:
    if (pMsg->lpVoid)
    {
      auto message = static_cast<CGUIMessage *>(pMsg->lpVoid);
      SendMessage(*message, pMsg->param1);
      delete message;
    }
    break;

  case TMSG_GUI_DIALOG_YESNO:
  {

    if (!pMsg->lpVoid && pMsg->param1 < 0 && pMsg->param2 < 0)
      return;

    auto dialog = static_cast<CGUIDialogYesNo*>(GetWindow(WINDOW_DIALOG_YES_NO));
    if (!dialog)
      return;

    if (pMsg->lpVoid)
      pMsg->SetResult(dialog->ShowAndGetInput(*static_cast<HELPERS::DialogYesNoMessage*>(pMsg->lpVoid)));
    else
    {
      HELPERS::DialogYesNoMessage options;
      options.heading = pMsg->param1;
      options.text = pMsg->param2;
      pMsg->SetResult(dialog->ShowAndGetInput(options));
    }

  }
  break;

  case TMSG_GUI_DIALOG_OK:
  {

    if (!pMsg->lpVoid && pMsg->param1 < 0 && pMsg->param2 < 0)
      return;

    auto dialogOK = static_cast<CGUIDialogOK*>(GetWindow(WINDOW_DIALOG_OK));
    if (!dialogOK)
      return;

    if (pMsg->lpVoid)
      dialogOK->ShowAndGetInput(*static_cast<HELPERS::DialogOKMessage*>(pMsg->lpVoid));
    else
    {
      HELPERS::DialogOKMessage options;
      options.heading = pMsg->param1;
      options.text = pMsg->param2;
      dialogOK->ShowAndGetInput(options);
    }
    pMsg->SetResult(static_cast<int>(dialogOK->IsConfirmed()));
  }
  break;
  }
}

int CGUIWindowManager::GetMessageMask()
{
  return TMSG_MASK_WINDOWMANAGER;
}

bool CGUIWindowManager::OnAction(const CAction &action) const
{
  auto actionId = action.GetID();
  if (actionId == ACTION_GESTURE_BEGIN)
  {
    m_touchGestureActive = true;
  }

  bool ret;
  if (!m_inhibitTouchGestureEvents || !action.IsGesture())
  {
    ret = HandleAction(action);
  }
  else
  {
    // We swallow the event, so it is handled
    ret = true;
    CLog::Log(LOGDEBUG, "Swallowing touch action {} due to inhibition on window switch", actionId);
  }

  if (actionId == ACTION_GESTURE_END || actionId == ACTION_GESTURE_ABORT)
  {
    m_touchGestureActive = false;
    m_inhibitTouchGestureEvents = false;
  }

  return ret;
}

bool CGUIWindowManager::HandleAction(CAction const& action) const
{
  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  size_t topmost = m_activeDialogs.size();
  while (topmost)
  {
    auto dialog = m_activeDialogs[--topmost];
    lock.unlock();
    if (dialog->IsModalDialog())
    { // we have the topmost modal dialog
      if (!dialog->IsAnimating(ANIM_TYPE_WINDOW_CLOSE))
      {
        bool fallThrough = (dialog->GetID() == WINDOW_DIALOG_FULLSCREEN_INFO);
        if (dialog->OnAction(action))
          return true;
        // dialog didn't want the action - we'd normally return false
        // but for some dialogs we want to drop the actions through
        if (fallThrough)
        {
          lock.lock();
          break;
        }
        return false;
      }
      CLog::Log(LOGWARNING,
                "CGUIWindowManager - {} - ignoring action {}, because topmost modal dialog closing "
                "animation is running",
                __FUNCTION__, action.GetID());
      return true; // do nothing with the action until the anim is finished
    }
    lock.lock();
    if (topmost > m_activeDialogs.size())
      topmost = m_activeDialogs.size();
  }
  lock.unlock();
  CGUIWindow* window = GetWindow(GetActiveWindow());
  if (window)
    return window->OnAction(action);
  return false;
}

namespace
{
constexpr int WINDOW_SKIN_OSD_TOPBAR_OVERLAY = WINDOW_HOME + 1109;
bool IsAsyncFullscreenOverlayDialog(int id)
{
  switch (id & WINDOW_ID_MASK)
  {
    case WINDOW_DIALOG_PLAYER_PROCESS_INFO:
    case WINDOW_DIALOG_VIDEO_OSD:
    case WINDOW_DIALOG_VIDEO_OSD_SETTINGS:
    case WINDOW_DIALOG_AUDIO_OSD_SETTINGS:
    case WINDOW_DIALOG_SUBTITLE_OSD_SETTINGS:
      return true;
    case WINDOW_DIALOG_SEEK_BAR:
    case WINDOW_SKIN_OSD_TOPBAR_OVERLAY:
      return CServiceBroker::GetWinSystem()->GetGfxContext().GetGuiHdr() != GuiHdr::SDR;
    default:
      return false;
  }
}
bool IsWorkerEligibleAsyncFullscreenOverlayDialog(int id)
{
  const int maskedId = id & WINDOW_ID_MASK;
  if (maskedId == WINDOW_DIALOG_SEEK_BAR || maskedId == WINDOW_SKIN_OSD_TOPBAR_OVERLAY)
    return CServiceBroker::GetWinSystem()->GetGfxContext().GetGuiHdr() != GuiHdr::SDR;
  return maskedId == WINDOW_DIALOG_VIDEO_OSD;
}
constexpr auto FULLSCREEN_OVERLAY_CACHE_INTERVAL = std::chrono::milliseconds(167);
constexpr auto FULLSCREEN_OVERLAY_RELEASE_TIMEOUT = std::chrono::seconds(3);
constexpr auto FULLSCREEN_OVERLAY_QUIESCE_TIMEOUT = std::chrono::milliseconds(500);
constexpr auto FULLSCREEN_OVERLAY_SOFT_QUIESCE = std::chrono::milliseconds(100);
constexpr auto FULLSCREEN_OVERLAY_FENCE_AGE_LIMIT = std::chrono::milliseconds(500);
constexpr int FULLSCREEN_OVERLAY_FENCE_NR_LIMIT = 30;
constexpr uint64_t FULLSCREEN_OVERLAY_FENCE_WAIT_BOUND_NS = 8000000;
constexpr size_t FULLSCREEN_OVERLAY_MAX_CONTENT_RECTS = 4;
constexpr float FULLSCREEN_OVERLAY_CONTENT_PADDING = 1.0f;
static_assert(FULLSCREEN_OVERLAY_MAX_CONTENT_RECTS <= CGUIRenderTargetFBO::MAX_CONTENT_RECTS);
constexpr float BUF_AGE_SNAP_GRID = 16.0f;
constexpr uint32_t BUF_AGE_EMIT_TOLERANCE = 2;
constexpr auto BUF_AGE_EMIT_FLOOR = std::chrono::seconds(10);

CRect PadContentRect(const CRect& region, float scaleX, float scaleY)
{
  const float padX = scaleX > 0.0f
                         ? std::max(FULLSCREEN_OVERLAY_CONTENT_PADDING, 1.0f / scaleX)
                         : FULLSCREEN_OVERLAY_CONTENT_PADDING;
  const float padY = scaleY > 0.0f
                         ? std::max(FULLSCREEN_OVERLAY_CONTENT_PADDING, 1.0f / scaleY)
                         : FULLSCREEN_OVERLAY_CONTENT_PADDING;
  return CRect(region.x1 - padX, region.y1 - padY, region.x2 + padX, region.y2 + padY);
}

double ContentCoveragePct(const std::vector<CRect>& rects, unsigned int width, unsigned int height)
{
  const double viewport = static_cast<double>(width) * static_cast<double>(height);
  if (viewport <= 0.0)
    return 0.0;
  if (rects.empty() || rects.size() > CGUIRenderTargetFBO::MAX_CONTENT_RECTS)
    return 100.0;
  const CRect bounds(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
  double area = 0.0;
  for (const CRect& rect : rects)
  {
    CRect clipped = rect;
    clipped.Intersect(bounds);
    if (clipped.IsEmpty())
      continue;
    area += static_cast<double>(clipped.Width()) * static_cast<double>(clipped.Height());
  }
  if (area <= 0.0)
    return 100.0;
  return area * 100.0 / viewport;
}

CRect WindowDrawnBounds(const CGUIWindow& window)
{
  CRect region = window.GetRenderRegion();
  const CRect drawn = window.GetVisibleControlBounds();
  if (drawn.IsEmpty())
    return region;
  if (region.IsEmpty())
    return drawn;
  CRect inside = drawn;
  inside.Intersect(region);
  if (!(inside == drawn))
    region.Union(drawn);
  return region;
}

void AppendWindowContentRects(std::vector<CRect>& rects,
                              const CGUIWindow& window,
                              float scaleX,
                              float scaleY,
                              const CRect& viewport,
                              int& widenedWindowId)
{
  const CRect& region = window.GetRenderRegion();
  const bool haveReported = !region.IsEmpty();
  CRect reported;
  if (haveReported)
  {
    reported = PadContentRect(region, scaleX, scaleY);
    reported.Intersect(viewport);
    if (!reported.IsEmpty())
      rects.push_back(reported);
  }

  CRect drawn = window.GetVisibleControlBounds();
  if (drawn.IsEmpty())
    return;
  drawn = PadContentRect(drawn, scaleX, scaleY);
  drawn.Intersect(viewport);
  if (drawn.IsEmpty())
    return;

  if (haveReported)
  {
    CRect inside = drawn;
    inside.Intersect(reported);
    if (inside == drawn)
      return;
  }

  rects.push_back(drawn);
  widenedWindowId = window.GetID() & WINDOW_ID_MASK;
}

std::string FormatRectList(const std::vector<CRect>& rects)
{
  std::string out;
  for (const CRect& rect : rects)
    out += StringUtils::Format("{:.0f},{:.0f}-{:.0f},{:.0f} ", rect.x1, rect.y1, rect.x2, rect.y2);
  return out;
}

std::string FormatAnimFlags(CGUIWindow& window)
{
  std::string out;
  if (window.IsAnimating(ANIM_TYPE_WINDOW_OPEN))
    out += 'O';
  if (window.IsAnimating(ANIM_TYPE_WINDOW_CLOSE))
    out += 'C';
  if (window.IsAnimating(ANIM_TYPE_VISIBLE))
    out += 'V';
  if (window.IsAnimating(ANIM_TYPE_HIDDEN))
    out += 'H';
  if (window.IsAnimating(ANIM_TYPE_FOCUS))
    out += 'F';
  if (window.IsAnimating(ANIM_TYPE_UNFOCUS))
    out += 'U';
  if (window.IsAnimating(ANIM_TYPE_CONDITIONAL))
    out += 'N';
  if (out.empty())
    out = "-";
  return out;
}

std::vector<CRect> DisjointContentRects(std::vector<CRect> rects)
{
  for (bool merged = true; merged;)
  {
    merged = false;
    for (size_t i = 0; i < rects.size() && !merged; ++i)
    {
      for (size_t j = i + 1; j < rects.size(); ++j)
      {
        if (!rects[i].Intersects(rects[j]))
          continue;
        rects[i].Union(rects[j]);
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
        merged = true;
        break;
      }
    }
  }
  if (rects.size() > FULLSCREEN_OVERLAY_MAX_CONTENT_RECTS)
  {
    CRect all;
    for (const CRect& rect : rects)
      all.Union(rect);
    rects.assign(1, all);
  }
  return rects;
}

int ReadSysfsInt(const char* path)
{
  std::ifstream file(path);
  int value = -1;
  if (!(file >> value))
    return -1;
  return value;
}
}

bool RenderOrderSortFunction(const std::shared_ptr<CGUIWindow>& first,
                             const std::shared_ptr<CGUIWindow>& second)
{
  return first->GetRenderOrder() < second->GetRenderOrder();
}

namespace
{
class CFillStateRestore
{
public:
  CFillStateRestore(CGraphicContext& gfx, CRenderSystemBase* renderSystem)
    : m_gfx(gfx), m_renderSystem(renderSystem)
  {
    m_renderOrder = m_gfx.GetRenderOrder();
    m_clipDepth = m_gfx.GetClipRegionDepth();
    m_viewDepth = m_gfx.GetViewPortDepth();
    m_layer = m_gfx.GetLayer();
    m_scissors = m_gfx.GetScissors();
    if (m_renderSystem)
      m_renderSystem->GetViewPort(m_viewPort);
  }

  ~CFillStateRestore()
  {
    while (m_gfx.GetClipRegionDepth() > m_clipDepth)
      m_gfx.RestoreClipRegion();
    while (m_gfx.GetViewPortDepth() > m_viewDepth && m_gfx.GetViewPortDepth() > 1)
      m_gfx.RestoreViewPort();
    m_gfx.SetRenderOrder(m_renderOrder);
    m_gfx.SetLayer(m_layer);
    if (m_renderSystem)
      m_renderSystem->SetViewPort(m_viewPort);
    m_gfx.SetScissors(m_scissors);
  }

private:
  CGraphicContext& m_gfx;
  CRenderSystemBase* m_renderSystem;
  RENDER_ORDER m_renderOrder;
  size_t m_clipDepth;
  size_t m_viewDepth;
  uint32_t m_layer;
  CRect m_scissors;
  CRect m_viewPort;
};
}

class CFullscreenOverlayRenderThread : public CThread
{
public:
  explicit CFullscreenOverlayRenderThread(CGUIWindowManager& manager)
    : CThread("GUIOverlayFBO"), m_manager(manager)
  {
  }

  void QueueRender(std::vector<std::shared_ptr<CGUIWindow>> renderList,
                   CGUIWindowManager::AsyncFullscreenOverlaySignature signature,
                   int targetIndex)
  {
    {
      std::lock_guard lock(m_stateMutex);
      m_renderList = std::move(renderList);
      m_signature = std::move(signature);
      m_renderTargetIndex = targetIndex;
      m_hasPendingRender = true;
    }
    m_renderRequested.Set();
  }

  bool IsBusy()
  {
    std::lock_guard lock(m_stateMutex);
    return m_hasPendingRender || m_renderActive;
  }

  void Wake() { m_renderRequested.Set(); }

protected:
  void Process() override;

private:
  void RunPendingFill(const std::vector<std::shared_ptr<CGUIWindow>>& renderList,
                      const CGUIWindowManager::AsyncFullscreenOverlaySignature& signature,
                      int targetIndex);
  void ReleaseTargetsOnContext(bool force);

  CGUIWindowManager& m_manager;
  CEvent m_renderRequested;
  std::mutex m_stateMutex;
  std::vector<std::shared_ptr<CGUIWindow>> m_renderList;
  CGUIWindowManager::AsyncFullscreenOverlaySignature m_signature;
  int m_renderTargetIndex{-1};
  bool m_hasPendingRender{false};
  bool m_renderActive{false};
  bool m_contextCreated{false};
};

void CFullscreenOverlayRenderThread::Process()
{
  CServiceBroker::GetRenderSystem()->SetThreadGuiShaderScope(true);
  while (!m_bStop)
  {
    AbortableWait(m_renderRequested);
    if (m_bStop)
      break;
    m_renderRequested.Reset();

    std::vector<std::shared_ptr<CGUIWindow>> renderList;
    CGUIWindowManager::AsyncFullscreenOverlaySignature signature;
    int targetIndex = -1;
    bool havePending = false;
    {
      std::lock_guard lock(m_stateMutex);
      if (m_hasPendingRender)
      {
        havePending = true;
        renderList = std::move(m_renderList);
        m_renderList.clear();
        signature = std::move(m_signature);
        targetIndex = m_renderTargetIndex;
        m_hasPendingRender = false;
        m_renderActive = true;
      }
    }

    if (havePending)
    {
      try
      {
        RunPendingFill(renderList, signature, targetIndex);
      }
      catch (...)
      {
        CServiceBroker::GetWinSystem()->UnbindOverlayContext();
        m_manager.m_fullscreenOverlayRenderThreadDisabled.store(true, std::memory_order_relaxed);
        logM(LOGERROR, "async fullscreen OSD worker: exception during fill, disabled");
      }
      {
        std::lock_guard lock(m_stateMutex);
        m_renderActive = false;
      }
      {
        std::lock_guard lock(m_manager.m_fullscreenOverlayStateSection);
        if (m_manager.m_fullscreenOverlayReleaseRequested ||
            m_manager.m_fullscreenOverlayRetryRequested)
          m_renderRequested.Set();
      }
      continue;
    }

    bool releaseRequested = false;
    {
      std::lock_guard lock(m_manager.m_fullscreenOverlayStateSection);
      releaseRequested = m_manager.m_fullscreenOverlayReleaseRequested;
      if (m_manager.m_fullscreenOverlayRetryRequested)
      {
        m_manager.m_fullscreenOverlayRetryRequested = false;
        if (m_manager.m_preparedFullscreenOverlayFence && m_contextCreated &&
            CServiceBroker::GetWinSystem()->BindOverlayContext())
        {
          CServiceBroker::GetRenderSystem()->WaitGuiRenderFence(
              m_manager.m_preparedFullscreenOverlayFence, true);
          CServiceBroker::GetWinSystem()->UnbindOverlayContext();
        }
      }
    }

    if (releaseRequested)
      ReleaseTargetsOnContext(false);
  }

  ReleaseTargetsOnContext(true);
  auto* winSystem = CServiceBroker::GetWinSystem();
  if (m_contextCreated && winSystem)
  {
    if (winSystem->BindOverlayContext())
    {
      CServiceBroker::GetRenderSystem()->ReleaseThreadGuiShaders();
      winSystem->UnbindOverlayContext();
    }
    winSystem->DestroyOverlayContext();
    m_contextCreated = false;
  }
}

void CFullscreenOverlayRenderThread::RunPendingFill(
    const std::vector<std::shared_ptr<CGUIWindow>>& renderList,
    const CGUIWindowManager::AsyncFullscreenOverlaySignature& signature,
    int targetIndex)
{
  const bool diag = m_manager.m_fullscreenOverlayDiagEnabled.load(std::memory_order_relaxed);
  std::chrono::steady_clock::time_point start;
  if (diag)
    start = std::chrono::steady_clock::now();
  auto* winSystem = CServiceBroker::GetWinSystem();
  auto* renderSystem = CServiceBroker::GetRenderSystem();
  if (!winSystem || !renderSystem || targetIndex < 0 ||
      targetIndex >= static_cast<int>(CGUIWindowManager::FULLSCREEN_OVERLAY_RENDER_TARGET_COUNT))
    return;
  if (m_manager.m_fullscreenOverlayRenderThreadDisabled.load(std::memory_order_relaxed))
    return;

  std::unique_lock<CCriticalSection> exclusion(m_manager.m_guiRenderExclusion);

  if (!m_contextCreated)
  {
    if (!winSystem->CreateOverlayContext())
    {
      m_manager.m_fullscreenOverlayRenderThreadDisabled.store(true, std::memory_order_relaxed);
      logM(LOGERROR, "async fullscreen OSD worker: overlay context creation failed, disabled");
      return;
    }
    m_contextCreated = true;
  }

  if (!winSystem->BindOverlayContext())
  {
    m_manager.m_fullscreenOverlayRenderThreadDisabled.store(true, std::memory_order_relaxed);
    logM(LOGERROR, "async fullscreen OSD worker: overlay context bind failed, disabled");
    return;
  }

  void* fence = nullptr;
  bool filled = false;
  int fillIndex = targetIndex;
  {
    auto& gfx = winSystem->GetGfxContext();
    std::unique_lock<CCriticalSection> gfxLock(gfx);
    CFillStateRestore stateRestore(gfx, renderSystem);
    const unsigned int width = gfx.GetWidth();
    const unsigned int height = gfx.GetHeight();
    renderSystem->EstablishGuiRenderBaseline(width, height);
    gfx.SetLayer(m_manager.m_guiDepthLayer.load(std::memory_order_relaxed));
    gfx.ResetScissors();

    std::unique_ptr<CGUIRenderTargetFBO> target;
    void* oldFence = nullptr;
    {
      std::lock_guard lock(m_manager.m_fullscreenOverlayStateSection);
      if (m_manager.m_displayedFullscreenOverlayRenderTargetIndex == fillIndex)
        fillIndex = fillIndex == 0 ? 1 : 0;
      oldFence = m_manager.m_preparedFullscreenOverlayFence;
      m_manager.m_preparedFullscreenOverlayFence = nullptr;
      m_manager.m_preparedFullscreenOverlayRenderTargetIndex = -1;
      target = std::move(m_manager.m_fullscreenOverlayRenderTargets[fillIndex]);
    }
    if (oldFence)
      renderSystem->DeleteGuiRenderFence(oldFence);
    if (!target || target->GetWidth() != width || target->GetHeight() != height)
    {
      target.reset();
      target = renderSystem->CreateGuiRenderTarget(width, height);
    }
    if (target && renderSystem->BeginGuiRenderTarget(*target))
    {
      if (diag)
      {
        uint64_t elapsedNs = 0;
        if (renderSystem->PollGuiRenderTimerNs(elapsedNs))
        {
          const double gpuMs = static_cast<double>(elapsedNs) / 1000000.0;
          std::lock_guard gpuLock(m_manager.m_fullscreenOverlayStateSection);
          ++m_manager.m_fullscreenOverlayWorkerCounters.gpuSamples;
          if (elapsedNs == 0)
            ++m_manager.m_fullscreenOverlayWorkerCounters.gpuZeroSamples;
          m_manager.m_fullscreenOverlayWorkerCounters.gpuMsTotal += gpuMs;
          if (gpuMs > m_manager.m_fullscreenOverlayWorkerCounters.gpuMsMax)
            m_manager.m_fullscreenOverlayWorkerCounters.gpuMsMax = gpuMs;
        }
        renderSystem->BeginGuiRenderTimer();
      }
      const auto fillSettingsComponent = CServiceBroker::GetSettingsComponent();
      const auto fillAdvancedSettings =
          fillSettingsComponent ? fillSettingsComponent->GetAdvancedSettings() : nullptr;
      if (fillAdvancedSettings && fillAdvancedSettings->m_guiFrontToBackRendering)
      {
        gfx.SetRenderOrder(RENDER_ORDER_FRONT_TO_BACK);
        for (auto it = renderList.rbegin(); it != renderList.rend(); ++it)
          (*it)->DoRender();
        gfx.SetRenderOrder(RENDER_ORDER_BACK_TO_FRONT);
      }
      else
      {
        gfx.SetRenderOrder(RENDER_ORDER_ALL_BACK_TO_FRONT);
      }
      for (const auto& window : renderList)
        window->DoRender();
      std::vector<CRect> contentRects;
      contentRects.reserve(renderList.size() * 2);
      float scaleX = 1.0f;
      float scaleY = 1.0f;
      int scaledWidth = 0;
      int scaledHeight = 0;
      const CRect workerViewport(0.0f, 0.0f, static_cast<float>(target->GetWidth()),
                                 static_cast<float>(target->GetHeight()));
      int workerWidenedWindowId = 0;
      for (const auto& window : renderList)
      {
        const auto& coordsRes = window->GetCoordsRes();
        if (coordsRes.iWidth != scaledWidth || coordsRes.iHeight != scaledHeight)
        {
          gfx.GetGUIScaling(coordsRes, scaleX, scaleY);
          scaledWidth = coordsRes.iWidth;
          scaledHeight = coordsRes.iHeight;
        }
        AppendWindowContentRects(contentRects, *window, scaleX, scaleY, workerViewport,
                                 workerWidenedWindowId);
      }
      if (workerWidenedWindowId)
        LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGWINDOWING, 1000,
                              "osdcov: worker window {} draws outside its render region, composite "
                              "coverage widened",
                              workerWidenedWindowId);
      target->SetContentRects(DisjointContentRects(std::move(contentRects)));
      if (m_manager.m_osdTraceArmed.load(std::memory_order_relaxed))
      {
        std::string workerFocus;
        for (const auto& window : renderList)
          workerFocus += StringUtils::Format("{}:{} ", window->GetID() & WINDOW_ID_MASK,
                                             window->GetFocusedControlID());
        logComponentM(LOGDEBUG, LOGWINDOWING, "osdwfill: idx={} members={} focus=[{}] cov=[{}]",
                      fillIndex, renderList.size(), workerFocus,
                      FormatRectList(target->GetContentRects()));
      }
      renderSystem->EndGuiRenderTarget(*target);
      if (diag)
        renderSystem->EndGuiRenderTimer();
      fence = renderSystem->CreateGuiRenderFence();
      filled = fence != nullptr;
    }
    {
      std::lock_guard lock(m_manager.m_fullscreenOverlayStateSection);
      m_manager.m_fullscreenOverlayRenderTargets[fillIndex] = std::move(target);
    }
  }

  winSystem->UnbindOverlayContext();

  if (!filled)
    return;

  const double prepMs =
      diag ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                 .count()
           : 0.0;
  std::lock_guard lock(m_manager.m_fullscreenOverlayStateSection);
  m_manager.m_preparedFullscreenOverlayRenderTargetIndex = fillIndex;
  m_manager.m_preparedFullscreenOverlaySignature = signature;
  m_manager.m_preparedFullscreenOverlayFence = fence;
  m_manager.m_preparedFullscreenOverlayPublishTime = std::chrono::steady_clock::now();
  ++m_manager.m_fullscreenOverlayWorkerCounters.prep;
  m_manager.m_fullscreenOverlayWorkerCounters.prepMsTotal += prepMs;
}

void CFullscreenOverlayRenderThread::ReleaseTargetsOnContext(bool force)
{
  std::array<std::unique_ptr<CGUIRenderTargetFBO>,
             CGUIWindowManager::FULLSCREEN_OVERLAY_RENDER_TARGET_COUNT>
      targets;
  void* fence = nullptr;
  {
    std::lock_guard lock(m_manager.m_fullscreenOverlayStateSection);
    if (!force && !m_manager.m_fullscreenOverlayReleaseRequested)
      return;
    for (size_t i = 0; i < targets.size(); ++i)
      targets[i] = std::move(m_manager.m_fullscreenOverlayRenderTargets[i]);
    fence = m_manager.m_preparedFullscreenOverlayFence;
    m_manager.m_preparedFullscreenOverlayFence = nullptr;
    m_manager.m_preparedFullscreenOverlayRenderTargetIndex = -1;
    m_manager.m_displayedFullscreenOverlayRenderTargetIndex = -1;
    m_manager.m_preparedFullscreenOverlaySignature = {};
    m_manager.m_displayedFullscreenOverlaySignature = {};
    m_manager.m_fullscreenOverlayReleaseRequested = false;
  }

  if (!fence && !targets[0] && !targets[1])
    return;

  auto* winSystem = CServiceBroker::GetWinSystem();
  if (!m_contextCreated || !winSystem || !winSystem->BindOverlayContext())
  {
    for (auto& target : targets)
      static_cast<void>(target.release());
    m_manager.m_fullscreenOverlayRenderThreadDisabled.store(true, std::memory_order_relaxed);
    logM(LOGERROR,
         "async fullscreen OSD worker: release without context, targets and fence leaked");
    return;
  }

  auto* renderSystem = CServiceBroker::GetRenderSystem();
  if (fence && renderSystem)
    renderSystem->DeleteGuiRenderFence(fence);
  for (auto& target : targets)
    target.reset();
  winSystem->UnbindOverlayContext();
}

CGUIWindowManager::~CGUIWindowManager() = default;

void CGUIWindowManager::Process(unsigned int currentTime)
{
  assert(CServiceBroker::GetAppMessenger()->IsProcessThread());
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  m_dirtyregions.clear();

  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
    pWindow->DoProcess(currentTime, m_dirtyregions);

  // process all dialogs - visibility may change etc.
  // copy shared_ptrs to ensure dialogs stay alive during iteration even if internal containers are modified
  auto dialogs = m_dialogWindows;
  for (const auto& window : dialogs)
  {
    if (window && (window->IsDialog() || IsPythonWindow(window->GetID())))
      window->DoProcess(currentTime, m_dirtyregions);
  }

  // assign depth values to all active controls
  if (pWindow)
    pWindow->AssignDepth();

  auto activeDialogs = m_activeDialogs;
  stable_sort(activeDialogs.begin(), activeDialogs.end(), RenderOrderSortFunction);
  for (const auto& window : activeDialogs)
  {
    if (window->IsDialogRunning())
      window->AssignDepth();
  }

  m_guiDepthLayer.store(
      CServiceBroker::GetWinSystem()->GetGfxContext().GetLayer(), std::memory_order_relaxed);

  for (auto& itr : m_dirtyregions)
    m_tracker.MarkDirtyRegion(itr);
}

void CGUIWindowManager::MarkDirty()
{
  MarkDirty(CRect(0, 0, float(CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth()), float(CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight())));
}

void CGUIWindowManager::MarkDirtyRegionOnly(const CRect& rect)
{
  m_tracker.MarkDirtyRegion(CDirtyRegion(rect));
}

void CGUIWindowManager::MarkDirty(const CRect& rect)
{
  m_tracker.MarkDirtyRegion(CDirtyRegion(rect));

  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
    pWindow->MarkDirtyRegion();

  // make copy of vector as we may remove items from it as we go
  auto activeDialogs = m_activeDialogs;
  for (const auto& window : activeDialogs)
    if (window->IsDialogRunning())
      window->MarkDirtyRegion();
}

void CGUIWindowManager::UpdateFullscreenOverlayRenderTarget() const
{
  m_fullscreenOverlayComposite = false;
  m_fullscreenOverlayCompositeIds.clear();
  const auto now = std::chrono::steady_clock::now();

  static std::chrono::steady_clock::time_point s_statWindow{};
  static int s_frames = 0;
  static int s_composites = 0;
  static int s_fills = 0;
  static int s_creates = 0;
  static int s_releases = 0;
  static int s_declines = 0;
  static const char* s_lastReason = "none";
  static unsigned int s_fboW = 0;
  static unsigned int s_fboH = 0;
  if (now - s_statWindow >= std::chrono::seconds(1))
  {
    logComponentM(LOGDEBUG, LOGWINDOWING,
                  "asyncosd: frames={} composites={} fills={} create={} release={} decline={} "
                  "lastReason={} fbo={}x{}",
                  s_frames, s_composites, s_fills, s_creates, s_releases, s_declines, s_lastReason,
                  s_fboW, s_fboH);
    s_statWindow = now;
    s_frames = s_composites = s_fills = s_creates = s_releases = s_declines = 0;
  }
  ++s_frames;

  FullscreenOverlayFillStats stats;
  const bool composite = FillFullscreenOverlayTarget(stats, now);
  s_composites += stats.composites;
  s_fills += stats.fills;
  s_creates += stats.creates;
  s_releases += stats.releases;
  s_declines += stats.declines;
  if (stats.lastReason)
    s_lastReason = stats.lastReason;
  if (stats.fboW >= 0)
  {
    s_fboW = static_cast<unsigned int>(stats.fboW);
    s_fboH = static_cast<unsigned int>(stats.fboH);
  }
  m_fullscreenOverlayComposite = composite;
  m_osdTraceReason = composite ? "sync" : (stats.lastReason ? stats.lastReason : "decline");
}

bool CGUIWindowManager::FillFullscreenOverlayTarget(FullscreenOverlayFillStats& stats,
                                                    std::chrono::steady_clock::time_point now) const
{
  if (m_fullscreenOverlayRenderTarget &&
      (now - m_fullscreenOverlayLastActiveTime) >= FULLSCREEN_OVERLAY_RELEASE_TIMEOUT)
  {
    m_fullscreenOverlayRenderTarget.reset();
    ++stats.releases;
    stats.fboW = 0;
    stats.fboH = 0;
  }

  auto* renderSystem = CServiceBroker::GetRenderSystem();
  if (!renderSystem || !renderSystem->SupportsGuiRenderTargets())
    return false;

  auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  if (gfx.GetStereoMode() != RENDER_STEREO_MODE_OFF)
  {
    stats.lastReason = "stereo";
    ++stats.declines;
    return false;
  }

  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (!pWindow || ((pWindow->GetID() & WINDOW_ID_MASK) != WINDOW_FULLSCREEN_VIDEO))
  {
    stats.lastReason = "not-fullscreen";
    return false;
  }

  auto renderList = m_activeDialogs;
  stable_sort(renderList.begin(), renderList.end(), RenderOrderSortFunction);

  std::vector<CGUIWindow*> whitelisted;
  bool haveBand = false;
  int whitelistMin = 0;
  int whitelistMax = 0;
  bool anyAnimating = false;
  bool anyControlDirty = false;
  size_t signature = 0;
  for (const auto& window : renderList)
  {
    if (!window->IsDialogRunning())
      continue;
    if (!IsAsyncFullscreenOverlayDialog(window->GetID() & WINDOW_ID_MASK))
      continue;
    whitelisted.push_back(window.get());
    const int order = window->GetRenderOrder();
    if (!haveBand)
    {
      whitelistMin = order;
      haveBand = true;
    }
    whitelistMax = order;
    signature = signature * 31 + static_cast<size_t>(window->GetID() & WINDOW_ID_MASK);
    signature = signature * 31 + static_cast<size_t>(window->GetFocusedControlID());
    if (window->IsControlDirty())
      anyControlDirty = true;
    if (window->IsAnimating(ANIM_TYPE_WINDOW_OPEN) || window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE) ||
        window->IsAnimating(ANIM_TYPE_VISIBLE) || window->IsAnimating(ANIM_TYPE_HIDDEN) ||
        window->IsAnimating(ANIM_TYPE_FOCUS) || window->IsAnimating(ANIM_TYPE_UNFOCUS) ||
        window->IsAnimating(ANIM_TYPE_CONDITIONAL))
      anyAnimating = true;
  }
  if (whitelisted.empty())
  {
    stats.lastReason = "no-whitelisted";
    return false;
  }
  bool interleaved = false;
  for (const auto& window : renderList)
  {
    if (!window->IsDialogRunning())
      continue;
    if (IsAsyncFullscreenOverlayDialog(window->GetID() & WINDOW_ID_MASK))
      continue;
    const int order = window->GetRenderOrder();
    if (order >= whitelistMin && order <= whitelistMax)
    {
      interleaved = true;
      break;
    }
  }
  int guestCount = 0;
  if (interleaved)
  {
    const auto guestSettingsComponent = CServiceBroker::GetSettingsComponent();
    const auto guestAdvancedSettings =
        guestSettingsComponent ? guestSettingsComponent->GetAdvancedSettings() : nullptr;
    if (!guestAdvancedSettings || !guestAdvancedSettings->m_guiOsdGuestComposite)
    {
      stats.lastReason = "interleave";
      ++stats.declines;
      return false;
    }
    whitelisted.clear();
    signature = 0;
    anyAnimating = false;
    anyControlDirty = false;
    for (const auto& window : renderList)
    {
      if (!window->IsDialogRunning())
        continue;
      const int id = window->GetID() & WINDOW_ID_MASK;
      const int order = window->GetRenderOrder();
      if (!IsAsyncFullscreenOverlayDialog(id) &&
          !(order >= whitelistMin && order <= whitelistMax))
        continue;
      if (!IsAsyncFullscreenOverlayDialog(id))
        ++guestCount;
      whitelisted.push_back(window.get());
      signature = signature * 31 + static_cast<size_t>(id);
      signature = signature * 31 + static_cast<size_t>(window->GetFocusedControlID());
      if (window->IsControlDirty())
        anyControlDirty = true;
      if (window->IsAnimating(ANIM_TYPE_WINDOW_OPEN) ||
          window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE) ||
          window->IsAnimating(ANIM_TYPE_VISIBLE) || window->IsAnimating(ANIM_TYPE_HIDDEN) ||
          window->IsAnimating(ANIM_TYPE_FOCUS) || window->IsAnimating(ANIM_TYPE_UNFOCUS) ||
          window->IsAnimating(ANIM_TYPE_CONDITIONAL))
        anyAnimating = true;
    }
  }

  const unsigned int width = gfx.GetWidth();
  const unsigned int height = gfx.GetHeight();
  bool recreated = false;
  if (!m_fullscreenOverlayRenderTarget ||
      m_fullscreenOverlayRenderTarget->GetWidth() != width ||
      m_fullscreenOverlayRenderTarget->GetHeight() != height)
  {
    m_fullscreenOverlayRenderTarget = renderSystem->CreateGuiRenderTarget(width, height);
    recreated = true;
    ++stats.creates;
    stats.fboW = static_cast<int>(width);
    stats.fboH = static_cast<int>(height);
  }
  if (!m_fullscreenOverlayRenderTarget)
  {
    stats.lastReason = "create-failed";
    ++stats.declines;
    return false;
  }

  if (!anyControlDirty)
    m_fullscreenOverlayDirtyEpisodeFilled = false;

  const bool fillDiag = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                        CServiceBroker::GetLogging().CanLogComponent(LOGWINDOWING);
  std::vector<std::pair<int, int>> currentFocus;
  if (fillDiag)
  {
    currentFocus.reserve(whitelisted.size());
    for (auto* window : whitelisted)
      currentFocus.emplace_back(window->GetID() & WINDOW_ID_MASK, window->GetFocusedControlID());
  }
  const bool sigChanged = signature != m_fullscreenOverlaySignature;
  const bool dirtyDue = anyControlDirty && !m_fullscreenOverlayDirtyEpisodeFilled;
  const bool intervalDue =
      (now - m_fullscreenOverlayLastFillTime) >= FULLSCREEN_OVERLAY_CACHE_INTERVAL;
  const auto sinceFillMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - m_fullscreenOverlayLastFillTime)
                               .count();
  const bool needFill = recreated || sigChanged || anyAnimating || dirtyDue || intervalDue;
  const char* fillReason = "none";
  m_osdTraceFill = "none";
  if (needFill)
  {
    if (recreated || sigChanged)
    {
      fillReason = "sig";
      ++stats.fillSig;
    }
    else if (anyAnimating)
    {
      fillReason = "anim";
      ++stats.fillAnim;
    }
    else if (dirtyDue)
    {
      fillReason = "dirty";
      ++stats.fillDirty;
    }
    else
    {
      fillReason = "interval";
      ++stats.fillInterval;
    }
  }
  else
  {
    if (anyControlDirty)
      ++stats.fillSuppressed;
    if (fillDiag && currentFocus != m_fullscreenOverlayFilledFocus)
      ++stats.staleFocus;
  }
  m_osdTraceFill = fillReason;
  if (needFill)
  {
    CFillStateRestore stateRestore(gfx, renderSystem);
    if (!renderSystem->BeginGuiRenderTarget(*m_fullscreenOverlayRenderTarget))
    {
      stats.lastReason = "begin-failed";
      ++stats.declines;
      return false;
    }
    const auto fillSettingsComponent = CServiceBroker::GetSettingsComponent();
    const auto fillAdvancedSettings =
        fillSettingsComponent ? fillSettingsComponent->GetAdvancedSettings() : nullptr;
    if (fillAdvancedSettings && fillAdvancedSettings->m_guiFrontToBackRendering)
    {
      gfx.SetRenderOrder(RENDER_ORDER_FRONT_TO_BACK);
      for (auto it = whitelisted.rbegin(); it != whitelisted.rend(); ++it)
        (*it)->DoRender();
      gfx.SetRenderOrder(RENDER_ORDER_BACK_TO_FRONT);
    }
    else
    {
      gfx.SetRenderOrder(RENDER_ORDER_ALL_BACK_TO_FRONT);
    }
    for (auto* window : whitelisted)
      window->DoRender();
    std::vector<CRect> contentRects;
    contentRects.reserve(whitelisted.size() * 2);
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    int scaledWidth = 0;
    int scaledHeight = 0;
    const CRect viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    int widenedWindowId = 0;
    for (auto* window : whitelisted)
    {
      const auto& coordsRes = window->GetCoordsRes();
      if (coordsRes.iWidth != scaledWidth || coordsRes.iHeight != scaledHeight)
      {
        gfx.GetGUIScaling(coordsRes, scaleX, scaleY);
        scaledWidth = coordsRes.iWidth;
        scaledHeight = coordsRes.iHeight;
      }
      AppendWindowContentRects(contentRects, *window, scaleX, scaleY, viewport, widenedWindowId);
    }
    if (widenedWindowId)
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGWINDOWING, 1000,
                            "osdcov: window {} draws outside its render region, composite "
                            "coverage widened",
                            widenedWindowId);
    m_fullscreenOverlayRenderTarget->SetContentRects(DisjointContentRects(std::move(contentRects)));
    renderSystem->EndGuiRenderTarget(*m_fullscreenOverlayRenderTarget);
    m_fullscreenOverlayLastFillTime = now;
    m_fullscreenOverlaySignature = signature;
    if (anyControlDirty)
      m_fullscreenOverlayDirtyEpisodeFilled = true;
    ++stats.fills;
    if (fillDiag)
    {
      std::string focusNow;
      for (const auto& entry : currentFocus)
        focusNow += StringUtils::Format("{}:{} ", entry.first, entry.second);
      std::string focusPrev;
      for (const auto& entry : m_fullscreenOverlayFilledFocus)
        focusPrev += StringUtils::Format("{}:{} ", entry.first, entry.second);
      logComponentM(LOGDEBUG, LOGWINDOWING,
                    "osdfill: reason={} members={} guests={} sinceMs={} dirty={} focus=[{}] prev=[{}]",
                    fillReason, whitelisted.size(), guestCount, sinceFillMs,
                    anyControlDirty ? 1 : 0, focusNow, focusPrev);
      m_fullscreenOverlayFilledFocus = currentFocus;
    }
  }

  m_fullscreenOverlayCompositeIds.clear();
  m_fullscreenOverlayCompositeIds.reserve(whitelisted.size());
  for (auto* window : whitelisted)
    m_fullscreenOverlayCompositeIds.push_back(window->GetID() & WINDOW_ID_MASK);
  if (guestCount > 0)
    LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGWINDOWING, 1000, "osdguest: members={} guests={}",
                          whitelisted.size(), guestCount);

  m_fullscreenOverlayLastActiveTime = now;
  ++stats.composites;
  return true;
}

void CGUIWindowManager::SelectFullscreenOverlayCompositeSource() const
{
  const auto now = std::chrono::steady_clock::now();

  const bool diag = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                    CServiceBroker::GetLogging().CanLogComponent(LOGWINDOWING);
  m_fullscreenOverlayDiagEnabled.store(diag, std::memory_order_relaxed);

  static std::chrono::steady_clock::time_point s_statWindow{};
  if (diag && now - s_statWindow >= std::chrono::seconds(1))
  {
    FullscreenOverlayWorkerCounters window;
    {
      std::lock_guard lock(m_fullscreenOverlayStateSection);
      window = m_fullscreenOverlayWorkerCounters;
      m_fullscreenOverlayWorkerCounters = {};
    }
    s_statWindow = now;
    if (HasFullscreenOverlayWorkerActivity(window))
    {
      if (!m_fullscreenOverlayDiagConfigLogged)
      {
        const auto cfgSettingsComponent = CServiceBroker::GetSettingsComponent();
        const auto cfgAdvanced =
            cfgSettingsComponent ? cfgSettingsComponent->GetAdvancedSettings() : nullptr;
        auto* cfgWinSystem = CServiceBroker::GetWinSystem();
        auto* cfgRenderSystem = CServiceBroker::GetRenderSystem();
        if (cfgAdvanced && cfgWinSystem && cfgRenderSystem)
        {
          m_fullscreenOverlayDiagConfigLogged = true;
          auto& cfgGfx = cfgWinSystem->GetGfxContext();
          logComponentM(LOGDEBUG, LOGWINDOWING,
                        "asyncosdCfg: fbo={}x{} bufferAgePartial={} dirtyAlgo={} maxDirty={} "
                        "waitVsync={} waitGpu={} skipSleep={} gpuTimer={}",
                        cfgGfx.GetWidth(), cfgGfx.GetHeight(),
                        cfgAdvanced->m_guiBufferAgePartialRedraw,
                        cfgAdvanced->m_guiAlgorithmDirtyRegions, cfgAdvanced->m_guiMaxDirtyRegions,
                        cfgAdvanced->m_guiWaitVsyncBeforeSwap, cfgAdvanced->m_guiWaitGpuBeforeSwap,
                        cfgAdvanced->m_guiSkipSleepActiveWindow,
                        cfgRenderSystem->SupportsGuiRenderTimer());
        }
      }
      const int avgPrepMs =
          window.prep > 0 ? static_cast<int>(window.prepMsTotal / window.prep) : 0;
      const bool gpuTimerUseless = window.gpuSamples > 0 && window.gpuMsMax <= 0.0;
      const int avgGpuUs =
          (window.gpuSamples > 0 && !gpuTimerUseless)
              ? static_cast<int>(window.gpuMsTotal * 1000.0 / window.gpuSamples)
              : -1;
      const int maxGpuUs = (window.gpuSamples > 0 && !gpuTimerUseless)
                               ? static_cast<int>(window.gpuMsMax * 1000.0)
                               : -1;
      const int avgContentPct =
          window.contentSamples > 0
              ? static_cast<int>(window.contentPctTotal / window.contentSamples)
              : -1;
      const int maxContentPct =
          window.contentSamples > 0 ? static_cast<int>(window.contentPctMax) : -1;
      logComponentM(LOGDEBUG, LOGWINDOWING,
                    "asyncosdC: prep={} promo={} stale={} fenceNR={} fenceNRMax={} fenceWait={} "
                    "wdRetry={} "
                    "wdDemote={} busySkip={} pendSkip={} stall={} relReq={} inline={} avgPrepMs={} mode={} "
                    "excl={} compNone={} gpuN={} gpuZero={} gpuAvgUs={} gpuMaxUs={} syncComposites={} "
                    "syncFills={} syncCreate={} syncRelease={} syncDecline={} contentN={} "
                    "contentPct={} contentMaxPct={} syncLastReason={}",
                    window.prep, window.promo, window.stale, window.fenceNR, window.fenceNRMax,
                    window.fenceWait,
                    window.wdRetry, window.wdDemote, window.busySkip, window.pendSkip, window.stall,
                    window.relReq,
                    window.inlineFill, avgPrepMs, 2, window.excl, window.compNone,
                    window.gpuSamples, window.gpuZeroSamples, avgGpuUs, maxGpuUs,
                    window.syncComposites, window.syncFills, window.syncCreates,
                    window.syncReleases, window.syncDeclines, window.contentSamples,
                    avgContentPct, maxContentPct,
                    window.syncLastReason ? window.syncLastReason : "none");
      logComponentM(LOGDEBUG, LOGWINDOWING,
                    "osdfresh: fillSig={} fillAnim={} fillDirty={} fillInterval={} "
                    "suppressed={} staleFocusFrames={}",
                    window.fillSig, window.fillAnim, window.fillDirty, window.fillInterval,
                    window.fillSuppressed, window.staleFocus);
    }
  }

  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    m_fullscreenOverlayCompositeSource = FullscreenOverlayCompositeSource::NONE;
  }
  m_fullscreenOverlayCompositeIds.clear();
  m_osdTraceReason = "none";

  const auto guestSettingsComponent = CServiceBroker::GetSettingsComponent();
  const auto guestAdvancedSettings =
      guestSettingsComponent ? guestSettingsComponent->GetAdvancedSettings() : nullptr;
  const bool guestComposite = guestAdvancedSettings && guestAdvancedSettings->m_guiOsdGuestComposite;
  for (const auto& window : m_activeDialogs)
  {
    if (!window->IsDialogRunning())
      continue;
    if (!guestComposite && !IsAsyncFullscreenOverlayDialog(window->GetID() & WINDOW_ID_MASK))
      continue;
    if (window->IsAnimating(ANIM_TYPE_WINDOW_OPEN) || window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE) ||
        window->IsAnimating(ANIM_TYPE_VISIBLE) || window->IsAnimating(ANIM_TYPE_HIDDEN))
    {
      std::lock_guard lock(m_fullscreenOverlayStateSection);
      if (m_preparedFullscreenOverlayRenderTargetIndex >= 0 || m_preparedFullscreenOverlayFence)
      {
        auto* renderSystem = CServiceBroker::GetRenderSystem();
        if (m_preparedFullscreenOverlayFence && renderSystem)
        {
          renderSystem->DeleteGuiRenderFence(m_preparedFullscreenOverlayFence);
          m_preparedFullscreenOverlayFence = nullptr;
        }
        m_preparedFullscreenOverlayRenderTargetIndex = -1;
        m_fullscreenOverlayFenceNRStreak = 0;
        m_fullscreenOverlayRetryAttempted = false;
      }
      m_displayedFullscreenOverlayRenderTargetIndex = -1;
      m_osdTraceReason = "anim-disengage";
      return;
    }
  }

  const AsyncFullscreenOverlaySignature current = BuildAsyncFullscreenOverlaySignature();
  const FullscreenOverlayPromoteResult promoted =
      PromotePreparedFullscreenOverlayRenderTarget(current);
  if (promoted != FullscreenOverlayPromoteResult::NONE)
  {
    m_fullscreenOverlayCompositeIds = current.dialogIds;
    m_osdTraceReason = promoted == FullscreenOverlayPromoteResult::PROMOTED ? "promoted" : "kept";
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    m_fullscreenOverlayCompositeSource = FullscreenOverlayCompositeSource::WORKER_FBO;
    return;
  }

  FullscreenOverlayFillStats stats;
  const bool filled = FillFullscreenOverlayTarget(stats, now);
  m_osdTraceReason = filled ? "sync" : (stats.lastReason ? stats.lastReason : "decline");
  if (filled)
  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    m_fullscreenOverlayCompositeSource = FullscreenOverlayCompositeSource::SYNC_TARGET;
    if (diag)
    {
      AccumulateSyncFillStats(stats);
      ++m_fullscreenOverlayWorkerCounters.inlineFill;
    }
  }
  else if (diag && !current.dialogIds.empty())
  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    AccumulateSyncFillStats(stats);
    ++m_fullscreenOverlayWorkerCounters.compNone;
  }
}

bool CGUIWindowManager::HasFullscreenOverlayWorkerActivity(
    const FullscreenOverlayWorkerCounters& counters)
{
  return counters.prep || counters.promo || counters.stale || counters.fenceNR ||
         counters.fenceWait || counters.pendSkip ||
         counters.wdRetry || counters.wdDemote || counters.busySkip || counters.stall ||
         counters.relReq || counters.inlineFill || counters.excl || counters.compNone ||
         counters.syncComposites || counters.syncFills || counters.syncCreates ||
         counters.syncReleases || counters.syncDeclines || counters.contentSamples ||
         counters.gpuSamples || counters.syncLastReason;
}

void CGUIWindowManager::AccumulateSyncFillStats(const FullscreenOverlayFillStats& stats) const
{
  auto& counters = m_fullscreenOverlayWorkerCounters;
  counters.syncComposites += stats.composites;
  counters.syncFills += stats.fills;
  counters.syncCreates += stats.creates;
  counters.syncReleases += stats.releases;
  counters.syncDeclines += stats.declines;
  counters.fillSig += stats.fillSig;
  counters.fillAnim += stats.fillAnim;
  counters.fillDirty += stats.fillDirty;
  counters.fillInterval += stats.fillInterval;
  counters.fillSuppressed += stats.fillSuppressed;
  counters.staleFocus += stats.staleFocus;
  if (stats.lastReason)
    counters.syncLastReason = stats.lastReason;
}

bool CGUIWindowManager::WaitPreparedFullscreenOverlayFence(CRenderSystemBase* renderSystem) const
{
  if (renderSystem->WaitGuiRenderFence(m_preparedFullscreenOverlayFence, true))
    return true;
  if (!renderSystem->WaitGuiRenderFenceBounded(m_preparedFullscreenOverlayFence,
                                               FULLSCREEN_OVERLAY_FENCE_WAIT_BOUND_NS))
    return false;
  ++m_fullscreenOverlayWorkerCounters.fenceWait;
  return true;
}

CGUIWindowManager::FullscreenOverlayPromoteResult CGUIWindowManager::
    PromotePreparedFullscreenOverlayRenderTarget(const AsyncFullscreenOverlaySignature& current) const
{
  auto* renderSystem = CServiceBroker::GetRenderSystem();
  std::lock_guard lock(m_fullscreenOverlayStateSection);
  auto& counters = m_fullscreenOverlayWorkerCounters;

  if (m_fullscreenOverlayRenderThreadDisabled.load(std::memory_order_relaxed))
    return FullscreenOverlayPromoteResult::NONE;

  if (current.dialogIds.empty())
  {
    m_displayedFullscreenOverlayRenderTargetIndex = -1;
    return FullscreenOverlayPromoteResult::NONE;
  }

  for (int id : current.dialogIds)
  {
    if (!IsWorkerEligibleAsyncFullscreenOverlayDialog(id))
    {
      m_displayedFullscreenOverlayRenderTargetIndex = -1;
      return FullscreenOverlayPromoteResult::NONE;
    }
  }

  if (m_preparedFullscreenOverlayRenderTargetIndex >= 0 && renderSystem &&
      !current.dialogIds.empty())
  {
    if (!(m_preparedFullscreenOverlaySignature == current))
    {
      ++counters.stale;
      if (m_preparedFullscreenOverlayFence)
      {
        renderSystem->DeleteGuiRenderFence(m_preparedFullscreenOverlayFence);
        m_preparedFullscreenOverlayFence = nullptr;
      }
      m_preparedFullscreenOverlayRenderTargetIndex = -1;
      m_fullscreenOverlayFenceNRStreak = 0;
      m_fullscreenOverlayRetryAttempted = false;
    }
    else if (WaitPreparedFullscreenOverlayFence(renderSystem))
    {
      if (m_preparedFullscreenOverlayFence)
      {
        renderSystem->DeleteGuiRenderFence(m_preparedFullscreenOverlayFence);
        m_preparedFullscreenOverlayFence = nullptr;
      }
      m_displayedFullscreenOverlayRenderTargetIndex = m_preparedFullscreenOverlayRenderTargetIndex;
      m_displayedFullscreenOverlaySignature = m_preparedFullscreenOverlaySignature;
      m_preparedFullscreenOverlayRenderTargetIndex = -1;
      m_fullscreenOverlayFenceNRStreak = 0;
      m_fullscreenOverlayRetryAttempted = false;
      ++counters.promo;
      auto* self = const_cast<CGUIWindowManager*>(this);
      const auto promoteSettingsComponent = CServiceBroker::GetSettingsComponent();
      const auto promoteAdvancedSettings =
          promoteSettingsComponent ? promoteSettingsComponent->GetAdvancedSettings() : nullptr;
      if (promoteAdvancedSettings && promoteAdvancedSettings->m_guiBufferAgePartialRedraw >= 1)
      {
        std::vector<CRect> regions;
        for (const auto& window : m_activeDialogs)
        {
          if (!window->IsDialogRunning())
            continue;
          if (!IsAsyncFullscreenOverlayDialog(window->GetID() & WINDOW_ID_MASK))
            continue;
          const CRect region = WindowDrawnBounds(*window);
          if (!region.IsEmpty())
            regions.push_back(region);
        }
        if (regions.empty() && m_lastPromotedRegions.empty())
          self->MarkDirty();
        else
        {
          for (const auto& region : regions)
            self->MarkDirtyRegionOnly(region);
          for (const auto& region : m_lastPromotedRegions)
            self->MarkDirtyRegionOnly(region);
        }
        self->m_lastPromotedRegions = std::move(regions);
      }
      else
      {
        self->MarkDirty();
      }
      return FullscreenOverlayPromoteResult::PROMOTED;
    }
    else
    {
      ++counters.fenceNR;
      if (m_fullscreenOverlayFenceNRIndex != m_preparedFullscreenOverlayRenderTargetIndex ||
          m_fullscreenOverlayFenceNRPublishTime != m_preparedFullscreenOverlayPublishTime)
      {
        m_fullscreenOverlayFenceNRIndex = m_preparedFullscreenOverlayRenderTargetIndex;
        m_fullscreenOverlayFenceNRPublishTime = m_preparedFullscreenOverlayPublishTime;
        m_fullscreenOverlayFenceNRStreak = 0;
      }
      ++m_fullscreenOverlayFenceNRStreak;
      if (m_fullscreenOverlayFenceNRStreak > counters.fenceNRMax)
        counters.fenceNRMax = m_fullscreenOverlayFenceNRStreak;
      const bool aged = (std::chrono::steady_clock::now() - m_preparedFullscreenOverlayPublishTime) >
                        FULLSCREEN_OVERLAY_FENCE_AGE_LIMIT;
      if (aged || m_fullscreenOverlayFenceNRStreak >= FULLSCREEN_OVERLAY_FENCE_NR_LIMIT)
      {
        if (!m_fullscreenOverlayRetryAttempted)
        {
          m_fullscreenOverlayRetryAttempted = true;
          m_fullscreenOverlayRetryRequested = true;
          m_preparedFullscreenOverlayPublishTime = std::chrono::steady_clock::now();
          m_fullscreenOverlayFenceNRStreak = 0;
          ++counters.wdRetry;
          if (m_fullscreenOverlayRenderThread)
            m_fullscreenOverlayRenderThread->Wake();
        }
        else
        {
          ++counters.wdDemote;
          m_fullscreenOverlayRenderThreadDisabled.store(true, std::memory_order_relaxed);
          if (m_preparedFullscreenOverlayFence)
          {
            renderSystem->DeleteGuiRenderFence(m_preparedFullscreenOverlayFence);
            m_preparedFullscreenOverlayFence = nullptr;
          }
          m_preparedFullscreenOverlayRenderTargetIndex = -1;
          m_displayedFullscreenOverlayRenderTargetIndex = -1;
          m_fullscreenOverlayReleaseRequested = true;
          if (m_fullscreenOverlayRenderThread)
            m_fullscreenOverlayRenderThread->Wake();
          logM(LOGERROR,
               "async fullscreen OSD worker: fence never signaled, demoted to sync fallback");
          const auto demoteSettingsComponent = CServiceBroker::GetSettingsComponent();
          const auto demoteAdvancedSettings =
              demoteSettingsComponent ? demoteSettingsComponent->GetAdvancedSettings() : nullptr;
          if (demoteAdvancedSettings &&
              demoteAdvancedSettings->m_guiBufferAgePartialRedraw >= 1)
            const_cast<CGUIWindowManager*>(this)->MarkDirty();
        }
      }
    }
  }

  if (m_displayedFullscreenOverlayRenderTargetIndex >= 0 && !current.dialogIds.empty() &&
      m_displayedFullscreenOverlaySignature == current)
    return FullscreenOverlayPromoteResult::KEEP_DISPLAYED;

  return FullscreenOverlayPromoteResult::NONE;
}

CGUIWindowManager::AsyncFullscreenOverlaySignature CGUIWindowManager::
    BuildAsyncFullscreenOverlaySignature() const
{
  AsyncFullscreenOverlaySignature signature;

  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (!pWindow || ((pWindow->GetID() & WINDOW_ID_MASK) != WINDOW_FULLSCREEN_VIDEO))
    return signature;

  auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  if (gfx.GetStereoMode() != RENDER_STEREO_MODE_OFF)
    return signature;
  signature.activeWindowID = pWindow->GetID() & WINDOW_ID_MASK;
  signature.width = gfx.GetWidth();
  signature.height = gfx.GetHeight();
  auto* renderSystem = CServiceBroker::GetRenderSystem();
  signature.shaderEpoch = renderSystem ? renderSystem->GetGuiShaderEpoch() : 0;

  auto renderList = m_activeDialogs;
  stable_sort(renderList.begin(), renderList.end(), RenderOrderSortFunction);
  bool haveBand = false;
  int whitelistMin = 0;
  int whitelistMax = 0;
  for (const auto& window : renderList)
  {
    if (!window->IsDialogRunning())
      continue;
    if (!IsAsyncFullscreenOverlayDialog(window->GetID() & WINDOW_ID_MASK))
      continue;
    signature.dialogIds.push_back(window->GetID() & WINDOW_ID_MASK);
    signature.focusIds.push_back(window->GetFocusedControlID());
    const int order = window->GetRenderOrder();
    if (!haveBand)
    {
      whitelistMin = order;
      haveBand = true;
    }
    whitelistMax = order;
  }
  bool interleaved = false;
  for (const auto& window : renderList)
  {
    if (!window->IsDialogRunning())
      continue;
    if (IsAsyncFullscreenOverlayDialog(window->GetID() & WINDOW_ID_MASK))
      continue;
    const int order = window->GetRenderOrder();
    if (haveBand && order >= whitelistMin && order <= whitelistMax)
    {
      interleaved = true;
      break;
    }
  }
  if (interleaved)
  {
    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    const auto advancedSettings =
        settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
    signature.dialogIds.clear();
    signature.focusIds.clear();
    if (advancedSettings && advancedSettings->m_guiOsdGuestComposite)
    {
      for (const auto& window : renderList)
      {
        if (!window->IsDialogRunning())
          continue;
        const int id = window->GetID() & WINDOW_ID_MASK;
        const int order = window->GetRenderOrder();
        if (IsAsyncFullscreenOverlayDialog(id) ||
            (order >= whitelistMin && order <= whitelistMax))
        {
          signature.dialogIds.push_back(id);
          signature.focusIds.push_back(window->GetFocusedControlID());
        }
      }
    }
  }
  return signature;
}

const CGUIRenderTargetFBO* CGUIWindowManager::GetFullscreenOverlayCompositeTarget() const
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings = settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
  const int mode = advancedSettings ? advancedSettings->m_videoAsyncFullscreenOSD : 0;
  if (mode == 1)
  {
    if (m_fullscreenOverlayComposite && m_fullscreenOverlayRenderTarget)
      return m_fullscreenOverlayRenderTarget.get();
    return nullptr;
  }
  if (mode == 2)
  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    if (m_fullscreenOverlayCompositeSource == FullscreenOverlayCompositeSource::SYNC_TARGET &&
        m_fullscreenOverlayRenderTarget)
      return m_fullscreenOverlayRenderTarget.get();
    if (m_fullscreenOverlayCompositeSource == FullscreenOverlayCompositeSource::WORKER_FBO &&
        m_displayedFullscreenOverlayRenderTargetIndex >= 0)
      return m_fullscreenOverlayRenderTargets[m_displayedFullscreenOverlayRenderTargetIndex].get();
  }
  return nullptr;
}

void CGUIWindowManager::QuiesceFullscreenOverlayWorker()
{
  if (!m_fullscreenOverlayRenderThread)
    return;
  if (m_fullscreenOverlayRenderThreadDisabled.load(std::memory_order_relaxed))
    return;
  if (!m_fullscreenOverlayRenderThread->IsBusy())
    return;

  if (m_fullscreenOverlayDiagEnabled.load(std::memory_order_relaxed))
  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    ++m_fullscreenOverlayWorkerCounters.stall;
  }
  CSingleExit exitGfx(CServiceBroker::GetWinSystem()->GetGfxContext());
  const auto deadline = std::chrono::steady_clock::now() + FULLSCREEN_OVERLAY_SOFT_QUIESCE;
  while (m_fullscreenOverlayRenderThread->IsBusy())
  {
    if (std::chrono::steady_clock::now() >= deadline)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool CGUIWindowManager::BeginRenderExclusion()
{
  if (!m_fullscreenOverlayRenderThread)
    return true;

  {
    CSingleExit exitGfx(CServiceBroker::GetWinSystem()->GetGfxContext());
    if (m_guiRenderExclusion.try_lock())
    {
      m_renderExclusionHeld = true;
      return true;
    }
    if (m_fullscreenOverlayDiagEnabled.load(std::memory_order_relaxed))
    {
      std::lock_guard lock(m_fullscreenOverlayStateSection);
      ++m_fullscreenOverlayWorkerCounters.excl;
    }
    const auto deadline = std::chrono::steady_clock::now() + FULLSCREEN_OVERLAY_QUIESCE_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (m_guiRenderExclusion.try_lock())
      {
        m_renderExclusionHeld = true;
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  const bool firstFailure =
      !m_fullscreenOverlayRenderThreadDisabled.exchange(true, std::memory_order_relaxed);
  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    m_preparedFullscreenOverlayRenderTargetIndex = -1;
    m_displayedFullscreenOverlayRenderTargetIndex = -1;
    m_fullscreenOverlayReleaseRequested = true;
    ++m_fullscreenOverlayWorkerCounters.wdDemote;
  }
  m_fullscreenOverlayRenderThread->Wake();
  if (firstFailure)
    logM(LOGERROR,
         "async fullscreen OSD worker held the render exclusion beyond its budget, disabled");
  const auto demoteSettingsComponent = CServiceBroker::GetSettingsComponent();
  const auto demoteAdvancedSettings =
      demoteSettingsComponent ? demoteSettingsComponent->GetAdvancedSettings() : nullptr;
  if (demoteAdvancedSettings && demoteAdvancedSettings->m_guiBufferAgePartialRedraw >= 1)
    MarkDirty();
  return false;
}

void CGUIWindowManager::EndRenderExclusion()
{
  if (!m_renderExclusionHeld)
    return;
  m_renderExclusionHeld = false;
  m_guiRenderExclusion.unlock();
}

void CGUIWindowManager::ScheduleAsyncFullscreenOverlayRender()
{
  if (m_fullscreenOverlayRenderThreadDisabled.load(std::memory_order_relaxed))
    return;

  const auto now = std::chrono::steady_clock::now();
  AsyncFullscreenOverlaySignature signature = BuildAsyncFullscreenOverlaySignature();
  if (signature.activeWindowID == 0 || signature.dialogIds.empty())
  {
    bool wake = false;
    {
      std::lock_guard lock(m_fullscreenOverlayStateSection);
      bool haveTargets = false;
      for (const auto& target : m_fullscreenOverlayRenderTargets)
      {
        if (target)
        {
          haveTargets = true;
          break;
        }
      }
      if (haveTargets && !m_fullscreenOverlayReleaseRequested &&
          (now - m_fullscreenOverlayWorkerLastActivity) >= FULLSCREEN_OVERLAY_RELEASE_TIMEOUT)
      {
        m_fullscreenOverlayReleaseRequested = true;
        ++m_fullscreenOverlayWorkerCounters.relReq;
        wake = true;
      }
    }
    if (wake && m_fullscreenOverlayRenderThread)
      m_fullscreenOverlayRenderThread->Wake();
    return;
  }

  m_fullscreenOverlayWorkerLastActivity = now;

  for (int id : signature.dialogIds)
  {
    if (!IsWorkerEligibleAsyncFullscreenOverlayDialog(id))
      return;
  }

  std::vector<std::shared_ptr<CGUIWindow>> workerList;
  bool animatingOrDirty = false;
  bool animTransition = false;
  auto renderList = m_activeDialogs;
  stable_sort(renderList.begin(), renderList.end(), RenderOrderSortFunction);
  for (const auto& window : renderList)
  {
    if (!window->IsDialogRunning())
      continue;
    if (!IsAsyncFullscreenOverlayDialog(window->GetID() & WINDOW_ID_MASK))
      continue;
    workerList.push_back(window);
    if (window->IsControlDirty())
      animatingOrDirty = true;
    if (window->IsAnimating(ANIM_TYPE_WINDOW_OPEN) || window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE) ||
        window->IsAnimating(ANIM_TYPE_VISIBLE) || window->IsAnimating(ANIM_TYPE_HIDDEN))
    {
      animatingOrDirty = true;
      animTransition = true;
    }
    if (window->IsAnimating(ANIM_TYPE_FOCUS) || window->IsAnimating(ANIM_TYPE_UNFOCUS) ||
        window->IsAnimating(ANIM_TYPE_CONDITIONAL))
      animatingOrDirty = true;
  }
  if (workerList.empty() || animTransition)
    return;

  const bool signatureChanged = !(signature == m_pendingFullscreenOverlaySignature);
  const bool refresh = (now - m_fullscreenOverlayLastQueueTime) >= FULLSCREEN_OVERLAY_CACHE_INTERVAL;
  if (!signatureChanged && !animatingOrDirty && !refresh)
    return;

  if (m_fullscreenOverlayRenderThread && m_fullscreenOverlayRenderThread->IsBusy())
  {
    ++m_fullscreenOverlayWorkerCounters.busySkip;
    return;
  }

  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    if (m_preparedFullscreenOverlayRenderTargetIndex >= 0)
    {
      ++m_fullscreenOverlayWorkerCounters.pendSkip;
      return;
    }
  }

  if (!m_fullscreenOverlayRenderThread)
  {
    m_fullscreenOverlayRenderThread = std::make_unique<CFullscreenOverlayRenderThread>(*this);
    m_fullscreenOverlayRenderThread->Create();
  }

  int targetIndex = 0;
  {
    std::lock_guard lock(m_fullscreenOverlayStateSection);
    targetIndex = m_displayedFullscreenOverlayRenderTargetIndex == 0 ? 1 : 0;
    m_fullscreenOverlayReleaseRequested = false;
  }
  m_pendingFullscreenOverlaySignature = signature;
  m_fullscreenOverlayLastQueueTime = now;
  m_fullscreenOverlayRenderThread->QueueRender(std::move(workerList), std::move(signature),
                                               targetIndex);
}

void CGUIWindowManager::StopFullscreenOverlayRenderThread()
{
  if (!m_fullscreenOverlayRenderThread)
    return;
  m_fullscreenOverlayRenderThread->StopThread(false);
  m_fullscreenOverlayRenderThread->Wake();
  {
    CSingleExit exitGfx(CServiceBroker::GetWinSystem()->GetGfxContext());
    m_fullscreenOverlayRenderThread->StopThread(true);
  }
  m_fullscreenOverlayRenderThread.reset();

  std::lock_guard lock(m_fullscreenOverlayStateSection);
  if (m_preparedFullscreenOverlayFence)
  {
    auto* renderSystem = CServiceBroker::GetRenderSystem();
    if (renderSystem)
      renderSystem->DeleteGuiRenderFence(m_preparedFullscreenOverlayFence);
    m_preparedFullscreenOverlayFence = nullptr;
  }
  m_preparedFullscreenOverlayRenderTargetIndex = -1;
  m_displayedFullscreenOverlayRenderTargetIndex = -1;
}

void CGUIWindowManager::RenderPass() const
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings = settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
  const bool frontToBackRendering = advancedSettings && advancedSettings->m_guiFrontToBackRendering;

  if (frontToBackRendering)
    RenderPassDual();
  else
    RenderPassSingle();
}

void CGUIWindowManager::CompositeFullscreenOverlay(CRenderSystemBase* renderSystem,
                                                   const CGUIRenderTargetFBO& target) const
{
  if (m_fullscreenOverlayDiagEnabled.load(std::memory_order_relaxed) &&
      !m_fullscreenOverlayContentSampled)
  {
    m_fullscreenOverlayContentSampled = true;
    const double coverage =
        ContentCoveragePct(target.GetContentRects(), target.GetWidth(), target.GetHeight());
    std::lock_guard contentLock(m_fullscreenOverlayStateSection);
    ++m_fullscreenOverlayWorkerCounters.contentSamples;
    m_fullscreenOverlayWorkerCounters.contentPctTotal += coverage;
    if (coverage > m_fullscreenOverlayWorkerCounters.contentPctMax)
      m_fullscreenOverlayWorkerCounters.contentPctMax = coverage;
  }
  ++m_osdTraceBlitCount;
  renderSystem->RenderGuiRenderTarget(target);
}

void CGUIWindowManager::RenderPassSingle() const
{
  m_osdTracePass = 1;
  m_osdTraceBlitIndex = -1;
  m_osdTraceLiveAfterBlit = 0;
  CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderOrder(RENDER_ORDER_ALL_BACK_TO_FRONT);
  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
  {
    pWindow->ClearBackground();
    pWindow->DoRender();
  }

  auto renderList = m_activeDialogs;
  stable_sort(renderList.begin(), renderList.end(), RenderOrderSortFunction);

  const CGUIRenderTargetFBO* compositeTarget = GetFullscreenOverlayCompositeTarget();
  if (compositeTarget)
  {
    auto* renderSystem = CServiceBroker::GetRenderSystem();
    const auto isComposited = [this](int id) {
      if (!m_fullscreenOverlayCompositeIds.empty())
        return std::find(m_fullscreenOverlayCompositeIds.begin(),
                         m_fullscreenOverlayCompositeIds.end(),
                         id) != m_fullscreenOverlayCompositeIds.end();
      return IsAsyncFullscreenOverlayDialog(id);
    };
    bool composited = false;
    int traceIndex = 0;
    for (const auto& window : renderList)
    {
      if (!window->IsDialogRunning())
        continue;
      if (isComposited(window->GetID() & WINDOW_ID_MASK))
      {
        if (!composited && renderSystem)
        {
          m_osdTraceBlitIndex = traceIndex;
          CompositeFullscreenOverlay(renderSystem, *compositeTarget);
          composited = true;
        }
      }
      else
      {
        window->DoRender();
        if (composited)
          ++m_osdTraceLiveAfterBlit;
      }
      ++traceIndex;
    }
    if (!composited && renderSystem)
    {
      m_osdTraceBlitIndex = traceIndex;
      CompositeFullscreenOverlay(renderSystem, *compositeTarget);
    }
    return;
  }

  // we render the dialogs based on their render order.
  for (const auto& window : renderList)
  {
    if (window->IsDialogRunning())
      window->DoRender();
  }
}

void CGUIWindowManager::RenderPassDual() const
{
  m_osdTracePass = 2;
  m_osdTraceBlitIndex = -1;
  m_osdTraceLiveAfterBlit = 0;
  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
    pWindow->ClearBackground();

  const CGUIRenderTargetFBO* compositeTarget = GetFullscreenOverlayCompositeTarget();

  auto renderList = m_activeDialogs;
  stable_sort(renderList.begin(), renderList.end(), RenderOrderSortFunction);

  // first the opaque pass, rendering from front to back
  CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderOrder(RENDER_ORDER_FRONT_TO_BACK);
  if (!compositeTarget)
  {
    for (auto it = renderList.rbegin(); it != renderList.rend(); ++it)
    {
      if ((*it)->IsDialogRunning())
        (*it)->DoRender();
    }
  }

  if (pWindow)
    pWindow->DoRender();

  // now we render all elements with transparency back to front
  CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderOrder(RENDER_ORDER_BACK_TO_FRONT);
  if (pWindow)
  {
    pWindow->DoRender();
  }

  if (compositeTarget)
  {
    auto* renderSystem = CServiceBroker::GetRenderSystem();
    const auto isComposited = [this](int id) {
      if (!m_fullscreenOverlayCompositeIds.empty())
        return std::find(m_fullscreenOverlayCompositeIds.begin(),
                         m_fullscreenOverlayCompositeIds.end(),
                         id) != m_fullscreenOverlayCompositeIds.end();
      return IsAsyncFullscreenOverlayDialog(id);
    };
    bool composited = false;
    int traceIndex = 0;
    for (const auto& window : renderList)
    {
      if (!window->IsDialogRunning())
        continue;
      if (isComposited(window->GetID() & WINDOW_ID_MASK))
      {
        if (!composited && renderSystem)
        {
          m_osdTraceBlitIndex = traceIndex;
          CompositeFullscreenOverlay(renderSystem, *compositeTarget);
          composited = true;
        }
      }
      else
      {
        auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
        const RENDER_ORDER renderOrder = gfx.GetRenderOrder();
        gfx.SetRenderOrder(RENDER_ORDER_ALL_BACK_TO_FRONT);
        window->DoRender();
        gfx.SetRenderOrder(renderOrder);
        if (composited)
          ++m_osdTraceLiveAfterBlit;
      }
      ++traceIndex;
    }
    if (!composited && renderSystem)
    {
      m_osdTraceBlitIndex = traceIndex;
      CompositeFullscreenOverlay(renderSystem, *compositeTarget);
    }
  }
  else
  {
    for (const auto& window : renderList)
    {
      if (window->IsDialogRunning())
        window->DoRender();
    }
  }
}

void CGUIWindowManager::RenderEx() const
{
  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
    pWindow->RenderEx();

  // We don't call RenderEx for now on dialogs since it is used
  // to trigger non gui video rendering. We can activate it later at any time.
  /*
  vector<CGUIWindow *> &activeDialogs = m_activeDialogs;
  for (iDialog it = activeDialogs.begin(); it != activeDialogs.end(); ++it)
  {
    if ((*it)->IsDialogRunning())
      (*it)->RenderEx();
  }
  */
}

bool CGUIWindowManager::Render(bool forScreenshot)
{
  assert(CServiceBroker::GetAppMessenger()->IsProcessThread());
  CSingleExit lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  // AfterRender resets the live layer count, while cached control transforms
  // retain their assigned depths. A capture outside the normal frame needs the
  // corresponding count or those controls fall outside the clip depth range.
  auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  KODI::UTILS::CScopeGuard<uint32_t, 0, void(uint32_t)> restoreDepthLayer(
      [&gfx](uint32_t layer) { gfx.SetLayer(layer); }, forScreenshot ? gfx.GetLayer() : 0);
  if (forScreenshot)
    gfx.SetLayer(m_guiDepthLayer.load(std::memory_order_relaxed));

  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings = settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;

  const int asyncOverlayMode = advancedSettings ? advancedSettings->m_videoAsyncFullscreenOSD : -1;

  m_osdTraceArmed.store(advancedSettings && advancedSettings->m_guiOsdTrace != 0 &&
                            CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                            CServiceBroker::GetLogging().CanLogComponent(LOGWINDOWING),
                        std::memory_order_relaxed);
  m_osdTraceFill = "n/a";
  m_osdTraceBlitIndex = -1;
  m_osdTraceLiveAfterBlit = 0;
  m_osdTraceBlitCount = 0;
  m_osdTracePass = 0;
  m_osdTraceSkinRet = -1;

  m_fullscreenOverlayContentSampled = false;
  if (asyncOverlayMode != 2)
    m_fullscreenOverlayDiagEnabled.store(false, std::memory_order_relaxed);

  if (asyncOverlayMode == 1)
    UpdateFullscreenOverlayRenderTarget();
  else if (asyncOverlayMode == 2)
    SelectFullscreenOverlayCompositeSource();
  else
  {
    m_fullscreenOverlayCompositeIds.clear();
    m_osdTraceReason = "mode0";
  }

  CRenderSystemBase* renderSystem = CServiceBroker::GetRenderSystem();
  bool skinHdrWanted = false;
  if (advancedSettings && advancedSettings->m_guiSkinHdrFbo && renderSystem)
  {
    auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
    if (gfx.GetGuiHdr() != GuiHdr::SDR && gfx.GetStereoMode() == RENDER_STEREO_MODE_OFF)
    {
      const CGUIWindow* activeWindow = GetWindow(GetActiveWindow());
      const int activeId = activeWindow ? (activeWindow->GetID() & WINDOW_ID_MASK) : 0;
      skinHdrWanted = renderSystem->SupportsGuiRenderTargetConvert() &&
                      activeId != WINDOW_FULLSCREEN_VIDEO && activeId != WINDOW_FULLSCREEN_GAME &&
                      activeId != WINDOW_VISUALISATION && activeId != WINDOW_SLIDESHOW &&
                      activeId != WINDOW_SCREENSAVER;
    }
    if (m_skinHdrRenderTarget && gfx.GetGuiHdr() == GuiHdr::SDR)
    {
      m_skinHdrRenderTarget.reset();
      m_skinHdrTargetValid = false;
    }
  }
  else if (m_skinHdrRenderTarget)
  {
    m_skinHdrRenderTarget.reset();
    m_skinHdrTargetValid = false;
  }
  if (skinHdrWanted != m_skinHdrEngagedLastFrame)
  {
    m_skinHdrEngagedLastFrame = skinHdrWanted;
    m_skinHdrTargetValid = false;
    MarkDirty();
  }

  const bool visualizeDirtyRegions =
      advancedSettings && advancedSettings->m_guiVisualizeDirtyRegions;
  const int guiAlgorithmDirtyRegions =
      advancedSettings ? advancedSettings->m_guiAlgorithmDirtyRegions
                       : DIRTYREGION_SOLVER_FILL_VIEWPORT_ALWAYS;
  const int bufferAgeLever =
      advancedSettings ? advancedSettings->m_guiBufferAgePartialRedraw : 0;
  const int maxDirtyRegions = advancedSettings ? advancedSettings->m_guiMaxDirtyRegions : 0;

  if (bufferAgeLever >= 1)
  {
    const int stereoMode =
        static_cast<int>(CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode());
    if (stereoMode != m_bufAgeLastStereoMode)
    {
      m_bufAgeLastStereoMode = stereoMode;
      MarkDirty();
    }
  }

  // A screenshot draws without swapping. Draw the entire capture and the next
  // normal pass, so neither depends on stale pixels or declares damage twice
  // between swaps. A capture must not advance the swap-based region history.
  const bool forceFullRedraw = forScreenshot || m_renderedForScreenshot;
  m_renderedForScreenshot = forScreenshot;
  int bufferAge = forceFullRedraw ? 0 : CServiceBroker::GetWinSystem()->GetBufferAge();
  const int realAge = bufferAge;
  if (visualizeDirtyRegions)
    bufferAge = 20;
  if (!forScreenshot)
  {
    if (bufferAge)
      m_tracker.CleanMarkedRegions(bufferAge + 1);
    else
      m_tracker.CleanMarkedRegions(10);
  }

  CDirtyRegionList dirtyRegions = m_tracker.GetDirtyRegions();
  float frameSnapGrowth = 0.0f;
  if (!dirtyRegions.empty())
  {
    const CRect viewWindow =
        bufferAgeLever >= 1
            ? CRect(0, 0, float(CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth()),
                    float(CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight()))
            : CServiceBroker::GetWinSystem()->GetGfxContext().GetViewWindow();
    constexpr float dirtyRegionPadding = 1.0f;

    for (auto& region : dirtyRegions)
    {
      region.x1 -= dirtyRegionPadding;
      region.y1 -= dirtyRegionPadding;
      region.x2 += dirtyRegionPadding;
      region.y2 += dirtyRegionPadding;
      region.Intersect(viewWindow);
    }

    if (bufferAgeLever >= 1)
    {
      for (auto& region : dirtyRegions)
      {
        if (region.IsEmpty())
          continue;
        frameSnapGrowth -= region.Area();
        region.x1 = std::floor(region.x1 / BUF_AGE_SNAP_GRID) * BUF_AGE_SNAP_GRID;
        region.y1 = std::floor(region.y1 / BUF_AGE_SNAP_GRID) * BUF_AGE_SNAP_GRID;
        region.x2 = std::ceil(region.x2 / BUF_AGE_SNAP_GRID) * BUF_AGE_SNAP_GRID;
        region.y2 = std::ceil(region.y2 / BUF_AGE_SNAP_GRID) * BUF_AGE_SNAP_GRID;
        region.Intersect(viewWindow);
        frameSnapGrowth += region.Area();
      }
    }
  }

  bool hasRendered = false;
  bool renderedViaRegions = false;
  uint32_t frameRects = 0;
  uint32_t frameClip0 = 0;
  float frameDirtyPx = 0.0f;
  bool frameCapped = false;
  const bool bufAgeDiag = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                          CServiceBroker::GetLogging().CanLogComponent(LOGWINDOWING);
  uint32_t frameMarkedRects = 0;
  float frameMarkedPct = 0.0f;
  if (bufAgeDiag && bufferAgeLever >= 1)
  {
    const CDirtyRegionList& markedRegions = m_tracker.GetMarkedRegions();
    const float markedW = static_cast<float>(CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth());
    const float markedH = static_cast<float>(CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight());
    uint64_t rowMask[36] = {};
    for (const auto& r : markedRegions)
    {
      if (r.IsEmpty() || markedW <= 0.0f || markedH <= 0.0f)
        continue;
      if (r.x2 <= 0.0f || r.y2 <= 0.0f || r.x1 >= markedW || r.y1 >= markedH)
        continue;
      ++frameMarkedRects;
      const int c0 = std::clamp(static_cast<int>(r.x1 * 64.0f / markedW), 0, 63);
      const int c1 = std::clamp(static_cast<int>((r.x2 - 1.0f) * 64.0f / markedW), c0, 63);
      const int r0 = std::clamp(static_cast<int>(r.y1 * 36.0f / markedH), 0, 35);
      const int r1 = std::clamp(static_cast<int>((r.y2 - 1.0f) * 36.0f / markedH), r0, 35);
      const uint64_t colMask = (c1 - c0 == 63) ? ~uint64_t(0)
                                               : (((uint64_t(1) << (c1 - c0 + 1)) - 1) << c0);
      for (int row = r0; row <= r1; ++row)
        rowMask[row] |= colMask;
    }
    int covered = 0;
    for (uint64_t m : rowMask)
      covered += __builtin_popcountll(m);
    frameMarkedPct = covered * 100.0f / (64.0f * 36.0f);
  }

  const auto renderDirtyRegions = [&]() {
    bool rendered = false;
    CDirtyRegionList damageRegions;
    damageRegions.reserve(dirtyRegions.size());
    for (const auto& i : dirtyRegions)
    {
      if (i.IsEmpty())
        ++frameClip0;
      else
        damageRegions.push_back(i);
    }
    if (maxDirtyRegions > 0 && damageRegions.size() > static_cast<size_t>(maxDirtyRegions))
    {
      RenderPass();
      frameRects = static_cast<uint32_t>(damageRegions.size());
      frameCapped = true;
      renderedViaRegions = false;
      return true;
    }
    if (!damageRegions.empty() &&
        !CServiceBroker::GetWinSystem()->SetDirtyRegions(damageRegions))
    {
      // Recover this frame as well as subsequent ones; presenting even one
      // clipped pass after rejected damage can expose undefined/black pixels.
      CServiceBroker::GetWinSystem()->GetGfxContext().ResetScissors();
      RenderPass();
      frameRects = static_cast<uint32_t>(damageRegions.size());
      renderedViaRegions = false;
      return true;
    }
    for (const auto& i : damageRegions)
    {
      if (bufAgeDiag)
        frameDirtyPx += i.Area();

      CServiceBroker::GetWinSystem()->GetGfxContext().SetScissors(i);
      RenderPass();
      rendered = true;
    }
    CServiceBroker::GetWinSystem()->GetGfxContext().ResetScissors();

    frameRects = static_cast<uint32_t>(damageRegions.size());
    renderedViaRegions = rendered;
    return rendered;
  };

  bool skinHdrBegun = false;
  if (skinHdrWanted && renderSystem)
  {
    auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
    const unsigned int fboWidth = static_cast<unsigned int>(gfx.GetWidth());
    const unsigned int fboHeight = static_cast<unsigned int>(gfx.GetHeight());
    if (!m_skinHdrRenderTarget || m_skinHdrRenderTarget->GetWidth() != fboWidth ||
        m_skinHdrRenderTarget->GetHeight() != fboHeight)
    {
      m_skinHdrRenderTarget = renderSystem->CreateGuiRenderTarget(fboWidth, fboHeight);
      m_skinHdrTargetValid = false;
    }
    if (m_skinHdrRenderTarget)
      skinHdrBegun =
          renderSystem->BeginGuiRenderTargetPersistent(*m_skinHdrRenderTarget, !m_skinHdrTargetValid);
  }
  const bool skinHdrForceFull = skinHdrBegun && !m_skinHdrTargetValid;

  if (skinHdrBegun != m_skinHdrEngagedLogged)
  {
    m_skinHdrEngagedLogged = skinHdrBegun;
    logM(LOGINFO, "skin HDR composite target {} ({}x{})", skinHdrBegun ? "engaged" : "released",
         m_skinHdrRenderTarget ? m_skinHdrRenderTarget->GetWidth() : 0,
         m_skinHdrRenderTarget ? m_skinHdrRenderTarget->GetHeight() : 0);
  }

  // If we visualize the regions we will always render the entire viewport
  // If the buffer age is zero, the current content is undefined and has to be rendered
  if (visualizeDirtyRegions || bufferAge == 0 || skinHdrForceFull ||
      guiAlgorithmDirtyRegions == DIRTYREGION_SOLVER_FILL_VIEWPORT_ALWAYS)
  {
    CServiceBroker::GetWinSystem()->GetGfxContext().ResetScissors();
    RenderPass();
    hasRendered = true;
  }
  else if (guiAlgorithmDirtyRegions == DIRTYREGION_SOLVER_FILL_VIEWPORT_ON_CHANGE)
  {
    if (!dirtyRegions.empty())
    {
      RenderPass();
      hasRendered = true;
    }
  }
  else
  {
    hasRendered = renderDirtyRegions();

    if (!hasRendered && !dirtyRegions.empty())
    {
      RenderPass();
      hasRendered = true;
    }
  }

  if (skinHdrBegun)
  {
    renderSystem->EndGuiRenderTarget(*m_skinHdrRenderTarget);
    if (hasRendered)
    {
      m_skinHdrTargetValid = true;
      std::vector<CRect> compositeRects;
      if (renderedViaRegions)
      {
        compositeRects.reserve(dirtyRegions.size());
        for (const auto& region : dirtyRegions)
        {
          if (!region.IsEmpty())
            compositeRects.push_back(region);
        }
      }
      m_skinHdrRenderTarget->SetContentRects(DisjointContentRects(std::move(compositeRects)));
      CServiceBroker::GetWinSystem()->GetGfxContext().ResetScissors();
      m_osdTraceSkinRet = renderSystem->RenderGuiRenderTarget(*m_skinHdrRenderTarget, true) ? 1 : 0;
      std::string quadStr;
      if (bufAgeDiag)
      {
        for (const auto& quad : m_skinHdrRenderTarget->GetContentRects())
          quadStr += StringUtils::Format("{:.0f},{:.0f}-{:.0f},{:.0f} ", quad.x1, quad.y1, quad.x2,
                                         quad.y2);
      }
      auto& logGfx = CServiceBroker::GetWinSystem()->GetGfxContext();
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGWINDOWING, 1000,
                            "skinfbo: full={} rects={} forceFull={} age={} fbo={}x{} gui={}x{} "
                            "q=[{}]",
                            hasRendered && !renderedViaRegions ? 1 : 0, frameRects,
                            skinHdrForceFull ? 1 : 0, realAge, m_skinHdrRenderTarget->GetWidth(),
                            m_skinHdrRenderTarget->GetHeight(), logGfx.GetWidth(),
                            logGfx.GetHeight(), quadStr);
    }
    else
    {
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGWINDOWING, 1000,
                            "skinfbo: composite skipped, nothing rendered this frame");
    }
  }

  if (bufAgeDiag)
  {
    int animOpen = 0;
    int animClose = 0;
    std::string animIds;
    CGUIWindow* animActiveWindow = GetWindow(GetActiveWindow());
    if (animActiveWindow &&
        ((animActiveWindow->GetID() & WINDOW_ID_MASK) == WINDOW_FULLSCREEN_VIDEO))
    {
      for (const auto& window : m_activeDialogs)
      {
        if (!window->IsDialogRunning())
          continue;
        const bool opening = window->IsAnimating(ANIM_TYPE_WINDOW_OPEN);
        const bool closing = window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE);
        if (!opening && !closing)
          continue;
        if (opening)
          ++animOpen;
        if (closing)
          ++animClose;
        if (!animIds.empty())
          animIds += ',';
        animIds += std::to_string(window->GetID() & WINDOW_ID_MASK);
      }
    }
    static auto s_animLastFrame = std::chrono::steady_clock::now();
    static bool s_wasAnimating = false;
    if (animOpen || animClose)
    {
      const auto animNow = std::chrono::steady_clock::now();
      const auto animGapUs =
          std::chrono::duration_cast<std::chrono::microseconds>(animNow - s_animLastFrame).count();
      int animSrc = 0;
      {
        std::lock_guard animLock(m_fullscreenOverlayStateSection);
        animSrc = static_cast<int>(m_fullscreenOverlayCompositeSource);
      }
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGWINDOWING, 1000,
                    "osdanim: ids={} open={} close={} gapMs={:.1f} full={} partial={} rects={} "
                    "dirtyPx={:.0f} src={}",
                    animIds, animOpen, animClose, s_wasAnimating ? animGapUs / 1000.0 : 0.0,
                    hasRendered && !renderedViaRegions ? 1 : 0, renderedViaRegions ? 1 : 0,
                    frameRects, frameDirtyPx, animSrc);
      s_animLastFrame = animNow;
      s_wasAnimating = true;
    }
    else
      s_wasAnimating = false;
  }

  if (m_osdTraceArmed.load(std::memory_order_relaxed))
  {
    ++m_osdTraceFrame;
    CGUIWindow* traceActive = GetWindow(GetActiveWindow());
    const int traceActiveId = traceActive ? (traceActive->GetID() & WINDOW_ID_MASK) : 0;
    const char* rendKind = renderedViaRegions ? "partial" : (hasRendered ? "full" : "none");

    if (traceActiveId == WINDOW_FULLSCREEN_VIDEO)
    {
      int traceSrc = 0;
      {
        std::lock_guard lock(m_fullscreenOverlayStateSection);
        traceSrc = static_cast<int>(m_fullscreenOverlayCompositeSource);
      }
      const CGUIRenderTargetFBO* traceTarget = GetFullscreenOverlayCompositeTarget();
      std::string members;
      for (int id : m_fullscreenOverlayCompositeIds)
        members += StringUtils::Format("{} ", id);
      std::string running;
      std::string focus;
      std::string regions;
      std::string focusRegion;
      for (const auto& window : m_activeDialogs)
      {
        if (!window->IsDialogRunning())
          continue;
        const int id = window->GetID() & WINDOW_ID_MASK;
        running += StringUtils::Format("{}:{}:{} ", id, window->GetRenderOrder(),
                                       FormatAnimFlags(*window));
        CGUIControl* focused = window->GetFocusedControl();
        focus += StringUtils::Format("{}:{} ", id, focused ? focused->GetID() : 0);
        const CRect& windowRegion = window->GetRenderRegion();
        regions += StringUtils::Format("{}:{:.0f},{:.0f}-{:.0f},{:.0f} ", id, windowRegion.x1,
                                       windowRegion.y1, windowRegion.x2, windowRegion.y2);
        if (focused)
        {
          const CRect& controlRegion = focused->GetRenderRegion();
          focusRegion += StringUtils::Format("{}:{}:{:.0f},{:.0f}-{:.0f},{:.0f} ", id,
                                             focused->GetID(), controlRegion.x1, controlRegion.y1,
                                             controlRegion.x2, controlRegion.y2);
        }
      }
      std::string coverage = "null";
      if (traceTarget)
        coverage = StringUtils::Format("[{}]", FormatRectList(traceTarget->GetContentRects()));
      std::string damage;
      for (const auto& region : dirtyRegions)
      {
        if (!region.IsEmpty())
          damage += StringUtils::Format("{:.0f},{:.0f}-{:.0f},{:.0f} ", region.x1, region.y1,
                                        region.x2, region.y2);
      }
      bool overlayAnimated = false;
      uint64_t overlaySig = 0;
      const auto& appComponents = CServiceBroker::GetAppComponents();
      const auto tracePlayer = appComponents.GetComponent<CApplicationPlayer>();
      if (tracePlayer)
        overlaySig = tracePlayer->GetVisibleOverlaySetSignature(overlayAnimated);
      logComponentM(LOGDEBUG, LOGWINDOWING,
                    "osdtrace: f={} pass={} src={} prev={} why={} fill={} blit={}+{} blits={} "
                    "mem=[{}] run=[{}] focus=[{}] cov={} rend={} rects={} dirtyPx={:.0f} clip0={} "
                    "capped={} age={} ovl={}:{}",
                    m_osdTraceFrame, m_osdTracePass, traceSrc, m_osdTracePrevSource,
                    m_osdTraceReason, m_osdTraceFill, m_osdTraceBlitIndex, m_osdTraceLiveAfterBlit,
                    m_osdTraceBlitCount, members, running, focus, coverage, rendKind, frameRects,
                    frameDirtyPx, frameClip0, frameCapped ? 1 : 0, realAge, overlaySig,
                    overlayAnimated ? 1 : 0);
      logComponentM(LOGDEBUG, LOGWINDOWING, "osdgeom: f={} dirty=[{}] rgn=[{}] focusRgn=[{}]",
                    m_osdTraceFrame, damage, regions, focusRegion);
      m_osdTracePrevSource = traceSrc;
    }

    if (advancedSettings && advancedSettings->m_guiSkinHdrFbo)
    {
      auto& traceGfx = CServiceBroker::GetWinSystem()->GetGfxContext();
      const char* skinWhy = "ok";
      if (traceGfx.GetGuiHdr() == GuiHdr::SDR)
        skinWhy = "sdr";
      else if (traceGfx.GetStereoMode() != RENDER_STEREO_MODE_OFF)
        skinWhy = "stereo";
      else if (!renderSystem)
        skinWhy = "no-rendersystem";
      else if (!renderSystem->SupportsGuiRenderTargetConvert())
        skinWhy = "unsupported";
      else if (!skinHdrWanted)
        skinWhy = "window";
      std::string quads;
      unsigned int fboWidth = 0;
      unsigned int fboHeight = 0;
      if (m_skinHdrRenderTarget)
      {
        quads = FormatRectList(m_skinHdrRenderTarget->GetContentRects());
        fboWidth = m_skinHdrRenderTarget->GetWidth();
        fboHeight = m_skinHdrRenderTarget->GetHeight();
      }
      logComponentM(LOGDEBUG, LOGWINDOWING,
                    "skinhdr: f={} want={} why={} begun={} valid={} forceFull={} rend={} rects={} "
                    "dirtyPx={:.0f} age={} win={} fbo={}x{} gui={}x{} convert={} ret={} q=[{}]",
                    m_osdTraceFrame, skinHdrWanted ? 1 : 0, skinWhy, skinHdrBegun ? 1 : 0,
                    m_skinHdrTargetValid ? 1 : 0, skinHdrForceFull ? 1 : 0, rendKind, frameRects,
                    frameDirtyPx, realAge, traceActiveId, fboWidth, fboHeight, traceGfx.GetWidth(),
                    traceGfx.GetHeight(),
                    renderSystem && renderSystem->SupportsGuiRenderTargetConvert() ? 1 : 0,
                    m_osdTraceSkinRet, quads);
    }
  }

  static uint32_t s_bufAgeFull = 0, s_bufAgePartial = 0, s_bufAgeSkip = 0;
  static uint32_t s_bufAgeRects = 0, s_bufAgeClip0 = 0, s_bufAgeMeasured = 0;
  static uint32_t s_bufAgeMarkedRects = 0;
  static float s_bufAgeMarkedPct = 0.0f;
  static float s_bufAgeSnapGrowth = 0.0f;
  static float s_bufAgeDirtyPx = 0.0f;
  static uint32_t s_bufAgeCapped = 0;
  static uint32_t s_bufAgeHist[4] = {};
  static auto s_bufAgeWindow = std::chrono::steady_clock::now();

  if (bufferAgeLever >= 1 && bufAgeDiag)
  {
  ++s_bufAgeHist[realAge == 0 ? 0 : (realAge == 1 ? 1 : (realAge <= 3 ? 2 : 3))];
  if (frameMarkedRects)
  {
    ++s_bufAgeMeasured;
    s_bufAgeMarkedRects += frameMarkedRects;
    s_bufAgeMarkedPct += frameMarkedPct;
  }
  if (frameCapped)
    ++s_bufAgeCapped;
  if (renderedViaRegions)
  {
    ++s_bufAgePartial;
    s_bufAgeRects += frameRects;
    s_bufAgeClip0 += frameClip0;
    s_bufAgeSnapGrowth += frameSnapGrowth;
    s_bufAgeDirtyPx += frameDirtyPx;
  }
  else if (hasRendered)
    ++s_bufAgeFull;
  else
    ++s_bufAgeSkip;

  const auto bufAgeNow = std::chrono::steady_clock::now();
  if (bufAgeNow - s_bufAgeWindow >= std::chrono::seconds(1))
  {
    static uint32_t s_bufAgeLastEmitted[7] = {};
    static bool s_bufAgeEverEmitted = false;
    static auto s_bufAgeLastEmitTime = std::chrono::steady_clock::now();
    const uint32_t bufAgeCurrent[7] = {s_bufAgeHist[0], s_bufAgeHist[1], s_bufAgeHist[2],
                                       s_bufAgeHist[3], s_bufAgeFull,    s_bufAgePartial,
                                       s_bufAgeSkip};
    bool bufAgeChanged = !s_bufAgeEverEmitted;
    for (size_t i = 0; i < 7 && !bufAgeChanged; ++i)
    {
      const uint32_t delta = bufAgeCurrent[i] > s_bufAgeLastEmitted[i]
                                 ? bufAgeCurrent[i] - s_bufAgeLastEmitted[i]
                                 : s_bufAgeLastEmitted[i] - bufAgeCurrent[i];
      bufAgeChanged = delta > BUF_AGE_EMIT_TOLERANCE;
    }
    if (bufAgeChanged || bufAgeNow - s_bufAgeLastEmitTime >= BUF_AGE_EMIT_FLOOR)
    {
      const int gpuUtil = ReadSysfsInt("/sys/class/mpgpu/utilization");
      const int gpuUtilGl = ReadSysfsInt("/sys/class/mpgpu/util_gl");
      const int gpuUtilCl = ReadSysfsInt("/sys/class/mpgpu/util_cl");
      const int gpuFreq = ReadSysfsInt("/sys/class/mpgpu/cur_freq");
      const float rectsMean =
          s_bufAgePartial ? static_cast<float>(s_bufAgeRects) / s_bufAgePartial : 0.0f;
      const float snapMean = s_bufAgePartial ? s_bufAgeSnapGrowth / s_bufAgePartial : 0.0f;
      const float dirtyMean = s_bufAgePartial ? s_bufAgeDirtyPx / s_bufAgePartial : 0.0f;
      const float markedRectsMean =
          s_bufAgeMeasured ? static_cast<float>(s_bufAgeMarkedRects) / s_bufAgeMeasured : 0.0f;
      const float markedPctMean = s_bufAgeMeasured ? s_bufAgeMarkedPct / s_bufAgeMeasured : 0.0f;
      const float viewportPx =
          static_cast<float>(CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth()) *
          static_cast<float>(CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight());
      const float dirtyPct = viewportPx > 0.0f ? (dirtyMean / viewportPx) * 100.0f : 0.0f;
      logComponentM(LOGDEBUG, LOGWINDOWING,
                    "bufage: algo={} lever={} age={} hist={}/{}/{}/{} full={} partial={} skip={} measured={} "
                    "rects={:.2f} clip0={} snapPx={:.0f} dirtyPx={:.0f} dirtyPct={:.1f} capped={} "
                    "markedRects={:.2f} markedPct={:.1f} "
                    "gpu={}%(gl{}/cl{})@{}",
                    guiAlgorithmDirtyRegions, bufferAgeLever, realAge, s_bufAgeHist[0], s_bufAgeHist[1], s_bufAgeHist[2],
                    s_bufAgeHist[3], s_bufAgeFull, s_bufAgePartial, s_bufAgeSkip, s_bufAgeMeasured, rectsMean,
                    s_bufAgeClip0, snapMean, dirtyMean, dirtyPct, s_bufAgeCapped, markedRectsMean,
                    markedPctMean, gpuUtil,
                    gpuUtilGl, gpuUtilCl, gpuFreq);
      for (size_t i = 0; i < 7; ++i)
        s_bufAgeLastEmitted[i] = bufAgeCurrent[i];
      s_bufAgeEverEmitted = true;
      s_bufAgeLastEmitTime = bufAgeNow;
    }
    s_bufAgeWindow = bufAgeNow;
    s_bufAgeFull = s_bufAgePartial = s_bufAgeSkip = s_bufAgeRects = s_bufAgeClip0 = 0;
    s_bufAgeMeasured = s_bufAgeMarkedRects = 0;
    s_bufAgeMarkedPct = 0.0f;
    s_bufAgeSnapGrowth = 0.0f;
    s_bufAgeDirtyPx = 0.0f;
    s_bufAgeCapped = 0;
    s_bufAgeHist[0] = s_bufAgeHist[1] = s_bufAgeHist[2] = s_bufAgeHist[3] = 0;
  }
  }

  if (visualizeDirtyRegions)
  {
    CServiceBroker::GetWinSystem()->GetGfxContext().SetRenderingResolution(CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(), false);
    const CDirtyRegionList &markedRegions  = m_tracker.GetMarkedRegions();
    for (const auto& i : markedRegions)
      CGUITexture::DrawQuad(i, 0x0fff0000);
    for (const auto& i : dirtyRegions)
      CGUITexture::DrawQuad(i, 0x4c00ff00);
  }

  return hasRendered;
}

void CGUIWindowManager::AfterRender()
{
  CServiceBroker::GetWinSystem()->GetGfxContext().ResetDepth();
  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
    pWindow->AfterRender();

  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings =
      settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
  const bool scopedOsdRemark =
      advancedSettings && advancedSettings->m_guiBufferAgePartialRedraw >= 1 &&
      advancedSettings->m_guiBufferAgeAfterRenderScope && pWindow &&
      (pWindow->GetID() & WINDOW_ID_MASK) == WINDOW_FULLSCREEN_VIDEO;

  // make copy of vector as we may remove items from it as we go
  auto activeDialogs = m_activeDialogs;
  for (const auto& window : activeDialogs)
  {
    if (window->IsDialogRunning())
    {
      window->AfterRender();
      // Dialog state can affect visibility states
      if (pWindow && window->IsControlDirty() &&
          (!scopedOsdRemark || window->IsAnimating(ANIM_TYPE_WINDOW_OPEN) ||
           window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE) ||
           window->IsAnimating(ANIM_TYPE_VISIBLE) || window->IsAnimating(ANIM_TYPE_HIDDEN)))
        pWindow->MarkDirtyRegion();
    }
  }
}

void CGUIWindowManager::FrameMove()
{
  assert(CServiceBroker::GetAppMessenger()->IsProcessThread());
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  if(m_iNested == 0)
  {
    // delete any windows queued for deletion
    for (const auto& window : m_deleteWindows)
    {
      // Free any window resources
      window->FreeResources(true);
    }
    m_deleteWindows.clear();
  }

  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
    pWindow->FrameMove();
  // update any dialogs - we take a copy of the vector as some dialogs may close themselves
  // during this call
  auto dialogs = m_activeDialogs;
  for (const auto& window : dialogs)
  {
    window->FrameMove();
  }

  CServiceBroker::GetGUI()->GetInfoManager().UpdateAVInfo();
}

CGUIDialog* CGUIWindowManager::GetDialog(int id) const
{
  CGUIWindow *window = GetWindow(id);
  if (window && window->IsDialog())
    return dynamic_cast<CGUIDialog*>(window);
  return nullptr;
}

CGUIWindow* CGUIWindowManager::GetWindow(int id) const
{
  if (id == 0 || id == WINDOW_INVALID)
    return nullptr;

  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  auto it = m_mapWindows.find(id);
  if (it != m_mapWindows.end())
    return it->second.get();
  return nullptr;
}

bool CGUIWindowManager::ProcessRenderLoop(bool renderOnly)
{
  bool renderGui = true;

  if (CServiceBroker::GetAppMessenger()->IsProcessThread() && m_pCallback)
  {
    renderGui = m_pCallback->GetRenderGUI();
    m_iNested++;
    if (!renderOnly)
      m_pCallback->Process();
    {
      CSingleExit leaveIt(CServiceBroker::GetWinSystem()->GetGfxContext());
      m_pCallback->FrameMove(!renderOnly);
    }
    m_pCallback->Render();
    m_iNested--;
  }
  if (g_application.m_bStop || !renderGui)
    return false;
  else
    return true;
}

void CGUIWindowManager::SetCallback(IWindowManagerCallback& callback)
{
  m_pCallback = &callback;
}

void CGUIWindowManager::DeInitialize()
{
  StopFullscreenOverlayRenderThread();

  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  // Need a copy because addon-dialogs removes itself on Close()
  // Copy shared_ptrs to keep windows alive during cleanup
  std::vector<std::shared_ptr<CGUIWindow>> windowsCopy;
  windowsCopy.reserve(m_mapWindows.size());
  for (const auto& entry : m_mapWindows)
    windowsCopy.emplace_back(entry.second);

  for (const auto& pWindow : windowsCopy)
  {
    if (IsWindowActive(pWindow->GetID(), false))
    {
      pWindow->DisableAnimations();
      pWindow->Close(true);
    }
    pWindow->ResetControlStates();
    pWindow->FreeResources(true);
  }
  UnloadNotOnDemandWindows();

  m_vecMsgTargets.erase( m_vecMsgTargets.begin(), m_vecMsgTargets.end() );

  // destroy our custom windows...
  for (const auto& pWindow : m_vecCustomWindows)
  {
    RemoveFromWindowHistory(pWindow->GetID());
    Remove(pWindow->GetID());
  }

  // clear our vectors of windows
  m_vecCustomWindows.clear();
  m_activeDialogs.clear();

  m_initialized = false;
}

/// \brief Unroute window
/// \param id ID of the window routed
void CGUIWindowManager::RemoveDialog(int id)
{
  std::unique_lock<CCriticalSection> lock(CServiceBroker::GetWinSystem()->GetGfxContext());
  m_activeDialogs.erase(std::remove_if(m_activeDialogs.begin(), m_activeDialogs.end(),
                                       [id](const std::shared_ptr<CGUIWindow>& dialog)
                                       { return dialog->GetID() == id; }),
                        m_activeDialogs.end());
}

bool CGUIWindowManager::HasModalDialog(bool ignoreClosing) const
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  for (const auto& window : m_activeDialogs)
  {
    if (window->IsDialog() &&
        window->IsModalDialog() &&
        (!ignoreClosing || !window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE)))
    {
      return true;
    }
  }
  return false;
}

bool CGUIWindowManager::HasVisibleDialogContentInRegions(const std::vector<CRect>& regions) const
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  for (const auto& window : m_activeDialogs)
  {
    if (window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE))
      continue;
    if (window->HasVisibleControlInRegions(regions))
      return true;
  }
  return false;
}

bool CGUIWindowManager::HasVisibleModalDialog() const
{
  return HasModalDialog(false);
}

int CGUIWindowManager::GetTopmostDialog(bool modal, bool ignoreClosing) const
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  for (auto it = m_activeDialogs.rbegin(); it != m_activeDialogs.rend(); ++it)
  {
    if ((!modal || (*it)->IsModalDialog()) &&
        (!ignoreClosing || !(*it)->IsAnimating(ANIM_TYPE_WINDOW_CLOSE)))
      return (*it)->GetID();
  }
  return WINDOW_INVALID;
}

int CGUIWindowManager::GetTopmostDialog(bool ignoreClosing /*= false*/) const
{
  return GetTopmostDialog(false, ignoreClosing);
}

int CGUIWindowManager::GetTopmostModalDialog(bool ignoreClosing /*= false*/) const
{
  return GetTopmostDialog(true, ignoreClosing);
}

void CGUIWindowManager::SendThreadMessage(CGUIMessage& message, int window /*= 0*/)
{
  std::lock_guard lock(m_critSection);

  auto msg = new CGUIMessage(message);
  m_vecThreadMessages.emplace_back(msg, window);
}

void CGUIWindowManager::DispatchThreadMessages()
{
  // This method only be called in the xbmc main thread.

  // XXX: for more info of this method
  //      check the pr here: https://github.com/xbmc/xbmc/pull/2253

  // As a thread message queue service, it should follow these rules:
  // 1. [Must] Thread safe, message can be pushed into queue in arbitrary thread context.
  // 2. Messages [must] be processed in dispatch message thread context with the same
  //    order as they be pushed into the queue.
  // 3. Dispatch function [must] support call itself during message process procedure,
  //    and do not break other rules listed here. to make it clear: in the
  //    SendMessage(), it could start another xbmc main thread loop, calling
  //    DispatchThreadMessages() in it's internal loop, this must be supported.
  // 4. During DispatchThreadMessages() processing, any new pushed message [should] not
  //    be processed by the current loop in DispatchThreadMessages(), prevent dead loop.
  // 5. If possible, queued messages can be removed by certain filter condition
  //    and not break above.

  {
    std::unique_lock lock(m_critSection);

    // Optimize: Remove redundant messages for idempotent operations
    // Only keep the latest message for each (message_type, window, control) tuple
    // for message types where only the final value matters
    if (m_vecThreadMessages.size() > 1)
    {
      struct MsgKey
      {
        int message;
        int window;
        int control;
      };

      struct MsgKeyHash
      {
        size_t operator()(const MsgKey& key) const noexcept
        {
          size_t h = std::hash<int>{}(key.message);
          h ^= (std::hash<int>{}(key.window) + 0x9e3779b9 + (h << 6) + (h >> 2));
          h ^= (std::hash<int>{}(key.control) + 0x9e3779b9 + (h << 6) + (h >> 2));
          return h;
        }
      };

      struct MsgKeyEq
      {
        bool operator()(const MsgKey& a, const MsgKey& b) const noexcept
        {
          return a.message == b.message && a.window == b.window && a.control == b.control;
        }
      };

      const auto shouldDedupe = [](int message)
      {
        return message == GUI_MSG_LABEL_SET || message == GUI_MSG_LABEL2_SET;
      };

      // Iterate from newest -> oldest and drop older duplicates (keep latest).
      std::unordered_set<MsgKey, MsgKeyHash, MsgKeyEq> seen;
      for (auto it = m_vecThreadMessages.rbegin(); it != m_vecThreadMessages.rend();)
      {
        CGUIMessage* msg = it->first;
        const int win = it->second;
        const int message = msg->GetMessage();

        if (shouldDedupe(message))
        {
          MsgKey key{message, win, msg->GetControlId()};
          if (!seen.insert(key).second)
          {
            delete msg;
            auto forwardIt = std::prev(it.base());
            auto nextForward = m_vecThreadMessages.erase(forwardIt);
            it = std::make_reverse_iterator(nextForward);
            continue;
          }
        }

        ++it;
      }
    }

  }

  std::unique_lock lock(m_critSection);

  for (auto msgCount = m_vecThreadMessages.size(); msgCount > 0; --msgCount)
  {
    if (m_vecThreadMessages.empty())
      break;

    // pop up one message per time to make messages be processed by order.
    // this will ensure rule No.2 & No.3
    CGUIMessage* pMsg = m_vecThreadMessages.front().first;
    int window = m_vecThreadMessages.front().second;
    m_vecThreadMessages.pop_front();

    lock.unlock();

    // XXX: during SendMessage(), there could be a deeper 'xbmc main loop' inited by e.g. doModal
    //      which may loop there and callback to DispatchThreadMessages() multiple times.
    if (window)
      SendMessage(*pMsg, window);
    else
      SendMessage(*pMsg);
    delete pMsg;

    lock.lock();
  }
}

int CGUIWindowManager::RemoveThreadMessageByMessageIds(int *pMessageIDList)
{
  std::lock_guard lock(m_critSection);

  int removedMsgCount = 0;
  for (auto it = m_vecThreadMessages.begin();
       it != m_vecThreadMessages.end();)
  {
    CGUIMessage *pMsg = it->first;
    int *pMsgID;
    for(pMsgID = pMessageIDList; *pMsgID != 0; ++pMsgID)
      if (pMsg->GetMessage() == *pMsgID)
        break;
    if (*pMsgID)
    {
      it = m_vecThreadMessages.erase(it);
      delete pMsg;
      ++removedMsgCount;
    }
    else
    {
      ++it;
    }
  }
  return removedMsgCount;
}

void CGUIWindowManager::AddMsgTarget(IMsgTargetCallback* pMsgTarget)
{
  m_vecMsgTargets.emplace_back(pMsgTarget);
}

void CGUIWindowManager::RemoveMsgTarget(IMsgTargetCallback* pMsgTarget)
{
  m_vecMsgTargets.erase(std::remove(m_vecMsgTargets.begin(), m_vecMsgTargets.end(), pMsgTarget),
                        m_vecMsgTargets.end());
}

int CGUIWindowManager::GetActiveWindow() const
{
  if (!m_windowHistory.empty())
    return m_windowHistory.back();
  return WINDOW_INVALID;
}

int CGUIWindowManager::GetActiveWindowOrDialog() const
{
  // if there is a dialog active get the dialog id instead
  int iWin = GetTopmostModalDialog() & WINDOW_ID_MASK;
  if (iWin != WINDOW_INVALID)
    return iWin;

  // get the currently active window
  return GetActiveWindow() & WINDOW_ID_MASK;
}

bool CGUIWindowManager::IsWindowActive(int id, bool ignoreClosing /* = true */) const
{
  // mask out multiple instances of the same window
  id &= WINDOW_ID_MASK;
  if ((GetActiveWindow() & WINDOW_ID_MASK) == id)
    return true;
  // run through the dialogs
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  for (const auto& window : m_activeDialogs)
  {
    if ((window->GetID() & WINDOW_ID_MASK) == id && (!ignoreClosing || !window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE)))
      return true;
  }
  return false; // window isn't active
}

bool CGUIWindowManager::IsWindowActive(const std::string &xmlFile, bool ignoreClosing /* = true */) const
{
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  CGUIWindow *window = GetWindow(GetActiveWindow());
  if (window && StringUtils::EqualsNoCase(URIUtils::GetFileName(window->GetProperty("xmlfile").asString()), xmlFile))
    return true;
  // run through the dialogs
  for (const auto& window : m_activeDialogs)
  {
    if (StringUtils::EqualsNoCase(URIUtils::GetFileName(window->GetProperty("xmlfile").asString()), xmlFile) &&
        (!ignoreClosing || !window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE)))
      return true;
  }
  return false; // window isn't active
}

bool CGUIWindowManager::IsWindowVisible(int id) const
{
  return IsWindowActive(id, false);
}

bool CGUIWindowManager::IsWindowVisible(const std::string &xmlFile) const
{
  return IsWindowActive(xmlFile, false);
}

void CGUIWindowManager::LoadNotOnDemandWindows() const {
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  for (const auto& entry : m_mapWindows)
  {
    const auto& pWindow = entry.second;
    if (pWindow->GetLoadType() == CGUIWindow::LOAD_ON_GUI_INIT)
    {
      pWindow->FreeResources(true);
      pWindow->Initialize();
    }
  }
}

void CGUIWindowManager::UnloadNotOnDemandWindows() const {
  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  for (const auto& entry : m_mapWindows)
  {
    const auto& pWindow = entry.second;
    if (pWindow->GetLoadType() == CGUIWindow::LOAD_ON_GUI_INIT ||
        pWindow->GetLoadType() == CGUIWindow::KEEP_IN_MEMORY)
    {
      pWindow->FreeResources(true);
    }
  }
}

void CGUIWindowManager::AddToWindowHistory(int newWindowID)
{
  // Check the window stack to see if this window is in our history,
  // and if so, pop all the other windows off the stack so that we
  // always have a predictable "Back" behaviour for each window
  std::deque<int> history = m_windowHistory;
  while (!history.empty())
  {
    if (history.back() == newWindowID)
      break;
    history.pop_back();
  }
  if (!history.empty())
  { // found window in history
    m_windowHistory.swap(history);
  }
  else
  {
    // didn't find window in history - add it to the stack
    m_windowHistory.emplace_back(newWindowID);
  }
}

void CGUIWindowManager::RemoveFromWindowHistory(int windowID)
{
  std::deque<int> history = m_windowHistory;

  // pop windows from stack until we found the window
  while (!history.empty())
  {
    if (history.back() == windowID)
      break;
    history.pop_back();
  }

  // found window in history
  if (!history.empty())
  {
    history.pop_back(); // remove window from stack
    m_windowHistory.swap(history);
  }
}

bool CGUIWindowManager::IsModalDialogTopmost(int id) const
{
  return IsDialogTopmost(id, true);
}

bool CGUIWindowManager::IsModalDialogTopmost(const std::string &xmlFile) const
{
  return IsDialogTopmost(xmlFile, true);
}

bool CGUIWindowManager::IsDialogTopmost(int id, bool modal /* = false */) const
{
  CGUIWindow *topmost = GetWindow(GetTopmostDialog(modal, false));
  if (topmost && (topmost->GetID() & WINDOW_ID_MASK) == id)
    return true;
  return false;
}

bool CGUIWindowManager::IsDialogTopmost(const std::string &xmlFile, bool modal /* = false */) const
{
  CGUIWindow *topmost = GetWindow(GetTopmostDialog(modal, false));
  if (topmost && StringUtils::EqualsNoCase(URIUtils::GetFileName(topmost->GetProperty("xmlfile").asString()), xmlFile))
    return true;
  return false;
}

bool CGUIWindowManager::HasVisibleControls() const {
  CSingleExit lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  if (m_activeDialogs.empty())
  {
    CGUIWindow *window(GetWindow(GetActiveWindow()));
    return !window || window->HasVisibleControls();
  }
  else
    return true;
}

void CGUIWindowManager::ClearWindowHistory()
{
  while (!m_windowHistory.empty())
    m_windowHistory.pop_back();
}

void CGUIWindowManager::CloseWindowSync(CGUIWindow *window, int nextWindowID /*= 0*/)
{
  // Abort touch action if active
  if (m_touchGestureActive && !m_inhibitTouchGestureEvents)
  {
    CLog::Log(LOGDEBUG, "Closing window {} with active touch gesture, sending gesture abort event",
              window->GetID());
    window->OnAction({ACTION_GESTURE_ABORT});
    // Don't send any mid-gesture events to next window until new touch starts
    m_inhibitTouchGestureEvents = true;
  }

  window->Close(false, nextWindowID);

  bool renderLoopProcessed = true;
  while (window->IsAnimating(ANIM_TYPE_WINDOW_CLOSE) && renderLoopProcessed)
    renderLoopProcessed = ProcessRenderLoop(true);
}

#ifdef _DEBUG
void CGUIWindowManager::DumpTextureUse()
{
  CGUIWindow* pWindow = GetWindow(GetActiveWindow());
  if (pWindow)
    pWindow->DumpTextureUse();

  std::lock_guard lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  for (const auto& window : m_activeDialogs)
  {
    if (window->IsDialogRunning())
      window->DumpTextureUse();
  }
}
#endif
