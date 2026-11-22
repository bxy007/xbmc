/*
*      Copyright (C) 2005-2013 Team XBMC
*      http://xbmc.org
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with XBMC; see the file COPYING.  If not, see
*  <http://www.gnu.org/licenses/>.
*
*/

#include "DemuxStreamSSIF.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDDemux.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "DVDDemuxUtils.h"
#include "utils/LogThrottle.h"
#include "utils/log.h"

//#define DEBUG_VERBOSE
#define MVC_QUEUE_SIZE 100
constexpr auto STEREO_UNRECOVERABLE_TIMEOUT = std::chrono::seconds(15);
constexpr auto STEREO_DISCARD_GAP_RESET = std::chrono::seconds(3);
constexpr double MVC_EXT_RESEEK_GAP = 10.0 * DVD_TIME_BASE;
constexpr auto MVC_EXT_RECOVERY_INTERVAL = std::chrono::seconds(2);
constexpr size_t OFMD_TABLE_MAX = 512;
constexpr int MVC_MAX_AU_SIZE = 8 * 1024 * 1024;

DemuxPacket* CDemuxStreamSSIF::AddPacket(DemuxPacket* &srcPkt)
{
  if (srcPkt->iStreamId != m_h264StreamId &&
      srcPkt->iStreamId != m_mvcStreamId)
    return srcPkt;

  if (srcPkt->iStreamId == m_h264StreamId)
  {
    if (m_bluRay && !m_bluRay->HasExtention())
      return srcPkt;
#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> MVC add h264 packet: pts: {:.3f} dts: {:.3f}", srcPkt->pts*1e-6, srcPkt->dts*1e-6);
#endif
    m_H264queue.push(srcPkt);
  }
  else if (srcPkt->iStreamId == m_mvcStreamId)
  {
    AddMVCExtPacket(srcPkt);
  }

  return GetMVCPacket();
}

void CDemuxStreamSSIF::FreeQueuedPackets()
{
  while (!m_H264queue.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_H264queue.front());
    m_H264queue.pop();
  }
  while (!m_MVCqueue.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_MVCqueue.front());
    m_MVCqueue.pop_front();
  }
}

void CDemuxStreamSSIF::Flush()
{
  FreeQueuedPackets();
  {
    std::lock_guard lock(m_subtitleOffsets->m_mutex);
    m_subtitleOffsets->m_entries.clear();
  }
  m_baseDiscardsSinceMatch = 0;
  m_wrapResyncPending = false;
  m_frontDtsValid = false;
  if (m_bluRay)
    m_bluRay->ConsumeStereoResyncRequest();
}

DemuxPacket* CDemuxStreamSSIF::MergePacket(DemuxPacket* &srcPkt, DemuxPacket* &appendPkt)
{
  DemuxPacket* newpkt = nullptr;
  newpkt = CDVDDemuxUtils::AllocateDemuxPacket(srcPkt->iSize + appendPkt->iSize);
  newpkt->iSize = srcPkt->iSize + appendPkt->iSize;

  newpkt->pts = srcPkt->pts;
  newpkt->dts = srcPkt->dts;
  newpkt->duration = srcPkt->duration;
  newpkt->iGroupId = srcPkt->iGroupId;
  newpkt->iStreamId = srcPkt->iStreamId;
  memcpy(newpkt->pData, srcPkt->pData, srcPkt->iSize);
  memcpy(newpkt->pData + srcPkt->iSize, appendPkt->pData, appendPkt->iSize);

  CDVDDemuxUtils::FreeDemuxPacket(srcPkt);
  srcPkt = nullptr;
  CDVDDemuxUtils::FreeDemuxPacket(appendPkt);
  appendPkt = nullptr;

  return newpkt;
}

