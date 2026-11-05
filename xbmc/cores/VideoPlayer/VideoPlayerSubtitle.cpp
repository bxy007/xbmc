/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerSubtitle.h"

#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDCodecs/Overlay/DVDOverlayCodec.h"
#include "DVDCodecs/Overlay/DVDOverlaySpu.h"
#include "DVDDemuxers/DemuxStreamSSIF.h"
#include "DVDSubtitles/DVDSubtitleParser.h"
#include "VideoRenderers/RenderManager.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <chrono>
#include <mutex>

using namespace std::chrono_literals;

CVideoPlayerSubtitle::CVideoPlayerSubtitle(CDVDOverlayContainer* pOverlayContainer, CProcessInfo &processInfo,
                                         CRenderManager& renderManager)
: CThread("VideoPlayerSubtitle"), IDVDStreamPlayer(processInfo),
  m_renderManager(renderManager), m_messageQueue("subtitle")
{
  m_pOverlayContainer = pOverlayContainer;
  m_lastPts = DVD_NOPTS_VALUE;
  m_messageQueue.SetMaxDataSize(4 * 1024 * 1024);
}

CVideoPlayerSubtitle::~CVideoPlayerSubtitle()
{
  CloseStream(true);
}

void CVideoPlayerSubtitle::Flush()
{
  if (m_asyncParse.load(std::memory_order_relaxed) && m_messageQueue.IsInited())
  {
    m_messageQueue.Flush();
    m_messageQueue.Put(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_FLUSH), 1);
    m_wakeEvent.Set();
  }
  else
    HandleMessage(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_FLUSH));
}

namespace
{
std::shared_ptr<CDVDOverlayGroup> InitialiseNewOverlayGroup(std::shared_ptr<CDVDOverlay>& overlay)
{
  auto group{std::make_shared<CDVDOverlayGroup>()};
  group->iPTSStartTime = overlay->iPTSStartTime;
  group->iPTSStopTime = overlay->iPTSStopTime;
  group->bForced = overlay->bForced;
  group->replace = overlay->replace;
  group->SetOverlayContainerFlushable(overlay->IsOverlayContainerFlushable());
  group->m_overlays.emplace_back(overlay);
  return group;
}
} // namespace

