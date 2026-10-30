/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DirtyRegionTracker.h"
#include "GUIWindow.h"
#include "IMsgTargetCallback.h"
#include "IWindowManagerCallback.h"
#include "guilib/WindowIDs.h"
#include "messaging/IMessageTarget.h"

#include <array>
#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class CGUIDialog;
class CGUIRenderTargetFBO;
class CRenderSystemBase;
class CGUIMediaWindow;
class CFullscreenOverlayRenderThread;

#ifdef TARGET_WINDOWS_STORE
#pragma pack(push, 8)
#endif
enum class DialogModalityType;
#ifdef TARGET_WINDOWS_STORE
#pragma pack(pop)
#endif

namespace KODI
{
  namespace MESSAGING
  {
    class CApplicationMessenger;
  }
}

#define WINDOW_ID_MASK 0xffff

/*!
 \ingroup winman
 \brief
 */
class CGUIWindowManager : public KODI::MESSAGING::IMessageTarget
{
  friend CGUIDialog;
  friend CGUIMediaWindow;
public:
  CGUIWindowManager();
  ~CGUIWindowManager() override;
  bool SendMessage(CGUIMessage& message);
  bool SendMessage(int message, int senderID, int destID, int param1 = 0, int param2 = 0);
  bool SendMessage(CGUIMessage& message, int window);
  void Initialize();
  bool Add(CGUIWindow* pWindow);
  void AddUniqueInstance(CGUIWindow *window);
  void AddCustomWindow(CGUIWindow* pWindow);
  void Remove(int id);
  void Delete(int id);
  void ActivateWindow(int iWindowID, const std::string &strPath = "");
  void ForceActivateWindow(int iWindowID, const std::string &strPath = "");
  void ChangeActiveWindow(int iNewID, const std::string &strPath = "");
  void ActivateWindow(int iWindowID, const std::vector<std::string>& params, bool swappingWindows = false, bool force = false);
  void PreviousWindow();
  bool HasVisibleDialog() const { return !m_activeDialogs.empty(); }
  bool HasVisibleDialogContentInRegions(const std::vector<CRect>& regions) const;

  /**
   * \brief Switch window to fullscreen
   *
   * \param force enforce fullscreen switch
   * \return True if switch is made to fullscreen, otherwise False
   */
  bool SwitchToFullScreen(bool force = false);

  void CloseDialogs(bool forceClose = false) const;
  void CloseInternalModalDialogs(bool forceClose = false) const;

  void OnApplicationMessage(KODI::MESSAGING::ThreadMessage* pMsg) override;
  int GetMessageMask() override;

  // OnAction() runs through our active dialogs and windows and sends the message
  // off to the callbacks (application, python, playlist player) and to the
  // currently focused window(s).  Returns true only if the message is handled.
  bool OnAction(const CAction &action) const;

  /*! \brief Process active controls allowing them to animate before rendering.
   */
  void Process(unsigned int currentTime);

  /*! \brief Mark the screen as dirty, forcing a redraw at the next Render()
   */
  void MarkDirty();

  /*! \brief Mark a region as dirty, forcing a redraw at the next Render()
   */
  void MarkDirty(const CRect& rect);

  void MarkDirtyRegionOnly(const CRect& rect);

  /*! \brief Rendering of the current window and any dialogs
   Render is called every frame to draw the current window and any dialogs.
   It should only be called from the application thread.
   Returns true only if it has rendered something.
   */
  bool Render(bool forScreenshot = false);

  void RenderEx() const;

  /*! \brief Do any post render activities.
   */
  void AfterRender();

  void QuiesceFullscreenOverlayWorker();
  void ScheduleAsyncFullscreenOverlayRender();
  bool BeginRenderExclusion();
  void EndRenderExclusion();

  /*! \brief Per-frame updating of the current window and any dialogs
   FrameMove is called every frame to update the current window and any dialogs
   on screen. It should only be called from the application thread.
   */
  void FrameMove();

  /*! \brief Return whether the window manager is initialized.
   The window manager is initialized on skin load - if the skin isn't yet loaded,
   no windows should be able to be initialized.
   \return true if the window manager is initialized, false otherwise.
   */
  bool Initialized() const { return m_initialized; }

  /*! \brief Create and initialize all windows and dialogs
   */
  void CreateWindows();

  /*! \brief Destroy and remove all windows and dialogs
  *
  * \return true on success, false if destruction fails for any window
  */
  bool DestroyWindows();

  /*! \brief Destroy and remove the window or dialog with the given id
   *
   *\param id the window id
   */
  void DestroyWindow(int id);

