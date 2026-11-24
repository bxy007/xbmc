/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BlurayStateSerializer.h"
#include "DVDInputStream.h"
#include "cores/AudioEngine/Interfaces/AE.h"

#include <array>
#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <optional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class CMVCSubtitleOffsets;
class CDVDOverlay;
struct BlurayFileAccess;
struct BlurayPipClip;
struct BlurayPipSelection;

extern "C"
{
#include <libbluray/bluray.h>
#include <libbluray/bluray-version.h>
#include <libbluray/filesystem.h>
#include <libbluray/keys.h>
#include <libbluray/overlay.h>
#include <libbluray/player_settings.h>
}

#define MAX_PLAYLIST_ID 99999
#define MAX_CLIP_ID 99999
#define BD_EVENT_MENU_OVERLAY -1
#define BD_EVENT_MENU_ERROR   -2
#define BD_EVENT_ENC_ERROR    -3

#define HDMV_PID_VIDEO            0x1011
#define HDMV_PID_VIDEO_EL         0x1015
#define HDMV_PID_AUDIO_FIRST      0x1100
#define HDMV_PID_AUDIO_LAST       0x111f
#define HDMV_PID_PG_FIRST         0x1200
#define HDMV_PID_PG_LAST          0x121f
#define HDMV_PID_PG_HDR_FIRST     0x12a0
#define HDMV_PID_PG_HDR_LAST      0x12bf
#define HDMV_PID_IG_FIRST         0x1400
#define HDMV_PID_IG_LAST          0x141f

class CDVDOverlayImage;
class IVideoPlayer;
class CDVDDemux;