DemuxPacket* CDemuxStreamSSIF::GetMVCPacket()
{
  // if input is a bluray fill mvc queue before processing
  if (m_bluRay && !m_H264queue.empty())
  {
    const double dtsBase = m_H264queue.front()->dts;
    if (m_bluRay->ConsumeStereoResyncRequest())
      m_wrapResyncPending = true;
    if (m_wrapResyncPending && dtsBase != DVD_NOPTS_VALUE && m_frontDtsValid &&
        dtsBase < m_lastFrontDts)
    {
      m_wrapResyncPending = false;
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "CDemuxStreamSSIF: loop wrap reached the base queue front, {:.3f}s -> {:.3f}s, "
                    "re-seeking the MVC extension",
                    m_lastFrontDts * 1e-6, dtsBase * 1e-6);
      if (!m_bluRay->OpenNextStream())
        ResyncExtension(dtsBase);
      m_lastExtRecovery = std::chrono::steady_clock::time_point{};
    }
    if (dtsBase != DVD_NOPTS_VALUE)
    {
      m_lastFrontDts = dtsBase;
      m_frontDtsValid = true;
    }
    if (m_MVCqueue.empty())
      FillMVCQueue(dtsBase);
  }

  const auto nowTs = std::chrono::steady_clock::now();

  // Here, we recreate a h264 MVC packet from the base one + buffered MVC NALU's
  while (!m_H264queue.empty() && !m_MVCqueue.empty())
  {
    DemuxPacket* h264pkt = m_H264queue.front();
    double tsH264 = (h264pkt->dts != DVD_NOPTS_VALUE ? h264pkt->dts : h264pkt->pts);
    DemuxPacket* mvcpkt = m_MVCqueue.front();
    double tsMVC = (mvcpkt->dts != DVD_NOPTS_VALUE ? mvcpkt->dts : mvcpkt->pts);

    if (tsH264 == tsMVC)
    {
      m_H264queue.pop();
      m_MVCqueue.pop_front();

      while (!m_H264queue.empty())
      {
        DemuxPacket* pkt = m_H264queue.front();
        double ts = (pkt->dts != DVD_NOPTS_VALUE ? pkt->dts : pkt->pts);
        if (ts == DVD_NOPTS_VALUE)
        {
#if defined(DEBUG_VERBOSE)
          CLog::Log(LOGDEBUG, ">>> MVC merge h264 fragment: {:6}+{:6}, pts({:.3f}/{:.3f}) dts({:.3f}/{:.3f})", h264pkt->iSize, pkt->iSize, h264pkt->pts*1e-6, pkt->pts*1e-6, h264pkt->dts*1e-6, pkt->dts*1e-6);
#endif
          h264pkt = MergePacket(h264pkt, pkt);
          m_H264queue.pop();
        }
        else
          break;
      }

#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC merge packet: {:6}+{:6}, pts({:.3f}/{:.3f}) dts({:.3f}/{:.3f})", h264pkt->iSize, mvcpkt->iSize, h264pkt->pts*1e-6, mvcpkt->pts*1e-6, h264pkt->dts*1e-6, mvcpkt->dts*1e-6);
#endif
      if (!m_firstMatchLogged)
      {
        m_firstMatchLogged = true;
        logComponentM(LOGDEBUG, LOGVIDEO, "CDemuxStreamSSIF: BD3D MVC stereo pair matched at dts {:.3f}s - stitching active", tsH264 * 1e-6);
      }
      m_baseDiscardsSinceMatch = 0;
      m_stitchAnchor = nowTs;
      m_lastDiscardActivity = nowTs;
      m_starveAnchor = std::chrono::steady_clock::time_point{};
      return MergePacket(h264pkt, mvcpkt);
    }
    else if (tsH264 > tsMVC)
    {
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                            "CDemuxStreamSSIF: mvc view discarded (behind base), base dts {:.3f}s "
                            "pts {:.3f}s, mvc dts {:.3f}s pts {:.3f}s",
                            h264pkt->dts * 1e-6, h264pkt->pts * 1e-6, mvcpkt->dts * 1e-6,
                            mvcpkt->pts * 1e-6);
      CDVDDemuxUtils::FreeDemuxPacket(mvcpkt);
      m_MVCqueue.pop_front();
    }
    else
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC discard h264: {:6}, pts({:.3f}) dts({:.3f})", h264pkt->iSize, h264pkt->pts*1e-6, h264pkt->dts*1e-6);
#endif
      if (m_bluRay && tsH264 != DVD_NOPTS_VALUE && tsMVC != DVD_NOPTS_VALUE &&
          tsMVC - tsH264 > MVC_EXT_RESEEK_GAP &&
          nowTs - m_lastExtRecovery >= MVC_EXT_RECOVERY_INTERVAL)
      {
        ResyncExtension(tsH264);
        continue;
      }
      ++m_baseDiscardsSinceMatch;
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                            "CDemuxStreamSSIF: base view discarded, {} since last stereo match, "
                            "base dts {:.3f}s pts {:.3f}s, mvc dts {:.3f}s pts {:.3f}s",
                            m_baseDiscardsSinceMatch, h264pkt->dts * 1e-6, h264pkt->pts * 1e-6,
                            mvcpkt->dts * 1e-6, mvcpkt->pts * 1e-6);
      if (m_stitchAnchor == std::chrono::steady_clock::time_point{} ||
          nowTs - m_lastDiscardActivity > STEREO_DISCARD_GAP_RESET)
        m_stitchAnchor = nowTs;
      m_lastDiscardActivity = nowTs;
      if (!m_stereoDeadSignaled && m_bluRay &&
          nowTs - m_stitchAnchor > STEREO_UNRECOVERABLE_TIMEOUT)
      {
        m_stereoDeadSignaled = true;
        logM(LOGWARNING,
             "CDemuxStreamSSIF: no MVC stereo pair matched for {}s ({} base views discarded); 3D "
             "extension unrecoverable, ending playback",
             std::chrono::duration_cast<std::chrono::seconds>(nowTs - m_stitchAnchor).count(),
             m_baseDiscardsSinceMatch);
        m_bluRay->OnStereoStreamUnrecoverable();
      }
      CDVDDemuxUtils::FreeDemuxPacket(h264pkt);
      m_H264queue.pop();
    }
  }

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, ">>> MVC waiting. MVC({}) H264({})", m_MVCqueue.size(), m_H264queue.size());
#endif

  if (!m_H264queue.empty())
  {
    if (m_starveAnchor == std::chrono::steady_clock::time_point{} ||
        nowTs - m_lastStarveActivity > STEREO_DISCARD_GAP_RESET)
      m_starveAnchor = nowTs;
    m_lastStarveActivity = nowTs;

    LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                          "CDemuxStreamSSIF: no stereo pair produced for {}ms, base queue {} mvc "
                          "queue {}, base dts {:.3f}s pts {:.3f}s",
                          std::chrono::duration_cast<std::chrono::milliseconds>(nowTs -
                                                                               m_starveAnchor)
                              .count(),
                          m_H264queue.size(), m_MVCqueue.size(),
                          m_H264queue.front()->dts * 1e-6, m_H264queue.front()->pts * 1e-6);

    if (!m_stereoDeadSignaled && m_bluRay &&
        nowTs - m_starveAnchor > STEREO_UNRECOVERABLE_TIMEOUT)
    {
      m_stereoDeadSignaled = true;
      logM(LOGWARNING,
           "CDemuxStreamSSIF: no MVC extension data for {}s ({} base views queued, {} mvc views "
           "queued); 3D extension unrecoverable, ending playback",
           std::chrono::duration_cast<std::chrono::seconds>(nowTs - m_starveAnchor).count(),
           m_H264queue.size(), m_MVCqueue.size());
      m_bluRay->OnStereoStreamUnrecoverable();
    }
  }

  return CDVDDemuxUtils::AllocateDemuxPacket(0);
}