  /*! \brief Return the window of type \code{T} with the given id or
   * null if no window exists with the given id.
   *
   * \tparam T the window class type
   * \param id the window id
   * \return the window with for the given type \code{T} or null
   */
  template<typename T,
           typename std::enable_if<std::is_base_of<CGUIWindow, T>::value>::type* = nullptr>
  T* GetWindow(int id) const
  {
    return dynamic_cast<T*>(GetWindow(id));
  }

  /*! \brief Return the window with the given id or null.
   *
   * \param id the window id
   * \return the window with the given id or null
   */
  CGUIWindow* GetWindow(int id) const;

  /*! \brief Return the dialog window with the given id or null.
   *
   * \param id the dialog window id
   * \return the dialog window with the given id or null
   */
  CGUIDialog* GetDialog(int id) const;

  void SetCallback(IWindowManagerCallback& callback);
  void DeInitialize();

  /*! \brief Register a dialog as active dialog
   *
   * \param dialog The dialog to register as active dialog
   */
  void RegisterDialog(CGUIWindow* dialog);
  void RemoveDialog(int id);

  /*! \brief Get the ID of the topmost dialog
   *
   * \param ignoreClosing ignore dialog is closing
   * \return the ID of the topmost dialog or WINDOW_INVALID if no dialog is active
   */
  int GetTopmostDialog(bool ignoreClosing = false) const;

  /*! \brief Get the ID of the topmost modal dialog
   *
   * \param ignoreClosing ignore dialog is closing
   * \return the ID of the topmost modal dialog or WINDOW_INVALID if no modal dialog is active
   */
  int GetTopmostModalDialog(bool ignoreClosing = false) const;

  void SendThreadMessage(CGUIMessage& message, int window = 0);
  void DispatchThreadMessages();
  // method to removed queued messages with message id in the requested message id list.
  // pMessageIDList: point to first integer of a 0 ends integer array.
  int RemoveThreadMessageByMessageIds(int *pMessageIDList);
  void AddMsgTarget( IMsgTargetCallback* pMsgTarget );
  void RemoveMsgTarget(IMsgTargetCallback* pMsgTarget);
  int GetActiveWindow() const;
  int GetActiveWindowOrDialog() const;
  bool HasModalDialog(bool ignoreClosing) const;
  bool HasVisibleModalDialog() const;
  bool IsDialogTopmost(int id, bool modal = false) const;
  bool IsDialogTopmost(const std::string &xmlFile, bool modal = false) const;
  bool IsModalDialogTopmost(int id) const;
  bool IsModalDialogTopmost(const std::string &xmlFile) const;
  bool IsWindowActive(int id, bool ignoreClosing = true) const;
  bool IsWindowVisible(int id) const;
  bool IsWindowActive(const std::string &xmlFile, bool ignoreClosing = true) const;
  bool IsWindowVisible(const std::string &xmlFile) const;
  /*! \brief Checks if the given window is an addon window.
   *
   * \return true if the given window is an addon window, otherwise false.
   */
  bool IsAddonWindow(int id) const { return (id >= WINDOW_ADDON_START && id <= WINDOW_ADDON_END); }
  /*! \brief Checks if the given window is a python window.
   *
   * \return true if the given window is a python window, otherwise false.
   */
  bool IsPythonWindow(int id) const
  {
    return (id >= WINDOW_PYTHON_START && id <= WINDOW_PYTHON_END);
  }

  bool HasVisibleControls() const;

#ifdef _DEBUG
  void DumpTextureUse();
#endif
private:
  void RenderPass() const;
  /*! \brief Render in one back to front pass.
   */
  void RenderPassSingle() const;
  /*! \brief Render opaque elements front to back, and transparent ones back to front
   */
  void RenderPassDual() const;
  void UpdateFullscreenOverlayRenderTarget() const;

  enum class FullscreenOverlayCompositeSource
  {
    NONE,
    SYNC_TARGET,
    WORKER_FBO,
  };

  enum class FullscreenOverlayPromoteResult
  {
    NONE,
    PROMOTED,
    KEEP_DISPLAYED,
  };