class CDVDInputStreamBluray
  : public CDVDInputStream
  , public CDVDInputStream::IDisplayTime
  , public CDVDInputStream::IChapter
  , public CDVDInputStream::IPosTime
  , public CDVDInputStream::IMenus
  , public CDVDInputStream::IExtentionStream
{
public:
  CDVDInputStreamBluray() = delete;
  CDVDInputStreamBluray(IVideoPlayer* player, const CFileItem& fileitem);
  ~CDVDInputStreamBluray() override;
  bool Open() override;
  void Close() override;
  int Read(uint8_t* buf, int buf_size) override;
  int64_t Seek(int64_t offset, int whence) override;
  void Abort() override;
  bool IsEOF() override;
  int64_t GetLength() override;
  int GetBlockSize() override { return 6144; }
  ENextStream NextStream() override;


  /* IMenus */
  void ActivateButton() override { UserInput(BD_VK_ENTER); }
  void SelectButton(int iButton) override
  {
    if(iButton < 10)
      UserInput((bd_vk_key_e)(BD_VK_0 + iButton));
  }
  int  GetCurrentButton() override { return 0; }
  int  GetTotalButtons() override { return 0; }
  void OnUp() override  { UserInput(BD_VK_UP); }
  void OnDown() override  { UserInput(BD_VK_DOWN); }
  void OnLeft() override { UserInput(BD_VK_LEFT); }
  void OnRight() override { UserInput(BD_VK_RIGHT); }

  /*! \brief Open the Menu
  * \return true if the menu is successfully opened, false otherwise
  */
  bool OnMenu(MenuCall call) override;
  bool OnColorKey(int key) override;
  bool CanClosePopup() const override;
  void ClosePopup() override;
  void OnBack() override
  {
    if (!IsInMenu())
      return;
    if (m_hasVisibleBdjOverlay && !m_isInMainMenu)
    {
      UserInput(BD_VK_POPUP);
      return;
    }
    OnMenu(MenuCall::Auto);
  }
  void OnNext() override {}
  void OnPrevious() override {}

  /*!
   * \brief Get the supported menu type
   * \return The supported menu type
  */
  MenuType GetSupportedMenuType() override;


  bool IsInMenu() override;
  bool IsPopupOpen() const;
  bool IsMenuDomainSegment() const;
  bool IsReadInDataPhase() const
  {
    const EHoldState hold = m_hold.load();
    return hold == HOLD_NONE || hold == HOLD_DATA;
  }
  bool IsFeaturePlaylistActive();
  bool IsFeaturePlaylistLoaded();
  bool IsInMainMenu() const { return m_isInMainMenu.load(std::memory_order_relaxed); }
  bool IsOnFeaturePlaylist();
  bool OnMouseMove(const CPoint &point) override  { return MouseMove(point); }
  bool OnMouseClick(const CPoint &point) override { return MouseClick(point); }
  void SkipStill() override;
  bool ConsumeDiscontinuityFlush() override;
  bool GetSeamTimeOffsets(int& generation, double& current, double& previous) override
  {
    std::lock_guard<std::mutex> lock(m_seamOffsetMutex);
    generation = m_seamGeneration;
    current = m_seamTimeOffset;
    previous = m_seamTimeOffsetPrev;
    return true;
  }
  bool GetState(std::string& xmlstate) override;
  bool SetState(const std::string& xmlstate) override;
  bool CanSeek() override;


  void UserInput(bd_vk_key_e vk);
  bool MouseMove(const CPoint &point) const;
  bool MouseClick(const CPoint &point);

  bool IsNavMode() const { return m_navmode; }
  bool IsTitleEnded() const { return m_endedTitleTime > 0; }
  bool IsTitleEndWait() const { return m_titleEndWait; }
  bool IsTitleEndHeld() const;
  bool IsTailInTitle() const;
  bool IsReadHeld() const;
  bool TakeItemAbandoned();
  bool IsJumpPending() const { return m_itemAbandoned; }
  void PollEvents();
  void SetPresentedTime(double pts);
  void SetPresentationClock(double pts, double speed);
  void SetReadPresented();
  void ReportItemPlayedOut();
  void ResetPresentedLeft() { m_presentedLeft = false; }
  void SetLastPictureTime(double lastVideoPts, double clockPts);
  std::optional<double> GetHeldItemEnd() const;
  bool NextItemContinuesPlaylist() const { return m_nextItemInPlaylist; }
  std::optional<int> GetNextItemSeekTime() const { return m_nextItemSeekTime; }

  bool ListsEnhancementLayer(int pid) const;
  bool IsSecondaryVideo(int pid) const;
  std::shared_ptr<const BlurayPipClip> GetPipClip() const;
  BlurayPipSelection GetPipSelection(int speed) const;
  bool GetDemuxerGraphicsPlane(int pid, int& width, int& height) const;
  bool GetDemuxerFrameRate(int pid, int& rate, int& scale) const;

  bool SetActiveAudioStream(int pid);
  bool SetActiveSubtitleStream(int pid, bool visible);
  bool SetSubtitleVisible(bool visible);
  bool CanSelectStream() const;
  bool CanChangeAudioStream() const;
  bool CanChangeSubtitleStream() const;
  bool CanToggleSubtitle() const;
  uint64_t GetSubtitleSelectGeneration() const
  {
    return m_subtitleSelectGen.load(std::memory_order_acquire);
  }
  bool IsPgEnabled() const { return m_pgEnabled; }
  bool IsAudioStreamPick() const { return m_audioStreamPick; }
  bool IsPgStreamPick() const { return m_pgStreamPick; }

  int GetChapter() override;
  int GetChapterCount() override;
  void GetChapterName(std::string& name, int ch = -1) override;
  int64_t GetChapterPos(int ch) override;
  bool SeekChapter(int ch) override;

  CDVDInputStream::IDisplayTime* GetIDisplayTime() override { return this; }
  int GetTotalTime() override;
  int GetTime() override;

  CDVDInputStream::IPosTime* GetIPosTime() override { return this; }
  bool PosTime(int ms) override;

  void GetStreamInfo(int pid, std::string &language) const;

  void GetDiscStreamHdrMetadata(int pid, bool& isDolbyVision, bool& isHdrPlus) const;

  void UpdateGraphicsRegime();

  void EnableStream(uint32_t blurayStreamType, int pid, bool enable);

  int Get3dSubtitlePlane(uint16_t pid) const;
  const std::shared_ptr<CMVCSubtitleOffsets>& GetMVCSubtitleOffsets() const
  {
    return m_mvcSubtitleOffsets;
  }

  void OverlayCallback(const BD_OVERLAY * const);
#ifdef HAVE_LIBBLURAY_BDJ
  void OverlayCallbackARGB(const struct bd_argb_overlay_s * const);
#endif

  BLURAY_TITLE_INFO* GetTitleFromState(const std::string& xmlstate) const;
  BLURAY_TITLE_INFO* GetMainTitle() const;
  BLURAY_TITLE_INFO* GetTitleLongest(int titles = -1) const;
  BLURAY_TITLE_INFO* GetTitleFile(const std::string& name) const;

  void ProcessEvent();
  void RequestMenuOverlayRepost() { m_repostMenuOverlay = true; }
  bool ConsumeVideoCompatBoundary()
  {
    const bool compat = m_videoCompatBoundary;
    m_videoCompatBoundary = false;
    return compat;
  }
  bool ConsumeNaturalChainBoundary()
  {
    const bool natural = m_naturalChainBoundary;
    m_naturalChainBoundary = false;
    return natural;
  }
  bool IsNaturalChainBoundaryInFlight() const
  {
    return !m_bMVCPlayback && (m_naturalChainBoundary || m_crossPlaylistPending || m_atTitleEnd);
  }
  CDVDDemux* GetExtentionDemux() override { return m_pMVCDemux; };
  bool HasExtention() override { return m_bMVCPlayback; }
  bool AreEyesFlipped() override { return m_bFlipEyes; }
  void DisableExtention() override;
  bool OpenNextStream() override;
  void OnStereoStreamUnrecoverable() override { m_stereoUnrecoverable.store(true, std::memory_order_relaxed); }
  bool ConsumeStereoResyncRequest() override
  {
    if (!m_stereoResyncRequested.load(std::memory_order_relaxed))
      return false;
    m_stereoResyncRequested.store(false, std::memory_order_relaxed);
    return true;
  }
  bool ExtentionAbortRequested() override
  {
    return m_extAbortRequested.load(std::memory_order_relaxed);
  }

protected:
  struct SPlane;

  bool OpenPopup();
  void UpdateBdjPopup(bool visible);
  void OverlayFlush(int64_t pts);
  void DeliverParkedOverlayIfDue();
  void OverlayClose(bool deferrable = false);
  static void OverlayClear(SPlane& plane, int x, int y, int w, int h);
  static void OverlayInit (SPlane& plane, int w, int h);
  void InitBdjSurfaceLocked(int w, int h);
  void ResetBdjSurfaceLocked();
  bool ProcessItem(int playitem);
  void FreePrevTitleInfo();
  void StashBoundaryClip();
  void ReleaseTitle();
  void ResetTitleEnd();
  void CheckPresentedLeft();
  void ReportPresented(int64_t titleTime);
  void UpdatePendingGraphicsRegime(int64_t titleTime);
  void RecordTitleTimeSpan(const BLURAY_CLIP_INFO* clip, bool reset);
  int64_t ConvertPtsToTitleTime90k(double ptsDvdTime) const;
  void SelectPendingStreams();
  void PollHeldJoin();
  bool TakeHeldEvent();
  bool ProcessHeldEvents();
  bool TakeMenuJump(bool joinsInLine, bool dataRead);
  bool AbandonsItem(bool menuJump, bool fromStill) const;
  void MarkMenuInput();
  bool IsJoinHeld() const;
  const BLURAY_CLIP_INFO* GetActiveClipLocked() const;
  void LoadPipLayouts(unsigned int playItem);
  void UpdateSeamTimeOffset(const BLURAY_CLIP_INFO* prev, const BLURAY_CLIP_INFO* next);
  void ResetSeamTimeOffset(const char* reason);
  static bool AreClipVideoStreamsCompatible(const BLURAY_CLIP_INFO* a, const BLURAY_CLIP_INFO* b);
  static bool AreClipPgStreamsEqual(const BLURAY_CLIP_INFO* a, const BLURAY_CLIP_INFO* b);

  bool OpenMVCDemux(int playItem);
  bool CloseMVCDemux();
  void SeekMVCDemux(int64_t time);

  IVideoPlayer* m_player = nullptr;
  BLURAY* m_bd = nullptr;
  const BLURAY_TITLE* m_title = nullptr;
  mutable std::mutex m_clipTableMutex;
  BLURAY_TITLE_INFO* m_titleInfo = nullptr;
  uint32_t m_playlist = MAX_PLAYLIST_ID + 1;
  uint32_t m_featurePlaylist = MAX_PLAYLIST_ID + 1;
  BLURAY_CLIP_INFO* m_clip = nullptr;
  std::shared_ptr<const BlurayPipClip> m_pipClip;
  unsigned int m_secondaryVideoStream{0};
  bool m_secondaryVideoEnabled{false};
  bool m_secondaryVideoFullscreen{false};
  uint32_t m_angle = 0;
  std::atomic<uint32_t> m_titleNumber{0xfffffffe};
  std::atomic_bool m_atTitleEnd{false};
  bool m_wrapSeekExempt = false;
  bool m_crossPlaylistPending = false;
  bool m_videoCompatBoundary = false;
  bool m_naturalChainBoundary = false;
  bool m_seamlessPlayItem = false;
  BLURAY_TITLE_INFO* m_prevTitleInfo = nullptr;
  const BLURAY_CLIP_INFO* m_prevClip = nullptr;
  uint32_t m_prevPlaylist = MAX_PLAYLIST_ID + 1;
  bool m_prevWasMVC = false;
  bool m_prevFlipEyes = false;
  std::atomic_bool m_menu{false};
  std::atomic_bool m_isBdjTitle{false};
  std::atomic_bool m_isInMainMenu{false};
  std::atomic<uint64_t> m_subtitleSelectGen{0};
  std::atomic_bool m_discontinuityFlush{false};
  std::atomic_bool m_hasOverlay{false};
  std::atomic_bool m_hasMenuOverlay{false};
  std::atomic_bool m_hasVisibleBdjOverlay{false};
  std::atomic_bool m_popupAvailable{false};
  enum class BdjPopup
  {
    CLOSED,
    OPENING,
    OPEN,
    CLOSING,
  };
  std::atomic<BdjPopup> m_bdjPopup{BdjPopup::CLOSED};
  std::atomic_bool m_overlayCloseDeferred{false};
  std::atomic_bool m_repostMenuOverlay{false};
  std::atomic_bool m_navmode{false};
  std::atomic_bool m_pqAuthoredGraphics{false};
  std::mutex m_seamOffsetMutex;
  int m_seamGeneration = 0;
  double m_seamTimeOffset = 0.0;
  double m_seamTimeOffsetPrev = 0.0;
  struct TitleTimeSpan
  {
    int64_t ptsThreshold90k{0};
    int64_t base90k{0};
  };
  mutable std::mutex m_titleTimeSpanMutex;
  std::vector<TitleTimeSpan> m_titleTimeSpans;
  int m_endedTitleTime{0};
  bool m_titleEndWait{false};
  int64_t m_lastPictureTime{-1};
  int64_t m_presentedTime{-1};
  int64_t m_clockTime{-1};
  std::atomic_bool m_menuInput{false};
  bool m_itemAbandoned{false};
  std::queue<BD_EVENT> m_heldEvents;
  bool m_notificationsHeld{false};
  std::optional<bool> m_pendingPqGraphics;
  int64_t m_pendingPqGraphicsStart{0};
  bool m_presentedLeft{false};
  bool m_readSinceReport{false};
  int64_t m_heldDue{-2};
  int64_t m_reportedTime{-1};
  int64_t m_playedItemEnd{-1};
  bool m_playlistLeft{false};
  bool m_nextItemInPlaylist{false};
  bool m_itemSeeked{false};
  std::optional<int> m_nextItemSeekTime;
  uint32_t m_pgStreamNumber{0};
  uint32_t m_audioStreamNumber{0};
  bool m_audioStreamPick{false};
  bool m_pgStreamPick{false};
  bool m_pgEnabled{false};
  std::optional<int> m_audioPidPending;
  std::optional<int> m_pgPidPending;
  std::optional<bool> m_pgVisiblePending;
  std::atomic_bool m_bdjGraphicsActive{false};
  std::atomic<uint32_t> m_uoMask{0};
  bool m_bdStillActive = false;
  bool m_topMenuIsBdj = false;
  static constexpr size_t FLUSH_WINDOW = 5;
  std::array<std::chrono::steady_clock::time_point, FLUSH_WINDOW> m_recentFlushes{};
  size_t m_flushIdx = 0;
  std::atomic_bool m_menuBurstObserved{false};
  std::atomic<uint32_t> m_menuBurstPlaylist{MAX_PLAYLIST_ID + 1};
  std::atomic<uint32_t> m_menuRestorePlaylist{MAX_PLAYLIST_ID + 1};
  bool TitleCarriesAlwaysOnMenuComposition() const;
  bool PlaylistWithinMenuDurationBound() const;
  bool IsInMainOrHdmvMenu() const;
  mutable std::atomic<int> m_lastIsInMenuLogged{-1};
  mutable std::atomic<int> m_lastMenuDomainLogged{-1};
  int m_dispTimeBeforeRead = 0;
  std::chrono::steady_clock::time_point m_vmDiagWindow{};
  std::chrono::steady_clock::time_point m_vmDiagLastRead{};
  uint32_t m_vmDiagReads = 0;
  uint32_t m_vmDiagReadGapMaxUs = 0;
  uint32_t m_vmDiagArgbDraws = 0;
  uint64_t m_vmDiagArgbBytes = 0;
  uint32_t m_vmDiagHdmvDraws = 0;
  uint32_t m_vmDiagFlushes = 0;
  uint32_t m_vmDiagLockHoldMaxUs = 0;
  uint32_t m_vmDiagPlaneMax = 0;
  std::chrono::steady_clock::time_point m_vmDiagPlaylistEnter{};
  int m_vmDiagPlaylist = -1;
  std::string         m_root;

  // MVC related members
  CDVDDemux*          m_pMVCDemux = nullptr;
  CDVDInputStream    *m_pMVCInput = nullptr;
  bool                m_bMVCPlayback = false;
  std::atomic<bool>   m_stereoUnrecoverable{false};
  std::atomic<bool>   m_stereoResyncRequested{false};
  std::atomic<bool>   m_extAbortRequested{false};
  int                 m_nMVCSubPathIndex = 0;
  BLURAY_CLIP_INFO*   m_nMVCClip = nullptr;
  bool                m_bFlipEyes = false;
  bool                m_bMVCDisabled = false;
  const std::shared_ptr<CMVCSubtitleOffsets> m_mvcSubtitleOffsets;
  uint64_t            m_clipStartTime = 0;
  unsigned int        m_endOfTitleSpin = 0;
  std::chrono::steady_clock::time_point m_endOfTitleSpinStart{};
  std::queue<int>     m_clipQueue;

  typedef std::shared_ptr<CDVDOverlayImage> SOverlay;
  typedef std::list<SOverlay> SOverlays;

  struct SPlane
  {
    SOverlays o;
    int w = 0;
    int h = 0;
  };

  mutable CCriticalSection m_overlayLock;
  std::chrono::steady_clock::time_point m_bdjPopupDeadline{};
  SPlane m_planes[2];
  SOverlay m_bdjOverlay;
  uint64_t m_bdjVisiblePixels{0};
  bool m_bdjSurfaceDirty{false};
  std::shared_ptr<CDVDOverlay> m_pendingOverlayGroup;
  std::thread::id m_playerThread{};
  std::atomic<std::thread::id> m_readingThread{};
  void LoadMenuSounds();
  void FreeMenuSounds();
  void PlayMenuSound(uint32_t id);
  std::vector<IAE::SoundPtr> m_menuSounds;
  enum EHoldState {
    HOLD_NONE = 0,
    HOLD_HELD,
    HOLD_DATA,
    HOLD_STILL,
    HOLD_ERROR,
    HOLD_EXIT
  };
  std::atomic<EHoldState> m_hold{HOLD_NONE};
  BD_EVENT m_event;
  uint32_t m_lastReadEvent = BD_EVENT_NONE;
#ifdef HAVE_LIBBLURAY_BDJ
  struct bd_argb_buffer_s m_argb;
#endif

  private:
    void SetupPlayerSettings() const;
    void ApplyUHDCapabilities() const;
    void ApplyAudioCapability() const;
    void ReplaceTitleInfo(BLURAY_TITLE_INFO* incoming);
    bool IsClipCodecCompatible(const BLURAY_CLIP_INFO* a, const BLURAY_CLIP_INFO* b) const;
    std::unique_ptr<BlurayFileAccess> m_fileAccess;

    /*! Bluray state serializer handler */
    CBlurayStateSerializer m_blurayStateSerializer;
};
