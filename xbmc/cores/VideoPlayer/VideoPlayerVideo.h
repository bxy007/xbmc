/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDClock.h"
#include "DVDCodecs/Video/DVDVideoCodec.h"
#include "DVDMessageQueue.h"
#include "DVDOverlayContainer.h"
#include "DVDStreamInfo.h"
#include "IVideoPlayer.h"
#include "PTSTracker.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "threads/Thread.h"
#include "threads/SystemClock.h"
#include "utils/BitstreamStats.h"

#include <atomic>
#include <chrono>

#define DROP_DROPPED 1
#define DROP_VERYLATE 2
#define DROP_BUFFER_LEVEL 4

class CDemuxStreamVideo;

class CDroppingStats
{
public:
  void Reset();
  void AddOutputDropGain(double pts, int frames);
  struct CGain
  {
    int frames;
    double pts;
  };
  std::deque<CGain> m_gain;
  int m_totalGain;
  double m_lastPts;
};

class CVideoPlayerVideo : public CThread, public IDVDStreamPlayerVideo
{
public:
  CVideoPlayerVideo(
    CDVDClock* pClock,
    CDVDOverlayContainer* pOverlayContainer,
    CDVDMessageQueue& parent,
    CRenderManager& renderManager,
    CProcessInfo &processInfo,
    double messageQueueTimeSize);
  ~CVideoPlayerVideo() override;

  bool OpenStream(CDVDStreamInfo hint) override;
  void CloseStream(bool bWaitForBuffers) override;
  void SetSpeed(int iSpeed) override;
  void SetEOS(bool eos) override { m_isEOS = eos; }
  void Flush(bool sync) override;
  bool AcceptsData() const override;
  bool HasData() const override;
  int  GetLevel() const override { return m_messageQueue.GetLevel(); }
  void SetMaxTimeSize(double sec) override { m_messageQueue.SetMaxTimeSize(sec); }
  double GetMaxTimeSizeSeconds() const override
  {
    const double inverseSeconds = m_messageQueue.GetMaxTimeSize();
    return inverseSeconds > 0.0 ? 1.0 / inverseSeconds : 0.0;
  }
  double GetQueueTimeSize() const override { return m_messageQueue.GetTimeSize(); }
  bool IsInited() const override;
  bool IsEOS() override;
  bool IsOutputPictureInFlight() const { return m_outputPictureInFlight.load(); }
  void SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority = 0) override;
  void FlushMessages() override;

  void EnableSubtitle(bool bEnable) override { m_bRenderSubs = bEnable; }
  bool IsSubtitleEnabled() override { return m_bRenderSubs; }
  double GetSubtitleDelay() override { return m_iSubtitleDelay.load(std::memory_order_relaxed); }
  void SetSubtitleDelay(double delay) override
  {
    m_iSubtitleDelay.store(delay, std::memory_order_relaxed);
  }
  bool IsStalled() const override { return m_stalled; }
  bool IsPlaybackStalled() const override { return m_playbackStalled; }
  double GetCurrentPts() override;
  double GetOutputDelay() override; /* returns the expected delay, from that a packet is put in queue */
  std::string GetPlayerInfo() override;
  int GetVideoBitrate() override;
  bool SupportsExtention() const override { return m_pVideoCodec && m_pVideoCodec->SupportsExtention(); }
  bool HonorsAccurateSeek() const override { return !m_pVideoCodec || m_pVideoCodec->HonorsAccurateSeek(); }

  // classes
  CDVDOverlayContainer* m_pOverlayContainer;
  CDVDClock* m_pClock;

protected:

  enum EOutputState
  {
    OUTPUT_NORMAL,
    OUTPUT_ABORT,
    OUTPUT_DROPPED,
    OUTPUT_AGAIN
  };

  void OnExit() override;
  void Process() override;

  bool ProcessDecoderOutput(double &frametime, double &pts);
  void UpdatePlayerInfo();
  void SendMessageBack(const std::shared_ptr<CDVDMsg>& pMsg, int priority = 0);
  MsgQueueReturnCode GetMessage(std::shared_ptr<CDVDMsg>& pMsg,
                                std::chrono::milliseconds timeout,
                                int& priority);

  EOutputState OutputPicture(const VideoPicture& src);
  void ProcessOverlays(const VideoPicture& source, double pts) const;
  void OpenStream(CDVDStreamInfo& hint, std::unique_ptr<CDVDVideoCodec> codec);

  void ResetFrameRateCalc();
  void CalcFrameRate();
  void ResolveTelecineProbe(double& frametime, bool timedOut);
  int CalcDropRequirement(double pts);

  std::atomic<double> m_iSubtitleDelay{0.0};

  int m_retryProgressive;
  int m_telecineProbe;
  bool m_telecine;
  double m_halvedFieldRate;
  int m_telecineTwoFieldPackets;
  std::chrono::steady_clock::time_point m_telecineProbeStart;
  std::string m_vfmt;
  int m_iLateFrames;
  int m_iDroppedFrames;
  int m_iDroppedRequest;

  double m_fFrameRate;       //framerate of the video currently playing
  double m_fStableFrameRate; //place to store calculated framerates
  int m_iFrameRateCount;     //how many calculated framerates we stored in m_fStableFrameRate
  bool m_bAllowDrop;         //we can't drop frames until we've calculated the framerate
  int m_iFrameRateErr;       //how many frames we couldn't calculate the framerate, we give up after a while
  int m_iFrameRateLength;    //how many seconds we should measure the framerate
                             //this is increased exponentially from CVideoPlayerVideo::CalcFrameRate()

  bool m_bFpsInvalid;        // needed to ignore fps (e.g. dvd stills)
  std::atomic<bool> m_bRenderSubs;
  float m_fForcedAspectRatio;
  int m_speed;
  std::atomic_bool m_stalled = false;
  std::atomic_bool m_playbackStalled;
  std::atomic_bool m_isEOS{true};
  std::atomic_bool m_outputPictureInFlight{false};
  bool m_swBlockArmed = false;
  bool m_swBlockResetPending = false;
  int m_swBlockAgain = 0;
  int m_swBlockDropped = 0;
  int m_swVqMin = -1;
  int m_swVqMax = -1;
  int64_t m_swQWaitUs = 0;
  int64_t m_swWaitUs = 0;
  int64_t m_swAddUs = 0;
  std::chrono::steady_clock::time_point m_swBlockStamp{};
  bool m_paused;
  IDVDStreamPlayer::ESyncState m_syncState;
  std::atomic_bool m_bAbortOutput;

  BitstreamStats m_videoStats;

  CDVDMessageQueue m_messageQueue;
  CDVDMessageQueue& m_messageParent;
  CDVDStreamInfo m_hints;
  std::unique_ptr<CDVDVideoCodec> m_pVideoCodec;
  CPtsTracker m_ptsTracker;
  std::list<DVDMessageListItem> m_packets;
  CDroppingStats m_droppingStats;
  CRenderManager& m_renderManager;
  VideoPicture m_picture;

  EOutputState m_outputSate;
  bool m_noPicture{false};

  bool m_displayReset{false};
  std::chrono::steady_clock::time_point m_lastDisplayReset{std::chrono::steady_clock::time_point::min()};

  std::chrono::steady_clock::time_point m_rendererConfigureRetryStart{std::chrono::steady_clock::time_point::min()};

  XbmcThreads::EndTime<> m_playerInfoTimer;
};