  struct FullscreenOverlayWorkerCounters
  {
    int prep{0};
    double prepMsTotal{0.0};
    int promo{0};
    int stale{0};
    int fenceNR{0};
    int fenceWait{0};
    int wdRetry{0};
    int wdDemote{0};
    int busySkip{0};
    int stall{0};
    int relReq{0};
    int inlineFill{0};
    int excl{0};
    int compNone{0};
    int syncComposites{0};
    int syncFills{0};
    int syncCreates{0};
    int syncReleases{0};
    int syncDeclines{0};
    const char* syncLastReason{nullptr};
    int fenceNRMax{0};
    int pendSkip{0};
    int gpuSamples{0};
    int gpuZeroSamples{0};
    double gpuMsTotal{0.0};
    double gpuMsMax{0.0};
    int contentSamples{0};
    double contentPctTotal{0.0};
    double contentPctMax{0.0};
    int fillSig{0};
    int fillAnim{0};
    int fillDirty{0};
    int fillInterval{0};
    int fillSuppressed{0};
    int staleFocus{0};
  };

  struct FullscreenOverlayFillStats
  {
    int composites{0};
    int fills{0};
    int creates{0};
    int releases{0};
    int declines{0};
    const char* lastReason{nullptr};
    int fboW{-1};
    int fboH{-1};
    int fillSig{0};
    int fillAnim{0};
    int fillDirty{0};
    int fillInterval{0};
    int fillSuppressed{0};
    int staleFocus{0};
  };

  struct AsyncFullscreenOverlaySignature
  {
    int activeWindowID{0};
    unsigned int width{0};
    unsigned int height{0};
    unsigned int shaderEpoch{0};
    std::vector<int> dialogIds;
    std::vector<int> focusIds;

    bool operator==(const AsyncFullscreenOverlaySignature& other) const
    {
      return activeWindowID == other.activeWindowID && width == other.width &&
             height == other.height && shaderEpoch == other.shaderEpoch &&
             dialogIds == other.dialogIds && focusIds == other.focusIds;
    }
  };

  bool FillFullscreenOverlayTarget(FullscreenOverlayFillStats& stats,
                                   std::chrono::steady_clock::time_point now) const;
  void SelectFullscreenOverlayCompositeSource() const;
  void AccumulateSyncFillStats(const FullscreenOverlayFillStats& stats) const;
  static bool HasFullscreenOverlayWorkerActivity(const FullscreenOverlayWorkerCounters& counters);
  bool WaitPreparedFullscreenOverlayFence(CRenderSystemBase* renderSystem) const;
  FullscreenOverlayPromoteResult PromotePreparedFullscreenOverlayRenderTarget(
      const AsyncFullscreenOverlaySignature& current) const;
  AsyncFullscreenOverlaySignature BuildAsyncFullscreenOverlaySignature() const;
  const CGUIRenderTargetFBO* GetFullscreenOverlayCompositeTarget() const;
  void CompositeFullscreenOverlay(CRenderSystemBase* renderSystem,
                                  const CGUIRenderTargetFBO& target) const;
  void StopFullscreenOverlayRenderThread();

  void LoadNotOnDemandWindows() const;
  void UnloadNotOnDemandWindows() const;
  void AddToWindowHistory(int newWindowID);

  /*!
   \brief Check if the given window id is in the window history, and if so, remove this
    window and all overlying windows from the history so that we always have a predictable
    "Back" behaviour for each window.

   \param windowID the window id to remove from the window history
   */
  void RemoveFromWindowHistory(int windowID);
  void ClearWindowHistory();
  void CloseWindowSync(CGUIWindow *window, int nextWindowID = 0);
  int GetTopmostDialog(bool modal, bool ignoreClosing) const;

  friend class KODI::MESSAGING::CApplicationMessenger;

  /*! \brief Activate the given window.
   *
   * \param windowID The window ID to activate.
   * \param params Parameter
   * \param swappingWindows True if the window should be swapped with the previous window instead of put it in the window history, otherwise false
   * \param force True to ignore checks which refuses opening the window, otherwise false
   */
  void ActivateWindow_Internal(int windowID, const std::vector<std::string> &params, bool swappingWindows, bool force = false);

  bool ProcessRenderLoop(bool renderOnly);

  bool HandleAction(const CAction &action) const;

  std::unordered_map<int, std::shared_ptr<CGUIWindow>> m_mapWindows;
  std::vector<std::shared_ptr<CGUIWindow>> m_vecCustomWindows;
  std::vector<std::shared_ptr<CGUIWindow>> m_dialogWindows;
  std::vector<std::shared_ptr<CGUIWindow>> m_activeDialogs;
  std::vector<std::shared_ptr<CGUIWindow>> m_deleteWindows;
  bool m_renderedForScreenshot{false};

