/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDDemuxSPU.h"
#include "DVDMessageQueue.h"
#include "DVDOverlayContainer.h"
#include "DVDStreamInfo.h"
#include "DVDSubtitles/DVDFactorySubtitle.h"
#include "IVideoPlayer.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "threads/Thread.h"

#include <atomic>

class CDVDInputStream;
class CDVDSubtitleStream;
class CDVDSubtitleParser;
class CDVDInputStreamNavigator;
class CDVDOverlayCodec;
class CDemuxStreamSSIF;
class CRenderManager;

class CVideoPlayerSubtitle : public CThread, public IDVDStreamPlayer
{
public:
  CVideoPlayerSubtitle(CDVDOverlayContainer* pOverlayContainer, CProcessInfo &processInfo,
                       CRenderManager& renderManager);
  ~CVideoPlayerSubtitle() override;

  void UpdatePlaybackPosition(double pts, double offset);
  void Flush();
  void FindSubtitles(const char* strFilename);
  int GetSubtitleCount();

  void UpdateOverlayInfo(const std::shared_ptr<CDVDInputStreamNavigator>& pStream, int iAction)
  {
    m_pOverlayContainer->UpdateOverlayInfo(pStream, &m_dvdspus, iAction);
  }

  bool AcceptsData() const override;
  void SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority = 0) override;
  void FlushMessages() override { m_messageQueue.Flush(); }
  bool OpenStream(CDVDStreamInfo hints) override { return OpenStream(hints, hints.filename); }
  bool OpenStream(CDVDStreamInfo &hints, std::string& filename);
  void CloseStream(bool bWaitForBuffers) override;

  bool IsInited() const override { return true; }
  bool IsStalled() const override { return m_pOverlayContainer->GetSize() == 0; }
  void SetSSIF(CDemuxStreamSSIF* pSSIF) { m_pSSIF.store(pSSIF, std::memory_order_release); }

protected:
  void Process() override;
  void ProcessParser(double pts, double offset);
  void HandleMessage(const std::shared_ptr<CDVDMsg>& pMsg,
                     CDVDOverlayContainer* replayContainer = nullptr);

private:
  std::atomic<CDemuxStreamSSIF*> m_pSSIF{nullptr};
  CDVDOverlayContainer* m_pOverlayContainer;
  CRenderManager& m_renderManager;

  std::unique_ptr<CDVDSubtitleParser> m_pSubtitleFileParser;
  std::unique_ptr<CDVDOverlayCodec> m_pOverlayCodec;
  CDVDDemuxSPU        m_dvdspus;

  CDVDStreamInfo      m_streaminfo;
  double              m_lastPts;


  CCriticalSection    m_section;

  CDVDMessageQueue m_messageQueue;
  CEvent m_wakeEvent{true};
  std::atomic_bool m_asyncParse{false};
  std::atomic_bool m_usesTimedParser{false};
  std::atomic_bool m_hasPendingTiming{false};
  std::atomic_bool m_hasOverlayCodec{false};
  std::atomic<double> m_latestPts{DVD_NOPTS_VALUE};
  std::atomic<double> m_latestOffset{0.0};
};


//typedef struct SubtitleInfo
//{

//
//} SubtitleInfo;