void CDemuxStreamSSIF::AddMVCExtPacket(DemuxPacket* &mvcExtPkt)
{
  const bool hasTimestamp =
      (mvcExtPkt->dts != DVD_NOPTS_VALUE || mvcExtPkt->pts != DVD_NOPTS_VALUE);

  if (!m_MVCqueue.empty() && mvcExtPkt->pData && mvcExtPkt->iSize > 0)
  {
    const uint8_t* d = mvcExtPkt->pData;
    const int n = mvcExtPkt->iSize;
    int lead = 0;
    while (lead < n && lead < 32 && d[lead] == 0x00)
      lead++;
    const bool startsWithStartCode = (lead >= 2 && lead < n && d[lead] == 0x01);

    if (!startsWithStartCode || !hasTimestamp)
    {
      DemuxPacket* prevPkt = m_MVCqueue.back();
      m_MVCqueue.pop_back();
      if (prevPkt->iSize > MVC_MAX_AU_SIZE - mvcExtPkt->iSize)
      {
        logM(LOGWARNING,
             "CDemuxStreamSSIF::AddMVCExtPacket - dropping {} byte fragment, merged MVC access unit would exceed {} bytes",
             mvcExtPkt->iSize, MVC_MAX_AU_SIZE);
        CDVDDemuxUtils::FreeDemuxPacket(mvcExtPkt);
        mvcExtPkt = nullptr;
        m_MVCqueue.push_back(prevPkt);
        return;
      }
      const int scanFrom = prevPkt->iSize > 16 ? prevPkt->iSize - 16 : 0;
      DemuxPacket* merged = MergePacket(prevPkt, mvcExtPkt);
      if (merged->pData && merged->iSize > scanFrom + 12)
        ParseOFMD(merged->pData + scanFrom, merged->iSize - scanFrom, merged->pts);
      m_MVCqueue.push_back(merged);
      return;
    }
  }

  if (!hasTimestamp)
  {
    logM(LOGDEBUG,
         "CDemuxStreamSSIF::AddMVCExtPacket - dropping timestampless MVC fragment with no pending access unit");
    CDVDDemuxUtils::FreeDemuxPacket(mvcExtPkt);
    mvcExtPkt = nullptr;
    return;
  }

#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> MVC add mvc  packet: pts: {:.3f} dts: {:.3f}", mvcExtPkt->pts*1e-6, mvcExtPkt->dts*1e-6);
#endif
  if (mvcExtPkt->pData && mvcExtPkt->iSize > 0)
    ParseOFMD(mvcExtPkt->pData, mvcExtPkt->iSize, mvcExtPkt->pts);
  m_MVCqueue.push_back(mvcExtPkt);
}