void CVideoPlayerSubtitle::HandleMessage(const std::shared_ptr<CDVDMsg>& pMsg,
                                       CDVDOverlayContainer* replayContainer)
{
  std::lock_guard lock(m_section);

  auto* container = replayContainer ? replayContainer : m_pOverlayContainer;
  if (pMsg->IsType(CDVDMsg::SUBTITLE_REPLAY))
  {
    const auto replay = std::static_pointer_cast<CDVDMsgSubtitleReplay>(pMsg);
    CDVDOverlayContainer pending;
    for (const auto& packet : replay->packets)
    {
      HandleMessage(packet, &pending);
      pending.CleanUp(replay->pts);
    }
    // No stale caption flashes while replay catches up. Audio/video keep
    // running; only the completed overlay batch takes the renderer lock.
    std::lock_guard overlayLock(*m_pOverlayContainer);
    for (const auto& overlay : *pending.GetOverlays())
      m_pOverlayContainer->ProcessAndAddOverlayIfValid(overlay);
    m_renderManager.ReplaceSubtitleOverlays(*pending.GetOverlays(), replay->delay, replay->visible);
    CLog::Log(LOGDEBUG, "Subtitle switch replay: packets={} overlays={} pts={:.3f}",
              replay->packets.size(), pending.GetSize(), replay->pts / DVD_TIME_BASE);
  }
  else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
  {
    auto pMsgDemuxerPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg);
    DemuxPacket* pPacket = pMsgDemuxerPacket->GetPacket();
    CDemuxStreamSSIF* pSSIF = m_pSSIF.load(std::memory_order_acquire);

    if (m_pOverlayCodec)
    {
      OverlayMessage result = m_pOverlayCodec->Decode(pPacket);

      if (result == OverlayMessage::OC_OVERLAY)
      {
        if (std::shared_ptr<CDVDOverlay> overlay{m_pOverlayCodec->GetOverlay()}; overlay != nullptr)
        {
          if (m_streaminfo.codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE)
          {
            int depth = 0;
            if (pSSIF && m_streaminfo.m_3dSubtitlePlane != 0xFF)
              depth = pSSIF->GetSubtitleOffsetAtPts(pPacket->pts, m_streaminfo.m_3dSubtitlePlane);
            else
              depth = m_streaminfo.m_3dSubtitlePlane;
            overlay->m_3dSubtitleDepth = depth;
          }
          auto group{InitialiseNewOverlayGroup(overlay)};
          while ((overlay = m_pOverlayCodec->GetOverlay()) != nullptr)
          {
            if (m_streaminfo.codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE)
            {
              int depth = 0;
              if (pSSIF && m_streaminfo.m_3dSubtitlePlane != 0xFF)
                depth = pSSIF->GetSubtitleOffsetAtPts(pPacket->pts, m_streaminfo.m_3dSubtitlePlane);
              else
                depth = m_streaminfo.m_3dSubtitlePlane;
              overlay->m_3dSubtitleDepth = depth;
            }
            if (*group->m_overlays.back() == *overlay)
              group->m_overlays.emplace_back(overlay);
            else
            {
              container->ProcessAndAddOverlayIfValid(group);
              group = InitialiseNewOverlayGroup(overlay);
            }
          }
          container->ProcessAndAddOverlayIfValid(group);
        }
      }
    }
    else if (m_streaminfo.codec == AV_CODEC_ID_DVD_SUBTITLE)
    {
      std::shared_ptr<CDVDOverlaySpu> pSPUInfo =
          m_dvdspus.AddData(pPacket->pData, pPacket->iSize, pPacket->pts);
      if (pSPUInfo)
      {
        CLog::Log(LOGDEBUG, "CVideoPlayer::ProcessSubData: Got complete SPU packet");
        container->ProcessAndAddOverlayIfValid(pSPUInfo);
      }
    }

  }
  else if( pMsg->IsType(CDVDMsg::SUBTITLE_CLUTCHANGE) )
  {
    auto pData = std::static_pointer_cast<CDVDMsgSubtitleClutChange>(pMsg);
    for (int i = 0; i < 16; i++)
    {
      uint8_t* color = m_dvdspus.m_clut[i];
      auto t = (uint8_t*)pData->m_data[i];

// pData->m_data[i] points to an uint32_t
// Byte swapping is needed between big and little endian systems
#ifdef WORDS_BIGENDIAN
      color[0] = t[1]; // Y
      color[1] = t[2]; // Cr
      color[2] = t[3]; // Cb
#else
      color[0] = t[2]; // Y
      color[1] = t[0]; // Cr
      color[2] = t[1]; // Cb
#endif
    }
    m_dvdspus.m_bHasClut = true;
  }
  else if( pMsg->IsType(CDVDMsg::GENERAL_FLUSH)
        || pMsg->IsType(CDVDMsg::GENERAL_RESET) )
  {
    m_dvdspus.Reset();
    if (m_pSubtitleFileParser)
      m_pSubtitleFileParser->Reset();

    if (m_pOverlayCodec)
    {
      m_pOverlayCodec->Flush();

      if (m_streaminfo.codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE)
      {
        logComponentM(LOGDEBUG, LOGVIDEO,
                      "overlay subtitle flush: recreating PGS codec to clear stale "
                      "composition/palette/object cache");
        m_pOverlayCodec.reset();
        m_pOverlayCodec = CDVDFactoryCodec::CreateOverlayCodec(m_streaminfo);
        if (!m_pOverlayCodec)
          CLog::Log(LOGERROR,
                    "CVideoPlayerSubtitle: failed to recreate PGS overlay codec on flush");
        m_hasOverlayCodec.store(m_pOverlayCodec != nullptr, std::memory_order_relaxed);
      }
    }

    /* We must flush active overlays on flush or if we have a file
     * parser since it will re-populate active items.  */
    if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH) || m_pSubtitleFileParser)
      m_pOverlayContainer->Flush();

    m_lastPts = DVD_NOPTS_VALUE;
  }
}

void CVideoPlayerSubtitle::SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority)
{
  if (m_asyncParse.load(std::memory_order_relaxed) && m_messageQueue.IsInited())
  {
    m_messageQueue.Put(pMsg, priority);
    m_wakeEvent.Set();
  }
  else
    HandleMessage(pMsg);
}

void CVideoPlayerSubtitle::Process()
{
  while (!m_bStop)
  {
    m_wakeEvent.Reset();

    std::shared_ptr<CDVDMsg> pMsg;
    MsgQueueReturnCode ret;
    bool worked = false;
    while ((ret = m_messageQueue.Get(pMsg, 0ms)) == MSGQ_OK)
    {
      HandleMessage(pMsg);
      worked = true;
    }
    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        logM(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);
      return;
    }

    if (m_hasPendingTiming.exchange(false, std::memory_order_acquire))
    {
      const double pts = m_latestPts.load(std::memory_order_relaxed);
      const double offset = m_latestOffset.load(std::memory_order_relaxed);
      std::lock_guard lock(m_section);
      ProcessParser(pts, offset);
      worked = true;
    }

    if (!worked)
      m_wakeEvent.Wait(100ms);
  }
}