  mutable std::unique_ptr<CGUIRenderTargetFBO> m_fullscreenOverlayRenderTarget;
  mutable std::chrono::steady_clock::time_point m_fullscreenOverlayLastFillTime;
  mutable std::chrono::steady_clock::time_point m_fullscreenOverlayLastActiveTime;
  mutable size_t m_fullscreenOverlaySignature{0};
  mutable bool m_fullscreenOverlayDirtyEpisodeFilled{false};
  mutable bool m_fullscreenOverlayComposite{false};
  mutable std::vector<int> m_fullscreenOverlayCompositeIds;
  mutable std::vector<std::pair<int, int>> m_fullscreenOverlayFilledFocus;
  mutable const char* m_osdTraceReason{"none"};
  mutable const char* m_osdTraceFill{"none"};
  mutable int m_osdTraceBlitIndex{-1};
  mutable int m_osdTraceLiveAfterBlit{0};
  mutable int m_osdTracePrevSource{-1};
  mutable unsigned int m_osdTraceFrame{0};
  mutable std::atomic<bool> m_osdTraceArmed{false};
  mutable int m_osdTraceBlitCount{0};
  mutable int m_osdTracePass{0};
  mutable int m_osdTraceSkinRet{-1};

  mutable std::unique_ptr<CGUIRenderTargetFBO> m_skinHdrRenderTarget;
  mutable bool m_skinHdrTargetValid{false};
  bool m_skinHdrEngagedLogged{false};
  mutable bool m_skinHdrEngagedLastFrame{false};

  static constexpr size_t FULLSCREEN_OVERLAY_RENDER_TARGET_COUNT{2};
  mutable CCriticalSection m_fullscreenOverlayStateSection;
  mutable std::array<std::unique_ptr<CGUIRenderTargetFBO>, FULLSCREEN_OVERLAY_RENDER_TARGET_COUNT>
      m_fullscreenOverlayRenderTargets;
  mutable int m_preparedFullscreenOverlayRenderTargetIndex{-1};
  mutable int m_displayedFullscreenOverlayRenderTargetIndex{-1};
  mutable AsyncFullscreenOverlaySignature m_preparedFullscreenOverlaySignature;
  mutable AsyncFullscreenOverlaySignature m_displayedFullscreenOverlaySignature;
  mutable AsyncFullscreenOverlaySignature m_pendingFullscreenOverlaySignature;
  mutable void* m_preparedFullscreenOverlayFence{nullptr};
  mutable std::atomic<bool> m_fullscreenOverlayRenderThreadDisabled{false};
  mutable std::atomic<uint32_t> m_guiDepthLayer{2};
  mutable bool m_fullscreenOverlayReleaseRequested{false};
  mutable FullscreenOverlayCompositeSource m_fullscreenOverlayCompositeSource{
      FullscreenOverlayCompositeSource::NONE};
  std::unique_ptr<CFullscreenOverlayRenderThread> m_fullscreenOverlayRenderThread;
  mutable FullscreenOverlayWorkerCounters m_fullscreenOverlayWorkerCounters;
  mutable bool m_fullscreenOverlayContentSampled{false};
  mutable std::chrono::steady_clock::time_point m_preparedFullscreenOverlayPublishTime;
  mutable std::chrono::steady_clock::time_point m_fullscreenOverlayWorkerLastActivity;
  mutable std::chrono::steady_clock::time_point m_fullscreenOverlayLastQueueTime;
  mutable int m_fullscreenOverlayFenceNRStreak{0};
  mutable std::atomic<bool> m_fullscreenOverlayDiagEnabled{false};
  mutable bool m_fullscreenOverlayDiagConfigLogged{false};
  mutable int m_fullscreenOverlayFenceNRIndex{-1};
  mutable std::chrono::steady_clock::time_point m_fullscreenOverlayFenceNRPublishTime;
  mutable bool m_fullscreenOverlayRetryRequested{false};
  mutable bool m_fullscreenOverlayRetryAttempted{false};
  CCriticalSection m_guiRenderExclusion;
  bool m_renderExclusionHeld{false};

  friend class ::CFullscreenOverlayRenderThread;

  std::deque<int> m_windowHistory;

  IWindowManagerCallback* m_pCallback;
  std::list< std::pair<CGUIMessage*,int> > m_vecThreadMessages;
  CCriticalSection m_critSection;
  std::vector<IMsgTargetCallback*> m_vecMsgTargets;

  int  m_iNested;
  bool m_initialized;
  mutable bool m_touchGestureActive{false};
  mutable bool m_inhibitTouchGestureEvents{false};

  CDirtyRegionList m_dirtyregions;
  CDirtyRegionTracker m_tracker;
  int m_bufAgeLastStereoMode{0};
  std::vector<CRect> m_lastPromotedRegions;
};