void CDemuxStreamSSIF::ParseOFMD(const uint8_t* data, int size, double pts)
{
  if (size < 12)
    return;

  const uint8_t* end = data + size - 12;
  for (const uint8_t* p = data; p < end; p++)
  {
    if (p[0] != 0x4F || p[1] != 0x46 || p[2] != 0x4D || p[3] != 0x44)
      continue;

    const uint8_t* ofmd = p + 4;
    if (ofmd + 10 > data + size)
      break;

    uint8_t num_frames = ofmd[7];
    uint8_t num_pg_seq = ofmd[8];
    if (num_frames == 0 || num_pg_seq == 0)
      break;

    const uint8_t* offsets = ofmd + 10;
    if (offsets + static_cast<size_t>(num_pg_seq) * num_frames > data + size)
      break;

    std::lock_guard lock(m_subtitleOffsets->m_mutex);
    auto& entries = m_subtitleOffsets->m_entries;
    if (!entries.empty() && entries.back().pts == pts)
      return;

    CMVCSubtitleOffsets::Entry entry;
    entry.pts = pts;
    entry.planeOffsets.resize(num_pg_seq);
    for (int seq = 0; seq < num_pg_seq; seq++)
    {
      uint8_t raw = offsets[seq * num_frames];
      entry.planeOffsets[seq] =
          (raw & 0x80) ? -static_cast<int8_t>(raw & 0x7F) : static_cast<int8_t>(raw & 0x7F);
    }
    entries.push_back(std::move(entry));
    if (entries.size() > OFMD_TABLE_MAX)
      entries.erase(entries.begin());
    break;
  }
}

int CMVCSubtitleOffsets::Get(double pts, int planeId) const
{
  if (planeId < 0)
    return 0;

  std::lock_guard lock(m_mutex);
  const Entry* best = nullptr;
  for (const auto& entry : m_entries)
    if (entry.pts <= pts && (!best || entry.pts >= best->pts))
      best = &entry;

  if (!best || planeId >= static_cast<int>(best->planeOffsets.size()))
    return 0;

  return best->planeOffsets[planeId];
}