void CVideoPlayerSubtitle::UpdatePlaybackPosition(double pts, double offset)
{
  if (!m_asyncParse.load(std::memory_order_relaxed))
  {
    std::lock_guard lock(m_section);
    ProcessParser(pts, offset);
    return;
  }

  if (!m_usesTimedParser.load(std::memory_order_relaxed) || pts == DVD_NOPTS_VALUE)
    return;

  m_latestPts.store(pts, std::memory_order_relaxed);
  m_latestOffset.store(offset, std::memory_order_relaxed);
  m_hasPendingTiming.store(true, std::memory_order_release);
  m_wakeEvent.Set();
}

bool CVideoPlayerSubtitle::OpenStream(CDVDStreamInfo &hints, std::string &filename)
{
  CloseStream(false);

  bool startThread = false;
  {
    std::lock_guard lock(m_section);

    m_streaminfo = hints;

    if (!filename.empty() && filename != "dvd")
    {
      m_pSubtitleFileParser.reset(CDVDFactorySubtitle::CreateParser(filename));
      if (!m_pSubtitleFileParser)
      {
        CLog::Log(LOGERROR, "{} - Unable to create subtitle parser", __FUNCTION__);
        CloseStream(true);
        return false;
      }

      CLog::Log(LOGDEBUG, "Created subtitles parser: {}", m_pSubtitleFileParser->GetName());

      if (!m_pSubtitleFileParser->Open(hints))
      {
        CLog::Log(LOGERROR, "{} - Unable to init subtitle parser", __FUNCTION__);
        CloseStream(true);
        return false;
      }
      m_pSubtitleFileParser->Reset();
      m_usesTimedParser.store(true, std::memory_order_relaxed);
      startThread = true;
    }
    else if (hints.codec == AV_CODEC_ID_DVD_SUBTITLE && filename == "dvd")
    {
    }
    else
    {
      m_pOverlayCodec = CDVDFactoryCodec::CreateOverlayCodec(hints);
      if (!m_pOverlayCodec)
      {
        CLog::Log(LOGERROR, "{} - Unable to init overlay codec", __FUNCTION__);
        return false;
      }
      CLog::Log(LOGDEBUG, "Created subtitles overlay codec: {}", m_pOverlayCodec->GetName());
      m_hasOverlayCodec.store(true, std::memory_order_relaxed);
      startThread = true;
    }
  }

  m_asyncParse.store(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoSubtitleAsyncParse,
                     std::memory_order_relaxed);
  if (startThread && m_asyncParse.load(std::memory_order_relaxed))
  {
    m_messageQueue.Init();
    Create();
  }
  return true;
}

void CVideoPlayerSubtitle::CloseStream(bool bWaitForBuffers)
{
  if (IsRunning())
  {
    m_messageQueue.Abort();
    m_wakeEvent.Set();
    StopThread();
    m_messageQueue.End();
  }
  m_usesTimedParser.store(false, std::memory_order_relaxed);
  m_hasOverlayCodec.store(false, std::memory_order_relaxed);
  m_hasPendingTiming.store(false, std::memory_order_relaxed);

  std::lock_guard lock(m_section);

  m_pSubtitleFileParser.reset();
  m_pOverlayCodec.reset();

  m_dvdspus.FlushCurrentPacket();

  if (!bWaitForBuffers)
    m_pOverlayContainer->Clear();
}

void CVideoPlayerSubtitle::ProcessParser(double pts, double offset)
{
  if (m_pSubtitleFileParser)
  {
    if(pts == DVD_NOPTS_VALUE)
      return;

    if (pts + DVD_SEC_TO_TIME(1) < m_lastPts)
    {
      m_pOverlayContainer->Clear();
      m_pSubtitleFileParser->Reset();
    }

    if(m_pOverlayContainer->GetSize() >= 5)
      return;

    std::shared_ptr<CDVDOverlay> pOverlay = m_pSubtitleFileParser->Parse(pts);
    // add all overlays which fit the pts
    while(pOverlay)
    {
      pOverlay->iPTSStartTime -= offset;
      if(pOverlay->iPTSStopTime != 0.0)
        pOverlay->iPTSStopTime -= offset;

      m_pOverlayContainer->ProcessAndAddOverlayIfValid(pOverlay);
      pOverlay = m_pSubtitleFileParser->Parse(pts);
    }

    m_lastPts = pts;
  }
}

bool CVideoPlayerSubtitle::AcceptsData() const
{
  if (m_asyncParse.load(std::memory_order_relaxed))
  {
    if (m_messageQueue.IsInited() && m_messageQueue.IsFull())
      return false;
    if (m_hasOverlayCodec.load(std::memory_order_relaxed))
      return m_pOverlayContainer->GetSize() < 200;
    return m_pOverlayContainer->GetSize() < 5;
  }

  if (m_pOverlayCodec)
    return m_pOverlayContainer->GetSize() < 200;

  // FIXME : This may still be causing problems + magic number :(
  return m_pOverlayContainer->GetSize() < 5;
}