void CDemuxStreamSSIF::ResyncExtension(double dtsBase)
{
  CDVDDemux* ext = m_bluRay->GetExtentionDemux();
  if (!ext)
    return;

  while (!m_MVCqueue.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_MVCqueue.front());
    m_MVCqueue.pop_front();
  }

  const bool reseeked = ext->SeekTime(dtsBase / 1000.0, true);
  m_lastExtRecovery = std::chrono::steady_clock::now();
  logM(LOGDEBUG,
       "CDemuxStreamSSIF::ResyncExtension - re-seeking MVC extension to base dts {:.3f}s ({})",
       dtsBase * 1e-6, reseeked ? "accepted" : "rejected");
}

bool CDemuxStreamSSIF::FillMVCQueue(double dtsBase)
{
  if (!m_bluRay)
    return false;

  const bool trace = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                     CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO);
  unsigned int behindBase = 0;
  unsigned int accepted = 0;

  CDVDDemux* demux = m_bluRay->GetExtentionDemux();
  if (!demux)
    return false;
  DemuxPacket* mvc;
  while (!m_bluRay->ExtentionAbortRequested() && (m_MVCqueue.size() < MVC_QUEUE_SIZE) &&
         (mvc = demux->Read()))
  {
    if (dtsBase == DVD_NOPTS_VALUE || mvc->dts == DVD_NOPTS_VALUE)
    {
      // do nothing, can't compare timestamps when they are not set
    }
    else if (mvc->dts < dtsBase)
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC drop mvc: {:6}, pts({:.3f}) dts({:.3f})", mvc->iSize, mvc->pts*1e-6, mvc->dts*1e-6);
#endif
      if (trace)
        ++behindBase;
      const double gap = dtsBase - mvc->dts;
      CDVDDemuxUtils::FreeDemuxPacket(mvc);
      if (gap > MVC_EXT_RESEEK_GAP)
      {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastExtRecovery >= MVC_EXT_RECOVERY_INTERVAL)
        {
          m_lastExtRecovery = now;
          const bool reseeked = demux->SeekTime(dtsBase / 1000.0, true);
          logM(LOGWARNING,
               "CDemuxStreamSSIF::FillMVCQueue - MVC extension {:.1f}s behind base dts {:.3f}s, "
               "re-seeking instead of scanning ({})",
               gap * 1e-6, dtsBase * 1e-6, reseeked ? "accepted" : "rejected");
        }
      }
      continue;
    }
    if (trace)
      ++accepted;
    AddMVCExtPacket(mvc);
  };
  if (m_MVCqueue.size() != MVC_QUEUE_SIZE)
  {
    const bool advanced = m_bluRay->OpenNextStream();
    if (!advanced && m_MVCqueue.empty())
    {
      CDVDDemux* ext = m_bluRay->GetExtentionDemux();
      const auto now = std::chrono::steady_clock::now();
      const bool due = ext && now - m_lastExtRecovery >= MVC_EXT_RECOVERY_INTERVAL;
      bool reseeked = false;
      if (due && dtsBase != DVD_NOPTS_VALUE)
      {
        m_lastExtRecovery = now;
        reseeked = ext->SeekTime(dtsBase / 1000.0, true);
        logM(LOGWARNING,
             "CDemuxStreamSSIF::FillMVCQueue - MVC extension returned no data at base dts {:.3f}s, "
             "re-seeking extension demux ({})",
             dtsBase * 1e-6, reseeked ? "accepted" : "rejected");
      }
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                            "CDemuxStreamSSIF::FillMVCQueue - extension delivered nothing usable, "
                            "base dts {:.3f}s, {} views behind base, {} accepted, recovery {}",
                            dtsBase * 1e-6, behindBase, accepted,
                            !ext                         ? "no extension demux"
                            : dtsBase == DVD_NOPTS_VALUE ? "skipped, base dts not set"
                            : !due                       ? "rate limited"
                            : reseeked                   ? "seek accepted"
                                                         : "seek rejected");
    }
  }

  return true;
}
