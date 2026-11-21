/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDInputStreamBluray.h"
#include "cores/VideoPlayer/BDStageTrace.h"
#include "DVDDemuxers/DemuxStreamSSIF.h"

#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDCodecs/Overlay/DVDOverlayImage.h"
#include "DVDInputStreamFile.h"
#include "DVDDemuxers/DemuxMVC.h"
#include "IVideoPlayer.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AESound.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/File.h"
#include "URL.h"
#include "filesystem/BlurayCallback.h"
#include "filesystem/Directory.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/StereoscopicsManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/DiscSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/Geometry.h"
#include "utils/LangCodeExpander.h"
#include "utils/LogThrottle.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "video/VideoInfoTag.h"
#include "windowing/GraphicContext.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>

#include <libbluray/bluray.h>
#include <libbluray/filesystem.h>
#include <libbluray/mpls_data.h>
#include <libbluray/log_control.h>

extern "C"
{
#include <libavutil/mathematics.h>
}

namespace
{
constexpr int64_t END_OF_TITLE_SPIN_TIMEOUT_MS = 5000;
constexpr unsigned int END_OF_TITLE_SPIN_CLOCK_MASK = 0x3FF;

class CDVDInputStreamBlurayFile : public CDVDInputStream
{
public:
  static constexpr int BD_UNIT = 6144;

  CDVDInputStreamBlurayFile(BD_FILE_H* file, const std::string& filename, int64_t length)
    : CDVDInputStream(DVDSTREAM_TYPE_FILE, CFileItem()),
      m_file(file), m_filename(filename), m_length(length)
  {
  }

  ~CDVDInputStreamBlurayFile() override
  {
    if (m_file)
      m_file->close(m_file);
  }

  bool Open() override { return m_file != nullptr; }
  void Close() override {}

  int Read(uint8_t* buf, int buf_size) override
  {
    if (!m_file || m_eof)
      return m_eof ? 0 : -1;

    int copied = 0;
    while (copied < buf_size)
    {
      if (m_bufPos >= m_bufFill)
      {
        int64_t r = m_file->read(m_file, m_buf, BD_UNIT);
        if (r <= 0)
        {
          m_eof = (r == 0);
          return copied > 0 ? copied : static_cast<int>(r);
        }
        m_filePos += r;
        m_bufFill = static_cast<int>(r);
        m_bufPos = 0;
      }
      int avail = m_bufFill - m_bufPos;
      int take = std::min(avail, buf_size - copied);
      memcpy(buf + copied, m_buf + m_bufPos, take);
      m_bufPos += take;
      copied += take;
    }
    return copied;
  }

  int64_t Seek(int64_t offset, int whence) override
  {
    if (!m_file)
      return -1;

    int64_t target;
    if (whence == SEEK_SET)
      target = offset;
    else if (whence == SEEK_CUR)
      target = (m_filePos - m_bufFill + m_bufPos) + offset;
    else if (whence == SEEK_END)
      target = m_length + offset;
    else
      return -1;
    if (target < 0)
      return -1;

    int64_t blockStart = (target / BD_UNIT) * BD_UNIT;
    int64_t intraBlock = target - blockStart;

    if (m_file->seek(m_file, blockStart, SEEK_SET) < 0)
      return -1;
    m_filePos = blockStart;
    m_bufFill = 0;
    m_bufPos = 0;
    m_eof = false;

    if (intraBlock > 0)
    {
      int64_t rd = m_file->read(m_file, m_buf, BD_UNIT);
      if (rd <= 0)
        return -1;
      m_filePos += rd;
      m_bufFill = static_cast<int>(rd);
      m_bufPos = static_cast<int>(intraBlock) < m_bufFill ? static_cast<int>(intraBlock) : m_bufFill;
    }
    return target;
  }

  bool IsEOF() override { return m_eof; }
  int64_t GetLength() override { return m_length; }
  std::string GetFileName() override { return m_filename; }

private:
  BD_FILE_H* m_file = nullptr;
  std::string m_filename;
  int64_t m_length = 0;
  int64_t m_filePos = 0;
  uint8_t m_buf[BD_UNIT]{};
  int m_bufFill = 0;
  int m_bufPos = 0;
  bool m_eof = false;
};
}

#define LIBBLURAY_BYTESEEK 0
#define EMPTY_QUEUE(x) { while(!x.empty()) x.pop(); }

using namespace XFILE;

using namespace std::chrono_literals;

namespace
{
class BdCloseWorker
{
public:
  static BdCloseWorker& Instance()
  {
    static BdCloseWorker instance;
    return instance;
  }

  void Submit(BLURAY* bd)
  {
    if (!bd)
      return;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.push(bd);
    }
    m_cv.notify_one();
  }

  void WaitForIdle()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_queue.empty() && !m_busy)
      return;
    const auto t_start = std::chrono::steady_clock::now();
    const bool ok = m_idleCv.wait_for(lock, std::chrono::seconds(15),
                                      [this] { return m_queue.empty() && !m_busy; });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t_start).count();
    if (ok)
      logM(LOGDEBUG, "BdCloseWorker::WaitForIdle - waited {}ms for prior bd_close", ms);
    else
      logM(LOGWARNING,
           "BdCloseWorker::WaitForIdle - 15s timeout, proceeding (worker may be stuck)");
  }

  ~BdCloseWorker()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable())
      m_thread.join();
  }

private:
  BdCloseWorker() : m_thread(&BdCloseWorker::Run, this) {}

  void Run()
  {
    for (;;)
    {
      BLURAY* bd = nullptr;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
        if (m_queue.empty())
          return;
        bd = m_queue.front();
        m_queue.pop();
        m_busy = true;
      }
      const auto t_start = std::chrono::steady_clock::now();
      try
      {
        bd_close(bd);
      }
      catch (...)
      {
        logM(LOGERROR, "BdCloseWorker::Run - bd_close threw");
      }
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t_start).count();
      logM(LOGDEBUG, "BdCloseWorker::Run - bd_close completed in {}ms", ms);
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busy = false;
        if (m_queue.empty())
          m_idleCv.notify_all();
      }
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::condition_variable m_idleCv;
  std::queue<BLURAY*> m_queue;
  bool m_busy{false};
  bool m_stopping{false};
  std::thread m_thread;
};
}

static BD_DIR_H* dir_open(void* handle, const char* rel_path)
{
  return static_cast<CDVDInputStreamBluray*>(handle)->OpenDir(rel_path);
}

static BD_FILE_H* file_open(void* handle, const char* rel_path)
{
  return static_cast<CDVDInputStreamBluray*>(handle)->OpenFile(rel_path);
}

#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 1, 0))
namespace
{
// HDMI frame packing stacks both views with one frame's vertical blanking between them
constexpr int FRAME_PACKED_1080_HEIGHT = 2205;
constexpr int FRAME_PACKED_720_HEIGHT = 1470;

bool IsFramePacking(const RESOLUTION_INFO& res, int width, int height, int packedHeight)
{
  if (res.dwFlags & D3DPRESENTFLAG_MODE3DFP)
    return res.iScreenHeight == height;
  return (res.dwFlags & D3DPRESENTFLAG_MODE3DTB) && res.iScreenWidth == width &&
         res.iScreenHeight == packedHeight;
}

uint32_t GetDisplayCap()
{
  uint32_t displayCap = 0;
  const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
  for (size_t i = RES_CUSTOM; i < displaySettings.ResolutionInfoSize(); ++i)
  {
    const RESOLUTION_INFO& res = displaySettings.GetResolutionInfo(i);
    const long refreshRate = std::lround(res.fRefreshRate);
    if (res.dwFlags & D3DPRESENTFLAG_INTERLACED)
    {
      if (res.dwFlags &
          (D3DPRESENTFLAG_MODE3DSBS | D3DPRESENTFLAG_MODE3DTB | D3DPRESENTFLAG_MODE3DFP))
        displayCap |= BLURAY_DCAP_INTERLACED_3D;
    }
    else if (refreshRate == 24 && IsFramePacking(res, 1920, 1080, FRAME_PACKED_1080_HEIGHT))
      displayCap |= BLURAY_DCAP_1080p_720p_3D;
    else if (refreshRate == 50 && IsFramePacking(res, 1280, 720, FRAME_PACKED_720_HEIGHT))
      displayCap |= BLURAY_DCAP_720p_50Hz_3D;
  }
  return displayCap;
}

// the base view plays on any display, 3D output only on a 3D display not set to play 2D
void SetupStereoSettings(BLURAY* bd)
{
  const int playbackMode = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE);
  const uint32_t displayCap = GetDisplayCap();
  const bool prefer3D = displayCap != 0 && playbackMode != STEREOSCOPIC_PLAYBACK_MODE_MONO &&
                        playbackMode != STEREOSCOPIC_PLAYBACK_MODE_IGNORE;
  CLog::Log(LOGDEBUG,
            "CDVDInputStreamBluray::Open - 3D disc, display capability (PSR 23) 0x{:x}, prefer 3D {}",
            displayCap | BLURAY_DCAP_1080p_720p_3D, prefer3D);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_DISPLAY_CAP,
                        displayCap | BLURAY_DCAP_1080p_720p_3D);
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_OUTPUT_PREFER,
                        prefer3D ? BLURAY_OUTPUT_PREFER_3D : BLURAY_OUTPUT_PREFER_2D);
}

unsigned int stream_number(int pid, const BLURAY_STREAM_INFO* streams, uint8_t count)
{
  for (uint8_t i = 0; i < count; ++i)
  {
    if (streams[i].pid == static_cast<uint16_t>(pid))
      return static_cast<unsigned int>(i) + 1;
  }
  return 0;
}

constexpr AVRational BLURAY_CLIP_TIME_BASE{1, 90000};
} // namespace
#endif

static void bluray_overlay_cb(void *this_gen, const BD_OVERLAY * ov)
{
  static_cast<CDVDInputStreamBluray*>(this_gen)->OverlayCallback(ov);
}

#ifdef HAVE_LIBBLURAY_BDJ
void  bluray_overlay_argb_cb(void *this_gen, const struct bd_argb_overlay_s * const ov)
{
  static_cast<CDVDInputStreamBluray*>(this_gen)->OverlayCallbackARGB(ov);
}
#endif

CDVDInputStreamBluray::CDVDInputStreamBluray(IVideoPlayer* player, const CFileItem& fileitem) :
  CDVDInputStream(DVDSTREAM_TYPE_BLURAY, fileitem), m_player(player)
{
  m_content = "video/x-mpegts";
  memset(&m_event, 0, sizeof(m_event));
#ifdef HAVE_LIBBLURAY_BDJ
  memset(&m_argb,  0, sizeof(m_argb));
#endif
}

CDVDInputStreamBluray::~CDVDInputStreamBluray()
{
  Close();
}

void CDVDInputStreamBluray::Abort()
{
  m_hold = HOLD_EXIT;
  m_extAbortRequested.store(true, std::memory_order_relaxed);
}

bool CDVDInputStreamBluray::IsEOF()
{
  return false;
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleFromState(const std::string& xmlstate) const
{
  BlurayState blurayState;
  if (!m_blurayStateSerializer.XMLToBlurayState(blurayState, xmlstate))
  {
    CLog::LogF(LOGWARNING, "Failed to deserialize Bluray state");
    return nullptr;
  }
  return bd_get_playlist_info(m_bd, blurayState.playlistId, 0);
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetMainTitle() const
{
  int titles = bd_get_titles(m_bd, TITLES_RELEVANT, 0);
  if (titles <= 0)
    return nullptr;

  // Prefer libbluray's native heuristic algorithm:
  // Evaluates chapter count, HD audio properties (TrueHD/Atmos/DTS-HD MA), video properties,
  // and filters out ScreenPass fake playlist traps.
  int mainTitleIdx = bd_get_main_title(m_bd);
  if (mainTitleIdx >= 0 && mainTitleIdx < titles)
  {
    BLURAY_TITLE_INFO* info = bd_get_title_info(m_bd, mainTitleIdx, 0);
    if (info)
    {
      logComponentM(LOGDEBUG, LOGBLURAY,
                    "CDVDInputStreamBluray::GetMainTitle - selected main title {} (idx {}) via bd_get_main_title",
                    info->playlist, mainTitleIdx);
      return info;
    }
  }

  logComponentM(LOGDEBUG, LOGBLURAY,
                "CDVDInputStreamBluray::GetMainTitle - bd_get_main_title unavailable, falling back to GetTitleLongest");
  return GetTitleLongest(titles);
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleLongest(int titles) const
{
  if (titles < 0)
    titles = bd_get_titles(m_bd, TITLES_RELEVANT, 0);
  
  BLURAY_TITLE_INFO* s = nullptr;
  for (int i = 0; i < titles; i++)
  {
    BLURAY_TITLE_INFO* t = bd_get_title_info(m_bd, i, 0);
    if (!t)
    {
      CLog::Log(LOGDEBUG, "get_main_title - unable to get title {}", i);
      continue;
    }
    if (!s || s->duration < t->duration)
      std::swap(s, t);

    if (t)
      bd_free_title_info(t);
  }
  return s;
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleFile(const std::string& filename) const {
  unsigned int playlist;
  if(sscanf(filename.c_str(), "%05u.mpls", &playlist) != 1)
  {
    CLog::Log(LOGERROR, "get_playlist_title - unsupported playlist file selected {}",
              CURL::GetRedacted(filename));
    return nullptr;
  }

  return bd_get_playlist_info(m_bd, playlist, 0);
}


namespace
{
void WriteWav(XFILE::CFile& file, const BLURAY_SOUND_EFFECT& effect)
{
  const uint32_t dataSize = effect.num_frames * effect.num_channels * 2;
  const uint16_t channels = effect.num_channels;
  const uint32_t sampleRate = 48000;
  const uint32_t byteRate = sampleRate * channels * 2;
  const uint16_t blockAlign = channels * 2;
  struct __attribute__((packed))
  {
    char riff[4]{'R', 'I', 'F', 'F'};
    uint32_t riffSize;
    char wave[4]{'W', 'A', 'V', 'E'};
    char fmt[4]{'f', 'm', 't', ' '};
    uint32_t fmtSize{16};
    uint16_t audioFormat{1};
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample{16};
    char data[4]{'d', 'a', 't', 'a'};
    uint32_t dataSize;
  } hdr;
  hdr.riffSize = 36 + dataSize;
  hdr.channels = channels;
  hdr.sampleRate = sampleRate;
  hdr.byteRate = byteRate;
  hdr.blockAlign = blockAlign;
  hdr.dataSize = dataSize;
  file.Write(&hdr, sizeof(hdr));
  file.Write(effect.samples, dataSize);
}

void UpdateLibblurayDebugMask()
{
  uint32_t debugMask = DBG_CRIT | DBG_BLURAY | DBG_NAV;
  if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG))
    debugMask |= DBG_HDMV;
  bd_set_debug_mask(debugMask);
}
}

void CDVDInputStreamBluray::LoadMenuSounds()
{
  IAE* ae = CServiceBroker::GetActiveAE();
  if (!ae)
    return;

  constexpr unsigned MAX_SOUND_EFFECTS = 128;
  unsigned loaded = 0;
  for (unsigned id = 0; id < MAX_SOUND_EFFECTS; id++)
  {
    BLURAY_SOUND_EFFECT effect;
    if (bd_get_sound_effect(m_bd, id, &effect) <= 0)
      break;
    if (!effect.samples || effect.num_frames == 0 || effect.num_channels == 0 ||
        effect.num_channels > 2)
    {
      m_menuSounds.emplace_back(nullptr);
      continue;
    }
    const std::string path = StringUtils::Format("special://temp/bluray_sound_{:03}.wav", id);
    XFILE::CFile file;
    if (!file.OpenForWrite(path, true))
    {
      m_menuSounds.emplace_back(nullptr);
      continue;
    }
    WriteWav(file, effect);
    file.Close();
    m_menuSounds.emplace_back(ae->MakeSound(path));
    if (m_menuSounds.back())
      loaded++;
  }
  if (!m_menuSounds.empty())
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "CDVDInputStreamBluray - loaded {}/{} menu sound effects (sound.bdmv)",
                  loaded, m_menuSounds.size());
}

void CDVDInputStreamBluray::FreeMenuSounds()
{
  const size_t count = m_menuSounds.size();
  m_menuSounds.clear();
  for (size_t id = 0; id < count; id++)
    XFILE::CFile::Delete(StringUtils::Format("special://temp/bluray_sound_{:03}.wav", id));
}

void CDVDInputStreamBluray::PlayMenuSound(uint32_t id)
{
  if (id < m_menuSounds.size() && m_menuSounds[id])
  {
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_SOUND_EFFECT {}: play", id);
    m_menuSounds[id]->Play();
  }
  else
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_SOUND_EFFECT {}: no sound cached", id);
}

bool CDVDInputStreamBluray::Open()
{
  BdCloseWorker::Instance().WaitForIdle();

  if(m_player == nullptr)
    return false;

  m_playerThread = std::this_thread::get_id();

  std::string strPath(m_item.GetDynPath());
  std::string filename;
  std::string root;

  bool openDisc = false;
  bool resumable = true;

  // The item was selected via the simple menu
  if (URIUtils::IsProtocol(strPath, "bluray"))
  {
    CURL url(strPath);
    root = url.GetHostName();
    filename = URIUtils::GetFileName(url.GetFileName());

    // Disc images mounted via udf:// must always use files mode (bd_open_files),
    // as bd_open_disc only supports local physical drive devices and cannot handle udf:// URLs.
    // Furthermore, probing IsProtectedBlurayDisc on a stripped ISO path triggers false-positive
    // HTTP 200 responses from streaming proxies (such as emby-next-gen).
    CURL url2(root);
    if (!url2.IsProtocol("udf") && !URIUtils::IsRemote(root))
    {
      CFileItem item(url2, false);
      openDisc = item.IsProtectedBlurayDisc();
    }

    // check for a menu call for an image file
    if (StringUtils::EqualsNoCase(filename, "menu"))
    {
      resumable = false;

      // A menu on a disc image is opened below in files mode like any other disc: root already
      // names the image, and a menu on a BDMV folder has always been read that way.
    }
  }
  else if (m_item.IsDiscImage())
  {
    // Read the image through Kodi's filesystem rather than streaming raw 2048-byte blocks.
    // Stream mode reads through a file opened READ_NO_CACHE, so every block libbluray asks for
    // becomes its own request to the source, and it shares nothing with the cache the directory
    // above read the same image through. The dynamic path is the resolved one, so a .strm
    // pointing at an image reaches the image itself rather than the .strm.
    CURL url2("udf://");

    url2.SetHostName(m_item.GetDynPath());
    root = url2.Get();
  }
  else if (!URIUtils::IsRemote(m_item.GetDynPath()) && m_item.IsProtectedBlurayDisc())
  {
    openDisc = true;
  }
  else
  {
    strPath = URIUtils::GetDirectory(strPath);
    URIUtils::RemoveSlashAtEnd(strPath);

    if(URIUtils::GetFileName(strPath) == "PLAYLIST")
    {
      strPath = URIUtils::GetDirectory(strPath);
      URIUtils::RemoveSlashAtEnd(strPath);
    }

    if(URIUtils::GetFileName(strPath) == "BDMV")
    {
      strPath = URIUtils::GetDirectory(strPath);
      URIUtils::RemoveSlashAtEnd(strPath);
    }
    root = strPath;
    filename = URIUtils::GetFileName(m_item.GetDynPath());
  }

  // root should not have trailing slash
  URIUtils::RemoveSlashAtEnd(root);

  bd_set_debug_handler(CBlurayCallback::bluray_logger);
  UpdateLibblurayDebugMask();

  m_bd = bd_init();

  if (!m_bd)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to initialize libbluray");
    return false;
  }

  SetupPlayerSettings();

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - opening {}", CURL::GetRedacted(root));

  if (openDisc)
  {
    // This special case is required for opening original AACS protected Blu-ray discs. Otherwise
    // things like Bus Encryption might not be handled properly and playback will fail.
    m_rootPath = root;
    if (!bd_open_disc(m_bd, root.c_str(), nullptr))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to open {} in disc mode",
                CURL::GetRedacted(root));
      return false;
    }
  }
  else
  {
    m_rootPath = root;

#if defined(HAS_UDFREAD)
    // Only in files mode does libbluray reach the disc through Kodi's filesystem, opening a dozen
    // or so files and directories on it, and then the clips as they play. On a disc image each of
    // those opens would otherwise re-mount the image's UDF volume, so keep it mounted for as long
    // as the disc is open. (Stream mode reads the image itself, disc mode is a physical disc.)
    m_udfMount.emplace(root);
#endif

    if (!bd_open_files(m_bd, this, dir_open, file_open))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to open {} in files mode",
                CURL::GetRedacted(root));
      return false;
    }
  }

  bd_get_event(m_bd, nullptr);

  m_root = root;
  const BLURAY_DISC_INFO *disc_info = bd_get_disc_info(m_bd);

  if (!disc_info)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - bd_get_disc_info() failed");
    return false;
  }

#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 1, 0))
  // libbluray's 3D setup assumes a 3D display with 3D output preferred
  if (disc_info->content_exist_3D)
    SetupStereoSettings(m_bd);
  else
#endif
    ApplyUHDCapabilities();

  if (disc_info->bluray_detected)
  {
#if (BLURAY_VERSION > BLURAY_VERSION_CODE(1,0,0))
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - Disc name           : {}",
              disc_info->disc_name ? disc_info->disc_name : "");
#endif
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - First Play supported: {}",
              disc_info->first_play_supported);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - Top menu supported  : {}",
              disc_info->top_menu_supported);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - HDMV titles         : {}",
              disc_info->num_hdmv_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD-J titles         : {}",
              disc_info->num_bdj_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD-J handled        : {}",
              disc_info->bdj_handled);
    m_topMenuIsBdj = disc_info->top_menu ? disc_info->top_menu->bdj != 0
                                         : disc_info->bdj_detected != 0;
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - UNSUPPORTED titles  : {}",
              disc_info->num_unsupported_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - AACS detected       : {}",
              disc_info->aacs_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - libaacs detected    : {}",
              disc_info->libaacs_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - AACS handled        : {}",
              disc_info->aacs_handled);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD+ detected        : {}",
              disc_info->bdplus_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - libbdplus detected  : {}",
              disc_info->libbdplus_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD+ handled         : {}",
              disc_info->bdplus_handled);
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1,0,0))
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - no menus (libmmbd, or profile 6 bdj)  : {}",
              disc_info->no_menu_support);
#endif
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - 3D content exist    : {}", disc_info->content_exist_3D);
  }
  else
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - BluRay not detected");

  if (disc_info->aacs_detected && !disc_info->aacs_handled)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Media stream scrambled/encrypted with AACS");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    return false;
  }

  if (disc_info->bdplus_detected && !disc_info->bdplus_handled)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Media stream scrambled/encrypted with BD+");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    return false;
  }

  int mode = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_DISC_PLAYBACK);

  if (URIUtils::HasExtension(filename, ".mpls"))
  {
    m_navmode = false;
    ReplaceTitleInfo(GetTitleFile(filename));
  }
  else if (resumable && m_item.GetStartOffset() == STARTOFFSET_RESUME && m_item.IsResumable())
  {
    m_navmode = false;
    ReplaceTitleInfo(GetTitleFromState(m_item.GetVideoInfoTag()->GetResumePoint().playerState));
  }
  else if (mode == BD_PLAYBACK_MAIN_TITLE)
  {
    m_navmode = false;
    ReplaceTitleInfo(GetMainTitle());
  }
  else
  {
    m_navmode = true;
    if (!disc_info->first_play_supported)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Can't play disc in HDMV navigation mode - First Play title not supported");
      m_navmode = false;
    }

    if (m_navmode && disc_info->num_unsupported_titles > 0) {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Unsupported titles found - Some titles can't be played in navigation mode");
    }

    if(!m_navmode)
      ReplaceTitleInfo(GetMainTitle());
  }

  UpdateGraphicsRegime();

  if (m_navmode)
  {

    bd_register_overlay_proc (m_bd, this, bluray_overlay_cb);
#ifdef HAVE_LIBBLURAY_BDJ
    bd_register_argb_overlay_proc (m_bd, this, bluray_overlay_argb_cb, nullptr);
#endif
    bd_set_presentation_hold(m_bd, 1);

    LoadMenuSounds();

    if(bd_play(m_bd) <= 0)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed play disk {}",
                CURL::GetRedacted(strPath));
      return false;
    }
    m_hold = HOLD_DATA;
  }
  else
  {
    if(!m_titleInfo)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to get title info");
      return false;
    }

    if(!bd_select_playlist(m_bd, m_titleInfo->playlist))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to select playlist {}",
                m_titleInfo->idx);
      return false;
    }
  }

  // Process any events that occurred during opening
  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();
  m_playlistLeft = false;
  m_itemSeeked = false;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (m_clip)
      RecordTitleTimeSpan(m_clip, true);
  }

  OpenNextStream();

  aml_dv_set_disc_session(true);

  return true;
}

bool CDVDInputStreamBluray::IsInMainOrHdmvMenu() const
{
  if (m_bd == nullptr || !m_navmode)
    return false;
  if (m_isInMainMenu || (!m_isBdjTitle.load(std::memory_order_relaxed) && m_menu))
    return true;
  const bool seekProhibited = (m_uoMask.load() & BLURAY_UO_TIME_SEARCH_MASK) != 0;
  return m_hasMenuOverlay && seekProhibited;
}

bool CDVDInputStreamBluray::IsFeaturePlaylistActive()
{
  if (m_playlist > MAX_PLAYLIST_ID)
    return false;
  if (IsInMainOrHdmvMenu())
    return false;
  if (m_featurePlaylist <= MAX_PLAYLIST_ID && m_playlist == m_featurePlaylist)
    return true;
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  return m_titleInfo && m_titleInfo->duration >= 10 * 60 * 90000ULL;
}

bool CDVDInputStreamBluray::IsOnFeaturePlaylist()
{
  if (m_playlist > MAX_PLAYLIST_ID || m_featurePlaylist > MAX_PLAYLIST_ID)
    return false;
  if (IsInMainOrHdmvMenu())
    return false;
  return m_playlist == m_featurePlaylist;
}

// close file and reset everything
void CDVDInputStreamBluray::Close()
{
  aml_dv_set_disc_session(false);
  FreeMenuSounds();
  CloseMVCDemux();
  ReplaceTitleInfo(nullptr);
  FreePrevTitleInfo();
  m_crossPlaylistPending = false;
  m_videoCompatBoundary = false;
  m_naturalChainBoundary = false;
  m_heldEvents = {};
  {
    std::lock_guard<std::mutex> lock(m_titleTimeSpanMutex);
    m_titleTimeSpans.clear();
  }
  m_endedTitleTime = 0;
  m_titleEndWait = false;
  m_lastPictureTime = -1;
  m_presentedTime = -1;
  m_clockTime = -1;
  m_menuInput = false;
  m_itemAbandoned = false;
  m_notificationsHeld = false;
  m_pendingPqGraphics.reset();
  m_presentedLeft = false;
  m_readSinceReport = false;
  m_heldDue = -2;
  m_reportedTime = -1;
  m_playedItemEnd = -1;
  m_bdjGraphicsActive = false;
  m_isBdjTitle = false;
  m_hasOverlay = false;
  m_hasMenuOverlay = false;
  m_hasVisibleBdjOverlay = false;
  {
    std::lock_guard lock(m_overlayLock);
    m_bdjPopup = BdjPopup::CLOSED;
    m_pendingOverlayGroup.reset();
    ResetBdjSurfaceLocked();
  }

  if(m_bd)
  {
    bd_register_overlay_proc(m_bd, nullptr, nullptr);
#ifdef HAVE_LIBBLURAY_BDJ
    bd_register_argb_overlay_proc(m_bd, nullptr, nullptr, nullptr);
#endif
    BdCloseWorker::Instance().Submit(m_bd);
    m_bd = nullptr;
  }

  aml_set_bdj_overlay_active(false);

  m_rootPath.clear();
}

void CDVDInputStreamBluray::ReplaceTitleInfo(BLURAY_TITLE_INFO* incoming)
{
  BLURAY_TITLE_INFO* outgoing = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    outgoing = m_titleInfo;
    m_titleInfo = incoming;
    m_clip = nullptr;
    m_nMVCClip = nullptr;
    m_endedTitleTime = 0;
  }

  if (outgoing)
    bd_free_title_info(outgoing);
}

// the player still plays the title out of its queues until the next stream
void CDVDInputStreamBluray::ReleaseTitle()
{
  const int totalTime = GetTotalTime();
  if (m_titleInfo && m_clip)
    StashBoundaryClip();
  else
    ReplaceTitleInfo(nullptr);
  m_endedTitleTime = totalTime;
}

// a seek before the held end plays the title on, so its end is read and raised again
void CDVDInputStreamBluray::ResetTitleEnd()
{
  if (!m_titleEndWait)
    return;

  m_titleEndWait = false;
  m_playlistLeft = false;
  m_lastPictureTime = -1;
}

BD_DIR_H* CDVDInputStreamBluray::OpenDir(const char* relPath)
{
  return CBlurayCallback::dir_open(&m_rootPath, relPath);
}

#ifdef BD_FILE_OPEN_DIRECT_IO_HINT
static bool IsRemoteUdfImage(const std::string& root)
{
  return CURL(root).IsProtocol("udf") && URIUtils::IsRemote(root);
}
#endif

BD_FILE_H* CDVDInputStreamBluray::OpenFile(const char* relPath)
{
  unsigned int flags = 0;
  if (m_navmode && URIUtils::HasExtension(relPath, ".m2ts|.ssif"))
  {
    flags = XFILE::READ_AUDIO_VIDEO;
#ifdef BD_FILE_OPEN_DIRECT_IO_HINT
    // Read MVC packets on demand so two cache workers cannot reposition the same UDF stream.
    if (bd_file_open_use_direct_io() && IsRemoteUdfImage(m_rootPath))
    {
      flags = XFILE::READ_NO_CACHE;
      CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OpenFile - reading {} on demand", relPath);
    }
#endif
  }
  return CBlurayCallback::file_open(&m_rootPath, relPath, flags);
}

void CDVDInputStreamBluray::RecordTitleTimeSpan(const BLURAY_CLIP_INFO* clip, bool reset)
{
  if (!clip)
    return;

  double seamOffset = 0.0;
  {
    std::lock_guard<std::mutex> lock(m_seamOffsetMutex);
    seamOffset = m_seamTimeOffset;
  }
  const int64_t offset90k = std::llround(seamOffset * 90000.0);
  TitleTimeSpan span;
  span.ptsThreshold90k = static_cast<int64_t>(clip->in_time) + offset90k;
  span.base90k =
      static_cast<int64_t>(clip->start_time) - static_cast<int64_t>(clip->in_time) - offset90k;

  std::lock_guard<std::mutex> lock(m_titleTimeSpanMutex);
  if (reset)
    m_titleTimeSpans.clear();
  if (!m_titleTimeSpans.empty() && m_titleTimeSpans.back().ptsThreshold90k == span.ptsThreshold90k)
    m_titleTimeSpans.back() = span;
  else
  {
    m_titleTimeSpans.push_back(span);
    if (m_titleTimeSpans.size() > 32)
      m_titleTimeSpans.erase(m_titleTimeSpans.begin());
  }
}

int64_t CDVDInputStreamBluray::ConvertPtsToTitleTime90k(double ptsDvdTime) const
{
  if (ptsDvdTime == DVD_NOPTS_VALUE)
    return -1;

  const int64_t pts90k = av_rescale_q(static_cast<int64_t>(ptsDvdTime),
                                      AVRational{1, DVD_TIME_BASE}, BLURAY_CLIP_TIME_BASE);
  std::lock_guard<std::mutex> lock(m_titleTimeSpanMutex);
  if (m_titleTimeSpans.empty())
    return -1;

  const TitleTimeSpan* chosen = &m_titleTimeSpans.front();
  for (const auto& span : m_titleTimeSpans)
  {
    if (span.ptsThreshold90k <= pts90k)
      chosen = &span;
    else
      break;
  }
  return std::max<int64_t>(0, pts90k + chosen->base90k);
}

void CDVDInputStreamBluray::FreePrevTitleInfo()
{
  if (m_prevTitleInfo)
    bd_free_title_info(m_prevTitleInfo);

  m_prevTitleInfo = nullptr;
  m_prevClip = nullptr;
}

void CDVDInputStreamBluray::StashBoundaryClip()
{
  if (!m_titleInfo || !m_clip)
    return;

  FreePrevTitleInfo();
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  if (!m_titleInfo || !m_clip)
    return;

  m_prevTitleInfo = m_titleInfo;
  m_prevClip = m_clip;
  m_prevPlaylist = m_playlist;
  m_prevWasMVC = m_bMVCPlayback;
  m_prevFlipEyes = m_bFlipEyes;
  m_titleInfo = nullptr;
  m_clip = nullptr;
  m_nMVCClip = nullptr;
}

void CDVDInputStreamBluray::UpdateSeamTimeOffset(const BLURAY_CLIP_INFO* prev,
                                                const BLURAY_CLIP_INFO* next)
{
  constexpr double minStepSeconds = 2.0;

  if (!prev || !next || m_bMVCPlayback)
    return;

  const double step =
      (static_cast<double>(prev->out_time) - static_cast<double>(next->in_time)) / 90000.0;
  std::lock_guard<std::mutex> lock(m_seamOffsetMutex);
  if (std::fabs(step) < minStepSeconds)
  {
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "CDVDInputStreamBluray - seam offset step {:.3f}s below threshold, offset stays "
                  "{:.3f}s",
                  step, m_seamTimeOffset);
    return;
  }

  m_seamTimeOffsetPrev = m_seamTimeOffset;
  m_seamTimeOffset += step;
  m_seamGeneration++;
  logComponentM(LOGDEBUG, LOGBLURAY,
                "CDVDInputStreamBluray - seam offset step {:.3f}s applied, offset now {:.3f}s gen {}",
                step, m_seamTimeOffset, m_seamGeneration);
}

void CDVDInputStreamBluray::ResetSeamTimeOffset(const char* reason)
{
  std::lock_guard<std::mutex> lock(m_seamOffsetMutex);
  if (m_seamGeneration == 0 && m_seamTimeOffset == 0.0)
    return;

  m_seamTimeOffsetPrev = 0.0;
  m_seamTimeOffset = 0.0;
  m_seamGeneration++;
  logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - seam offset reset ({}), gen {}",
                reason, m_seamGeneration);
}

bool CDVDInputStreamBluray::AreClipVideoStreamsCompatible(const BLURAY_CLIP_INFO* a,
                                                          const BLURAY_CLIP_INFO* b)
{
  if (!a || !b)
    return false;
  if (a->video_stream_count < 1 || a->video_stream_count != b->video_stream_count)
    return false;
  if (a->dv_stream_count != b->dv_stream_count)
    return false;
  for (uint8_t i = 0; i < a->video_stream_count; ++i)
  {
    if (a->video_streams[i].pid != b->video_streams[i].pid ||
        a->video_streams[i].coding_type != b->video_streams[i].coding_type ||
        a->video_streams[i].format != b->video_streams[i].format ||
        a->video_streams[i].rate != b->video_streams[i].rate ||
        a->video_streams[i].dynamic_range_type != b->video_streams[i].dynamic_range_type ||
        a->video_streams[i].hdr_plus_flag != b->video_streams[i].hdr_plus_flag)
      return false;
  }
  for (uint8_t i = 0; i < a->dv_stream_count; ++i)
  {
    if (a->dv_streams[i].pid != b->dv_streams[i].pid)
      return false;
  }
  return true;
}

bool CDVDInputStreamBluray::AreClipPgStreamsEqual(const BLURAY_CLIP_INFO* a,
                                                  const BLURAY_CLIP_INFO* b)
{
  if (!a || !b)
    return false;
  if (a->pg_stream_count != b->pg_stream_count)
    return false;
  for (uint8_t i = 0; i < a->pg_stream_count; ++i)
  {
    if (a->pg_streams[i].pid != b->pg_streams[i].pid ||
        a->pg_streams[i].coding_type != b->pg_streams[i].coding_type)
      return false;
  }
  return true;
}

bool CDVDInputStreamBluray::IsClipCodecCompatible(const BLURAY_CLIP_INFO* a,
                                                  const BLURAY_CLIP_INFO* b) const
{
  if (!a || !b) return false;
  if (a->video_stream_count != b->video_stream_count) return false;
  if (a->audio_stream_count != b->audio_stream_count) return false;
  for (uint8_t i = 0; i < a->video_stream_count; ++i)
  {
    if (a->video_streams[i].pid != b->video_streams[i].pid ||
        a->video_streams[i].coding_type != b->video_streams[i].coding_type ||
        a->video_streams[i].format != b->video_streams[i].format ||
        a->video_streams[i].rate != b->video_streams[i].rate)
      return false;
  }
  for (uint8_t i = 0; i < a->audio_stream_count; ++i)
  {
    if (a->audio_streams[i].pid != b->audio_streams[i].pid ||
        a->audio_streams[i].coding_type != b->audio_streams[i].coding_type ||
        a->audio_streams[i].format != b->audio_streams[i].format ||
        a->audio_streams[i].rate != b->audio_streams[i].rate)
      return false;
  }
  return true;
}

void CDVDInputStreamBluray::ProcessEvent() {

  int pid = -1, ret;
  const bool titleEndWait = m_titleEndWait;
  switch (m_event.event) {

   /* errors */

  case BD_EVENT_ERROR:
    switch (m_event.param)
    {
    case BD_ERROR_HDMV:
    case BD_ERROR_BDJ:
      m_player->OnDiscNavResult(nullptr, BD_EVENT_MENU_ERROR);
      break;
    default:
      break;
    }
    logM(LOGERROR, "BD_EVENT_ERROR: Fatal error. Playback can't be continued.");
    m_hold = HOLD_ERROR;
    m_titleEndWait = false;
    break;

  case BD_EVENT_READ_ERROR:
    logComponentM(LOGERROR, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_READ_ERROR");
    break;

  case BD_EVENT_ENCRYPTED:
    logM(LOGERROR, "BD_EVENT_ENCRYPTED");
    switch (m_event.param)
    {
    case BD_ERROR_AACS:
      logM(LOGERROR, "BD_ERROR_AACS");
      break;
    case BD_ERROR_BDPLUS:
      logM(LOGERROR, "BD_ERROR_BDPLUS");
      break;
    default:
      break;
    }
    m_hold = HOLD_ERROR;
    m_titleEndWait = false;
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    break;

  /* playback control */

  case BD_EVENT_SEEK:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_SEEK");
    ResetSeamTimeOffset("seek");
    ResetTitleEnd();
    // a disc program seek after the released title end plays the title again; a seek within the
    // play item raises no play item event, so the seek time names the clip
    if (!m_titleInfo && m_playlist <= MAX_PLAYLIST_ID)
    {
      ReplaceTitleInfo(bd_get_playlist_info(m_bd, m_playlist, m_angle));
      const uint64_t seekTime = static_cast<uint64_t>(m_event.param) * 2;
      std::lock_guard<std::mutex> lock(m_clipTableMutex);
      for (uint32_t i = 0; m_titleInfo && i < m_titleInfo->clip_count; i++)
      {
        if (m_titleInfo->clips[i].start_time > seekTime)
          break;
        m_clip = &m_titleInfo->clips[i];
      }
      if (m_clip)
        RecordTitleTimeSpan(m_clip, true);
    }
    else
    {
      std::lock_guard<std::mutex> lock(m_clipTableMutex);
      if (m_clip)
        RecordTitleTimeSpan(m_clip, true);
    }
    m_itemSeeked = true;
    break;

  case BD_EVENT_STILL_TIME:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_STILL_TIME {}", m_event.param);
    pid = m_event.param;
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_STILL_TIME);
    m_hold = HOLD_STILL;
    break;

  case BD_EVENT_STILL:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_STILL {}", m_event.param);

    pid = m_event.param;

    if (pid == 1)
    {
      m_bdStillActive = true;
      m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_STILL);
    }
    else if (pid == 0 && m_bdStillActive)
    {
      m_bdStillActive = false;
      m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_STILL);
    }
    break;

  case BD_EVENT_DISCONTINUITY:
    if (m_seamlessPlayItem)
    {
      m_seamlessPlayItem = false;
      logComponentM(LOGDEBUG, LOGBLURAY,
                    "CDVDInputStreamBluray - BD_EVENT_DISCONTINUITY suppressed after seamless playitem");
      m_hold = HOLD_NONE;
      break;
    }
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_DISCONTINUITY");
    m_player->OnDiscNavResult(&m_event.param, BD_EVENT_DISCONTINUITY);
    m_hold = HOLD_NONE;
    break;

    /* playback position */

  case BD_EVENT_ANGLE:
  {
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_ANGLE {}", m_event.param);
    const bool angleReannounce = m_event.param == m_angle && m_titleInfo;
    m_angle = m_event.param;

    if (!angleReannounce && m_playlist <= MAX_PLAYLIST_ID)
    {
      ReplaceTitleInfo(bd_get_playlist_info(m_bd, m_playlist, m_angle));
      UpdateGraphicsRegime();
    }
    break;
  }

  case BD_EVENT_END_OF_TITLE:
    LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGBLURAY, 1000,
                          "CDVDInputStreamBluray - BD_EVENT_END_OF_TITLE {}", m_event.param);
    m_playlistLeft = true;
    // a held end keeps the title until its last picture is presented
    if (m_navmode && !m_titleEndWait)
      ReleaseTitle();
    break;

  case BD_EVENT_TITLE:
  {
    UpdateLibblurayDebugMask();
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_TITLE {}", m_event.param);
    const BLURAY_DISC_INFO* disc_info = bd_get_disc_info(m_bd);
    m_titleEndWait = false;
    m_playlistLeft = true;
    m_pendingPqGraphics.reset();
    m_bdjPopup = BdjPopup::CLOSED;

#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 1, 0))
    if (!disc_info || !disc_info->content_exist_3D)
#endif
      ApplyUHDCapabilities();

    m_isInMainMenu = false;

    if (m_event.param == BLURAY_TITLE_TOP_MENU)
    {
      m_title = disc_info->top_menu;
      m_isInMainMenu = true;
    }
    else if (m_event.param == BLURAY_TITLE_FIRST_PLAY)
      m_title = disc_info->first_play;
    else if (m_event.param <= disc_info->num_titles)
      m_title = disc_info->titles[m_event.param];
    else
      m_title = nullptr;

    m_isBdjTitle.store(m_title && m_title->bdj, std::memory_order_relaxed);
    m_titleNumber = m_event.param;
    break;
  }
  case BD_EVENT_PLAYLIST:
    if (!m_crossPlaylistPending)
      ResetSeamTimeOffset("playlist");
    UpdateLibblurayDebugMask();
    if (CServiceBroker::GetLogging().CanLogComponent(LOGBLURAY))
    {
      const auto plNow = std::chrono::steady_clock::now();
      if (m_vmDiagPlaylist >= 0 && m_vmDiagPlaylistEnter.time_since_epoch().count() != 0)
        logComponentM(LOGDEBUG, LOGBLURAY, "bdpl: playlist={} dwellMs={} next={}", m_vmDiagPlaylist,
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          plNow - m_vmDiagPlaylistEnter)
                          .count(),
                      m_event.param);
      m_vmDiagPlaylist = static_cast<int>(m_event.param);
      m_vmDiagPlaylistEnter = plNow;
    }
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_PLAYLIST {}", m_event.param);
    m_titleEndWait = false;
    m_playlistLeft = true;
    m_pendingPqGraphics.reset();
    if (m_menuRestorePlaylist <= MAX_PLAYLIST_ID)
    {
      if (m_event.param == m_menuRestorePlaylist && !m_menu)
      {
        m_menu = true;
        logComponentM(LOGDEBUG, LOGBLURAY,
                      "BD_EVENT_PLAYLIST {} menu playlist restarted, restoring menu state",
                      m_event.param);
      }
      m_menuRestorePlaylist = MAX_PLAYLIST_ID + 1;
    }
    BDSTAGE::Playlist(static_cast<int>(m_event.param), m_menu);
    if (m_overlayCloseDeferred && m_event.param != m_playlist)
      OverlayClose();
    if (m_event.param == m_playlist && m_titleInfo)
    {
      logComponentM(LOGDEBUG, LOGBLURAY,
                    "CDVDInputStreamBluray - BD_EVENT_PLAYLIST {} re-announced, keeping title info",
                    m_event.param);
      break;
    }
    m_playlist = m_event.param;
    ProcessItem(m_playlist);
    break;

  case BD_EVENT_PLAYITEM:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_PLAYITEM {}", m_event.param);
    ResetTitleEnd();
    {
      int resolvedAudioPid = -1;
      int resolvedPgPid = -1;
      bool clipValid = false;
      {
        std::lock_guard<std::mutex> lock(m_clipTableMutex);
        if (m_titleInfo && m_event.param < m_titleInfo->clip_count)
        {
          m_clip = &m_titleInfo->clips[m_event.param];
          clipValid = true;
          RecordTitleTimeSpan(m_clip, !m_seamlessPlayItem);
          if (m_clip->video_stream_count > 0)
          {
            bool pq = false;
            for (uint8_t i = 0; i < m_clip->video_stream_count; ++i)
            {
              const uint8_t range = m_clip->video_streams[i].dynamic_range_type;
              if (range == BLURAY_DYNAMIC_RANGE_HDR10 || range == BLURAY_DYNAMIC_RANGE_DOLBY_VISION)
              {
                pq = true;
                break;
              }
            }
            // with notifications held the disc program draws the pages of the item once it is shown
            if (m_notificationsHeld && m_title && m_title->bdj)
            {
              m_pendingPqGraphics = pq;
              m_pendingPqGraphicsStart = static_cast<int64_t>(m_clip->start_time);
            }
            else
            {
              m_pendingPqGraphics.reset();
              m_pqAuthoredGraphics.store(pq, std::memory_order_relaxed);
            }
          }
          if (m_audioStreamNumber > 0 &&
              m_audioStreamNumber <= static_cast<uint32_t>(m_clip->audio_stream_count))
            resolvedAudioPid = m_clip->audio_streams[m_audioStreamNumber - 1].pid;
          if (m_pgStreamNumber > 0 &&
              m_pgStreamNumber <= static_cast<uint32_t>(m_clip->pg_stream_count))
            resolvedPgPid = m_clip->pg_streams[m_pgStreamNumber - 1].pid;
        }
      }
      // Re-resolve active PSR audio/PG stream numbers against the new PlayItem's STN table.
      // libbluray only emits BD_EVENT_AUDIO_STREAM / BD_EVENT_PG_TEXTST_STREAM on PlayItem
      // transitions when PSR1/PSR2 is out of range for the new clip; if a playlist transitions
      // from a silent PlayItem (where PID resolved to -1) to one with audio/subtitles, or
      // remaps the same stream number to a different PID, refresh VideoPlayer's selected PID.
      if (m_navmode && clipValid && m_player)
      {
        if (m_audioStreamNumber > 0)
        {
          m_audioStreamPick = false;
          logComponentM(LOGDEBUG, LOGBLURAY,
                        "CDVDInputStreamBluray - BD_EVENT_PLAYITEM {} resolved audio stream {} -> pid {}",
                        m_event.param, m_audioStreamNumber, resolvedAudioPid);
          m_player->OnDiscNavResult(static_cast<void*>(&resolvedAudioPid), BD_EVENT_AUDIO_STREAM);
        }
        if (m_pgStreamNumber > 0)
        {
          logComponentM(LOGDEBUG, LOGBLURAY,
                        "CDVDInputStreamBluray - BD_EVENT_PLAYITEM {} resolved pg stream {} -> pid {}",
                        m_event.param, m_pgStreamNumber, resolvedPgPid);
          m_player->OnDiscNavResult(static_cast<void*>(&resolvedPgPid), BD_EVENT_PG_TEXTST_STREAM);
        }
      }
    }
    uint64_t clip_start, clip_in, bytepos;
    ret = bd_get_clip_infos(m_bd, m_event.param, &clip_start, &clip_in, &bytepos, nullptr);
    if (ret)
      m_clipStartTime = clip_start / 90;
    break;

  case BD_EVENT_CHAPTER:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_CHAPTER {}", m_event.param);
    break;

    /* stream selection */

  case BD_EVENT_AUDIO_STREAM:
    pid = -1;
    {
      std::lock_guard<std::mutex> lock(m_clipTableMutex);
      if (m_titleInfo && m_clip &&
          static_cast<uint32_t>(m_clip->audio_stream_count) > (m_event.param - 1))
        pid = m_clip->audio_streams[m_event.param - 1].pid;
      m_audioStreamPick = pid >= 0 && m_event.param != m_audioStreamNumber && m_titleInfo &&
                          m_clip && m_audioStreamNumber > 0 &&
                          m_audioStreamNumber <= static_cast<uint32_t>(m_clip->audio_stream_count);
    }
    m_audioStreamNumber = m_event.param;
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_AUDIO_STREAM {} {}", m_event.param, pid);
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_AUDIO_STREAM);
    break;

  case BD_EVENT_PG_TEXTST:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_PG_TEXTST {}", m_event.param);
    pid = m_event.param;
    m_pgEnabled = (m_event.param != 0);
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_PG_TEXTST);
    break;

  case BD_EVENT_PG_TEXTST_STREAM:
    pid = -1;
    {
      std::lock_guard<std::mutex> lock(m_clipTableMutex);
      if (m_titleInfo && m_clip &&
          static_cast<uint32_t>(m_clip->pg_stream_count) > (m_event.param - 1))
        pid = m_clip->pg_streams[m_event.param - 1].pid;
    }
    m_pgStreamNumber = m_event.param;
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_PG_TEXTST_STREAM {}, {}", m_event.param,
              pid);
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_PG_TEXTST_STREAM);
    break;

  case BD_EVENT_MENU:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_MENU {}", m_event.param);
    m_menu = (m_event.param != 0);
    m_menuRestorePlaylist = MAX_PLAYLIST_ID + 1;
    if (!m_menu)
      m_isInMainMenu = false;
    m_player->OnDiscNavResult(&m_event.param, BD_EVENT_MENU);
    break;

  case BD_EVENT_IDLE:
    // the disc program plays no playlist until its next act, as after a released title end
    if (m_navmode && m_event.param == 0)
    {
      if (!m_titleEndWait)
        ReleaseTitle();
      m_titleEndWait = true;
    }
    else
      KODI::TIME::Sleep(100ms);
    break;

  case BD_EVENT_SOUND_EFFECT:
    PlayMenuSound(m_event.param);
    break;

  case BD_EVENT_IG_STREAM:
  case BD_EVENT_SECONDARY_AUDIO:
  case BD_EVENT_SECONDARY_AUDIO_STREAM:
  case BD_EVENT_SECONDARY_VIDEO:
  case BD_EVENT_SECONDARY_VIDEO_SIZE:
  case BD_EVENT_SECONDARY_VIDEO_STREAM:
  case BD_EVENT_KEY_INTEREST_TABLE:
  case BD_EVENT_PIP_PG_TEXTST:
  case BD_EVENT_PIP_PG_TEXTST_STREAM:
    break;

  case BD_EVENT_PLAYMARK:
    // a seek the disc program makes at a mark is not the answer to a key
    m_menuInput = false;
    break;

  case BD_EVENT_BDJ_GRAPHICS:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_BDJ_GRAPHICS {}",
                  m_event.param);
    m_bdjGraphicsActive = (m_event.param != 0);
    aml_set_bdj_overlay_active((!m_isBdjTitle.load(std::memory_order_relaxed) && m_menu) ||
                               m_hasVisibleBdjOverlay ||
                               (m_hold == HOLD_STILL && m_hasOverlay));
    break;

  case BD_EVENT_POPUP:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_POPUP {}", m_event.param);
    m_popupAvailable = (m_event.param != 0);
    break;

  case BD_EVENT_STEREOSCOPIC_STATUS:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_STEREOSCOPIC_STATUS {}",
                  m_event.param);
    break;

  case BD_EVENT_UO_MASK_CHANGED:
    m_uoMask = m_event.param;
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "CDVDInputStreamBluray - BD_EVENT_UO_MASK_CHANGED 0x{:x} (menu_call={} time_search={} chapter_search={})",
                  m_event.param, (m_event.param & BLURAY_UO_MENU_CALL) != 0,
                  (m_event.param & BLURAY_UO_TIME_SEARCH_MASK) != 0,
                  (m_event.param & BLURAY_UO_CHAPTER_SEARCH) != 0);
    break;

  case BD_EVENT_PLAYLIST_STOP:
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - BD_EVENT_PLAYLIST_STOP: flush buffers");
    m_titleEndWait = false;
    m_menuRestorePlaylist = (m_menu && m_hasMenuOverlay) ? m_playlist : MAX_PLAYLIST_ID + 1;
    m_menu = false;
    m_menuBurstObserved = false;
    m_menuBurstPlaylist = MAX_PLAYLIST_ID + 1;
    ReplaceTitleInfo(nullptr);
    if (m_hasMenuOverlay)
    {
      m_overlayCloseDeferred = true;
      logComponentM(LOGDEBUG, LOGBLURAY,
                    "OverlayClose deferred at playlist stop, menu overlay held | AMLPlaneState: {}",
                    aml_get_plane_state_diag());
    }
    else
      OverlayClose();
    m_player->OnDiscNavResult(nullptr, BD_EVENT_PLAYLIST_STOP);
    break;
  case BD_EVENT_NONE:
    break;

  default:
    logM(LOGWARNING, "unhandled libbluray event {} [param {}]", m_event.event, m_event.param);
    break;
  }

  // an event that ends the wait holds the old demuxer until the next stream opens
  if (titleEndWait && !m_titleEndWait && m_hold != HOLD_ERROR && m_hold != HOLD_EXIT)
    m_hold = HOLD_HELD;

  /* event has been consumed */
  m_event.event = BD_EVENT_NONE;

  if (m_bMVCPlayback)
  {
    bool queued = false;
    {
      std::lock_guard<std::mutex> lock(m_clipTableMutex);
      if (m_clip && m_titleInfo && m_clip >= m_titleInfo->clips &&
          m_clip < m_titleInfo->clips + m_titleInfo->clip_count && m_nMVCClip != m_clip &&
          (m_clipQueue.empty() || m_clip != m_titleInfo->clips + m_clipQueue.front()))
      {
        m_clipQueue.push(m_clip - m_titleInfo->clips);
        queued = true;
      }
    }

    if (queued && m_pMVCDemux == nullptr)
      OpenNextStream();
  }

  SelectPendingStreams();
}

void CDVDInputStreamBluray::DisableExtention()
{
  CloseMVCDemux();
  m_bMVCDisabled = true;
  m_bMVCPlayback = false;
}

int CDVDInputStreamBluray::Read(uint8_t* buf, int buf_size)
{
  int result = 0;
  struct InReadGuard
  {
    explicit InReadGuard(std::atomic<std::thread::id>& flag) : m_flag(flag)
    {
      m_flag.store(std::this_thread::get_id(), std::memory_order_relaxed);
    }
    ~InReadGuard() { m_flag.store(std::thread::id(), std::memory_order_relaxed); }
    std::atomic<std::thread::id>& m_flag;
  };
  const InReadGuard inReadGuard(m_readingThread);
  if (m_repostMenuOverlay.load(std::memory_order_relaxed))
  {
    m_repostMenuOverlay = false;
    if (m_menu && m_hasOverlay)
    {
      logComponentM(LOGDEBUG, LOGBLURAY,
                    "CDVDInputStreamBluray::Read - reposting retained menu overlay composition after stream reopen");
      OverlayFlush(-1);
    }
  }
  DeliverParkedOverlayIfDue();
  // the packets a held demuxer plays out keep the time of the item they came from
  const int dispTimeHeld = m_dispTimeBeforeRead;
  if (!m_navmode || m_hold != HOLD_HELD)
    m_dispTimeBeforeRead = static_cast<int>((bd_tell_time(m_bd) / 90));
  if (m_stereoUnrecoverable.load(std::memory_order_relaxed))
  {
    if (m_hold != HOLD_EXIT)
      logComponentM(LOGWARNING, LOGBLURAY,
                    "CDVDInputStreamBluray::Read - 3D MVC stereo stitching unrecoverable, ending playback");
    m_hold = HOLD_EXIT;
    return -1;
  }
  if(m_navmode)
  {
    do {

      DeliverParkedOverlayIfDue();

      if (m_hold == HOLD_HELD || m_titleEndWait)
         return 0;

      if(  m_hold == HOLD_ERROR
        || m_hold == HOLD_EXIT)
        return -1;

      if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
          CServiceBroker::GetLogging().CanLogComponent(LOGBLURAY))
      {
        const auto vmNow = std::chrono::steady_clock::now();
        if (m_vmDiagLastRead.time_since_epoch().count() != 0)
        {
          const auto gap = static_cast<uint32_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(vmNow - m_vmDiagLastRead)
                  .count());
          if (gap > m_vmDiagReadGapMaxUs)
            m_vmDiagReadGapMaxUs = gap;
        }
        m_vmDiagLastRead = vmNow;
        ++m_vmDiagReads;
        if (m_vmDiagWindow.time_since_epoch().count() == 0)
          m_vmDiagWindow = vmNow;
        else if (vmNow - m_vmDiagWindow >= std::chrono::seconds(1))
        {
          logComponentM(LOGDEBUG, LOGBLURAY,
                        "bdvm: reads={} readGapMaxMs={} argbDraws={} argbKB={} hdmvDraws={} "
                        "flushes={} lockHoldMaxUs={} planeMax={} hold={} menu={} playlist={}",
                        m_vmDiagReads, m_vmDiagReadGapMaxUs / 1000, m_vmDiagArgbDraws,
                        m_vmDiagArgbBytes / 1024, m_vmDiagHdmvDraws, m_vmDiagFlushes,
                        m_vmDiagLockHoldMaxUs, m_vmDiagPlaneMax,
                        static_cast<int>(m_hold.load()), m_menu.load(), m_playlist);
          m_vmDiagWindow = vmNow;
          m_vmDiagReads = 0;
          m_vmDiagReadGapMaxUs = 0;
          m_vmDiagArgbDraws = 0;
          m_vmDiagArgbBytes = 0;
          m_vmDiagHdmvDraws = 0;
          m_vmDiagFlushes = 0;
          m_vmDiagLockHoldMaxUs = 0;
          m_vmDiagPlaneMax = 0;
        }
      }

      result = bd_read_ext (m_bd, buf, buf_size, &m_event);
      m_readSinceReport = true;
      m_lastReadEvent = m_event.event;

      if(result < 0)
      {
        m_hold = HOLD_ERROR;
        return result;
      }
      CheckPresentedLeft();

      if (m_event.event == BD_EVENT_END_OF_TITLE && result == 0)
      {
        m_atTitleEnd = true;
        if (m_event.param != 0 || (m_title && m_title->bdj))
        {
          // a held title end stops reads until presented; an unheld BD-J end waits on the program
          m_titleEndWait = true;
          m_endOfTitleSpin = 0;
        }
        else if (++m_endOfTitleSpin == 1)
          m_endOfTitleSpinStart = std::chrono::steady_clock::now();
        else if ((m_endOfTitleSpin & END_OF_TITLE_SPIN_CLOCK_MASK) == 0)
        {
          const auto spinMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - m_endOfTitleSpinStart)
                                  .count();
          if (spinMs > END_OF_TITLE_SPIN_TIMEOUT_MS)
          {
            logM(LOGWARNING,
                 "navigation not advancing after {}ms and {} consecutive BD_EVENT_END_OF_TITLE with "
                 "no data, ending playback",
                 spinMs, m_endOfTitleSpin);
            m_hold = HOLD_EXIT;
            return -1;
          }
        }
      }
      else
      {
        m_endOfTitleSpin = 0;
      }

      const bool fromStill = (m_hold == HOLD_STILL);

      /* Check for holding events */
      switch(m_event.event) {
        case BD_EVENT_SEEK:
        {
          const bool menuJump = TakeMenuJump(false, result > 0);
          if (m_wrapSeekExempt)
          {
            m_wrapSeekExempt = false;
            logComponentM(LOGDEBUG, LOGBLURAY,
                          "BD_EVENT_SEEK consumed by same-playlist loop-wrap continuation");
            break;
          }
          if(m_hold != HOLD_DATA)
          {
            m_hold = HOLD_HELD;
            if (result == 0)
              m_dispTimeBeforeRead = dispTimeHeld;
            if (result == 0 && AbandonsItem(menuJump, fromStill))
              m_itemAbandoned = true;
            return result;
          }
          break;
        }

        case BD_EVENT_TITLE:
        {
          const bool menuJump = TakeMenuJump(false, result > 0);
          if (m_atTitleEnd && !menuJump)
            break;
          if(m_hold != HOLD_DATA)
          {
            StashBoundaryClip();
            m_hold = HOLD_HELD;
            if (result == 0)
              m_dispTimeBeforeRead = dispTimeHeld;
            if (result == 0 && AbandonsItem(menuJump, fromStill))
              m_itemAbandoned = true;
            return result;
          }
          break;
        }

        case BD_EVENT_ANGLE:
          if (m_atTitleEnd && m_event.param == m_angle)
            break;
          if(m_hold != HOLD_DATA)
          {
            m_hold = HOLD_HELD;
            if (result == 0)
              m_dispTimeBeforeRead = dispTimeHeld;
            return result;
          }
          break;

        case BD_EVENT_PLAYLIST:
        {
          const bool menuJump = TakeMenuJump(false, result > 0);
          if (m_atTitleEnd && !menuJump && m_event.param == m_playlist && m_titleInfo && m_clip)
          {
            logComponentM(LOGDEBUG, LOGBLURAY,
                          "BD_EVENT_PLAYLIST {} same-playlist loop wrap at title end, seamless continuation",
                          m_event.param);
            m_wrapSeekExempt = true;
            if (m_bMVCPlayback)
              m_stereoResyncRequested.store(true, std::memory_order_relaxed);
            ProcessEvent();
            m_event.event = BD_EVENT_NONE;
            break;
          }
          if(m_hold != HOLD_DATA)
          {
            if (m_atTitleEnd && !menuJump && m_event.param != m_playlist && m_titleInfo && m_clip &&
                m_clip->video_stream_count >= 1)
            {
              logComponentM(LOGDEBUG, LOGBLURAY,
                            "BD_EVENT_PLAYLIST {} cross-playlist candidate from playlist {}",
                            m_event.param, m_playlist);
              StashBoundaryClip();
              ProcessEvent();
              m_event.event = BD_EVENT_NONE;
              m_crossPlaylistPending = true;
              break;
            }
            StashBoundaryClip();
            m_hold = HOLD_HELD;
            if (result == 0)
              m_dispTimeBeforeRead = dispTimeHeld;
            if (result == 0 && AbandonsItem(menuJump, fromStill))
              m_itemAbandoned = true;
            return result;
          }
          break;
        }

        case BD_EVENT_PLAYITEM:
          if(m_hold != HOLD_DATA)
          {
            const bool pending = m_crossPlaylistPending;
            const bool jumpWithoutData =
                result == 0 &&
                (m_menuInput || (m_notificationsHeld && !fromStill && m_title && m_title->bdj));
            if (jumpWithoutData)
            {
              m_crossPlaylistPending = false;
              if (pending)
                FreePrevTitleInfo();
              const bool menuJump = TakeMenuJump(false, false);
              m_seamlessPlayItem = false;
              m_hold = HOLD_HELD;
              m_dispTimeBeforeRead = dispTimeHeld;
              if (AbandonsItem(menuJump, fromStill))
                m_itemAbandoned = true;
              return result;
            }
            m_crossPlaylistPending = false;
            const BLURAY_CLIP_INFO* cur = pending ? m_prevClip : m_clip;
            const char* tdReason = nullptr;
            const BLURAY_CLIP_INFO* nextClip =
              (m_titleInfo && m_event.param < m_titleInfo->clip_count)
                ? &m_titleInfo->clips[m_event.param] : nullptr;
            if (!pending && cur && nextClip == cur)
            {
              TakeMenuJump(true, result > 0);
              logComponentM(LOGDEBUG, LOGBLURAY,
                            "BD_EVENT_PLAYITEM {} same-clip re-entry, seamless continuation",
                            m_event.param);
              UpdateSeamTimeOffset(cur, nextClip);
              m_seamlessPlayItem = true;
              ProcessEvent();
              m_event.event = BD_EVENT_NONE;
              break;
            }
            if (!m_titleInfo) tdReason = "no_titleInfo";
            else if (!cur) tdReason = pending ? "xpl_no_prev_clip" : "no_current_clip";
            else if (!nextClip) tdReason = "clip_oob";
            else if (!pending && cur->audio_stream_count < 1) tdReason = "current_no_audio";
            else if (!pending && cur->video_stream_count < 1) tdReason = "current_no_video";
            else if (!pending && nextClip->audio_stream_count < 1) tdReason = "next_no_audio";
            else if (!pending && nextClip->video_stream_count < 1) tdReason = "next_no_video";
            else if (pending && !AreClipVideoStreamsCompatible(cur, nextClip))
              tdReason = "xpl_video_changed";
            else if (pending && cur->audio_stream_count != nextClip->audio_stream_count)
              tdReason = "xpl_audio_set_changed";
            else if (pending && !AreClipPgStreamsEqual(cur, nextClip)) tdReason = "xpl_pg_changed";
            else if (pending && m_prevWasMVC != m_bMVCPlayback) tdReason = "xpl_mvc_changed";
            else if (pending && m_bMVCPlayback && m_prevFlipEyes != m_bFlipEyes)
              tdReason = "xpl_mvc_eyes_changed";
            else if (!IsClipCodecCompatible(cur, nextClip)) tdReason = "codec_changed";
            if (tdReason == nullptr)
            {
              TakeMenuJump(true, result > 0);
              logComponentM(LOGDEBUG, LOGBLURAY,
                            "BD_EVENT_PLAYITEM {} {} continuation (videoPid=0x{:x} videoType=0x{:x}, audioPid=0x{:x} audioType=0x{:x})",
                            m_event.param, pending ? "cross-playlist seamless" : "seamless",
                            cur->video_streams[0].pid, cur->video_streams[0].coding_type,
                            cur->audio_stream_count ? cur->audio_streams[0].pid : 0,
                            cur->audio_stream_count ? cur->audio_streams[0].coding_type : 0);
              UpdateSeamTimeOffset(cur, nextClip);
              m_seamlessPlayItem = true;
              ProcessEvent();
              m_event.event = BD_EVENT_NONE;
              if (pending)
              {
                m_wrapSeekExempt = true;
                FreePrevTitleInfo();
              }
              break;
            }
            const bool menuJump = TakeMenuJump(false, result > 0);
            const bool contentChain =
              cur && nextClip && cur->audio_stream_count >= 1 && cur->video_stream_count >= 1 &&
              nextClip->audio_stream_count >= 1 && nextClip->video_stream_count >= 1;
            if (cur && nextClip && !m_bMVCPlayback && !(pending && m_prevWasMVC))
              m_videoCompatBoundary = AreClipVideoStreamsCompatible(cur, nextClip);
            if (pending)
              FreePrevTitleInfo();
            logComponentM(LOGDEBUG, LOGBLURAY,
                          "BD_EVENT_PLAYITEM {} teardown: {}{}", m_event.param, tdReason,
                          m_videoCompatBoundary ? " (video-compatible boundary)" : "");
            m_seamlessPlayItem = false;
            m_naturalChainBoundary =
              contentChain && !m_bMVCPlayback && !(pending && m_prevWasMVC);
            m_hold = HOLD_HELD;
            if (result == 0)
              m_dispTimeBeforeRead = dispTimeHeld;
            if (result == 0 && AbandonsItem(menuJump, fromStill))
              m_itemAbandoned = true;
            return result;
          }
          break;

        case BD_EVENT_STILL_TIME:
          // an infinite still starts here, so later disc events reach the player through the read
          if(m_hold == HOLD_STILL)
            m_event.event = 0; /* Consume duplicate still event */
          else if (m_event.param == 0)
            ProcessEvent();
          else
            m_hold = HOLD_HELD;
          return result;

        case BD_EVENT_END_OF_TITLE:
          if (m_event.param != 0 || (m_title && m_title->bdj))
            m_titleEndWait = true;
          break;

        default:
          TakeMenuJump(false, result > 0);
          break;
      }

      if(result > 0)
      {
        m_hold = HOLD_NONE;
        m_atTitleEnd = false;
        m_wrapSeekExempt = false;
        m_videoCompatBoundary = false;
        m_naturalChainBoundary = false;
        if (m_crossPlaylistPending)
        {
          logM(LOGWARNING,
               "CDVDInputStreamBluray::Read - data before BD_EVENT_PLAYITEM resolved a "
               "cross-playlist candidate, dropping it");
          m_crossPlaylistPending = false;
          FreePrevTitleInfo();
        }
      }

      ProcessEvent();

    } while(result == 0);

  }
  else
  {
    result = bd_read(m_bd, buf, buf_size);
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }

  return result;
}



static uint8_t  clamp(double v)
{
  return (v) > 255.0 ? 255 : ((v) < 0.0 ? 0 : static_cast<uint32_t>((v + 0.5)));
}

static uint32_t build_rgba(const BD_PG_PALETTE_ENTRY &e)
{
  double r = 1.164 * (e.Y - 16)                        + 1.596 * (e.Cr - 128);
  double g = 1.164 * (e.Y - 16) - 0.391 * (e.Cb - 128) - 0.813 * (e.Cr - 128);
  double b = 1.164 * (e.Y - 16) + 2.018 * (e.Cb - 128);
  return static_cast<uint32_t>(e.T)      << PIXEL_ASHIFT
       | static_cast<uint32_t>(clamp(r)) << PIXEL_RSHIFT
       | static_cast<uint32_t>(clamp(g)) << PIXEL_GSHIFT
       | static_cast<uint32_t>(clamp(b)) << PIXEL_BSHIFT;
}

static bool HasVisibleArgbPixels(const uint32_t* pixels, int stridePixels, int w, int h)
{
  if (!pixels || w <= 0 || h <= 0 || stridePixels < w)
    return false;

  for (int y = 0; y < h; ++y)
  {
    const uint32_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stridePixels);
    uint32_t alphaOr = 0;
    for (int x = 0; x < w; ++x)
      alphaOr |= row[x];
    if ((alphaOr & 0xFF000000u) != 0)
      return true;
  }
  return false;
}

void CDVDInputStreamBluray::OverlayClose(bool deferrable)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  std::lock_guard lock(m_overlayLock);
  if (deferrable && m_atTitleEnd && m_hasMenuOverlay && IsInMenu())
  {
    m_overlayCloseDeferred = true;
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "OverlayClose deferred at title end, awaiting menu redraw | AMLPlaneState: {}",
                  aml_get_plane_state_diag());
    return;
  }
  m_overlayCloseDeferred = false;
  m_pendingOverlayGroup.reset();
  ResetBdjSurfaceLocked();
  logComponentM(LOGDEBUG, LOGBLURAY, "OverlayClose | AMLPlaneState: {}", aml_get_plane_state_diag());
  for(SPlane& plane : m_planes)
    plane.o.clear();
  auto group = std::make_shared<CDVDOverlayGroup>();
  group->bForced = true;
  group->SetDiscMenuOverlay(true);
  m_player->OnDiscNavResult(static_cast<void*>(&group), BD_EVENT_MENU_OVERLAY);
  m_hasOverlay = false;
  m_hasMenuOverlay = false;
  m_hasVisibleBdjOverlay = false;
  m_bdjPopup = BdjPopup::CLOSED;
  m_bdjGraphicsActive = false;
  aml_set_bdj_overlay_active(false);
#endif
}

void CDVDInputStreamBluray::OverlayInit(SPlane& plane, int w, int h)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  plane.o.clear();
  plane.w = w;
  plane.h = h;
#endif
}

void CDVDInputStreamBluray::ResetBdjSurfaceLocked()
{
  m_bdjOverlay.reset();
  m_bdjVisiblePixels = 0;
  m_bdjSurfaceDirty = false;
}

void CDVDInputStreamBluray::InitBdjSurfaceLocked(int w, int h)
{
  ResetBdjSurfaceLocked();
  if (w <= 0 || h <= 0)
    return;

  const bool isHdrPq = m_pqAuthoredGraphics.load(std::memory_order_relaxed);
  const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
  auto surface = std::make_shared<CDVDOverlayImage::PersistentSurface>();
  surface->width = w;
  surface->height = h;
  surface->pixelsRaw.assign(pixelCount, 0u);
  surface->pixelsPma.assign(pixelCount, 0u);
  surface->isHdrPq.store(isHdrPq, std::memory_order_relaxed);

  auto overlay = std::make_shared<CDVDOverlayImage>();
  overlay->palette.clear();
  overlay->linesize = w * 4;
  overlay->x = 0;
  overlay->y = 0;
  overlay->width = w;
  overlay->height = h;
  overlay->source_width = w;
  overlay->source_height = h;
  overlay->m_isHdrPq = isHdrPq;
  overlay->SetDiscMenuOverlay(true);
  overlay->m_persistentSurface = std::move(surface);

  m_bdjOverlay = std::move(overlay);
}

void CDVDInputStreamBluray::OverlayClear(SPlane& plane, int x, int y, int w, int h)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  CRectInt ovr(x
          , y
          , x + w
          , y + h);

  /* fixup existing overlays */
  for(auto it = plane.o.begin(); it != plane.o.end();)
  {
    CRectInt old((*it)->x
            , (*it)->y
            , (*it)->x + (*it)->width
            , (*it)->y + (*it)->height);

    std::vector<CRectInt> rem = old.SubtractRect(ovr);

    /* if no overlap we are done */
    if(rem.size() == 1 && !(rem[0] != old))
    {
      ++it;
      continue;
    }

    SOverlays add;
    const bool isArgb = (*it)->palette.empty();
    for(auto itr = rem.begin(); itr != rem.end(); ++itr)
    {
      if (isArgb &&
          !HasVisibleArgbPixels(
              reinterpret_cast<const uint32_t*>((*it)->data_at(itr->x1, itr->y1)),
              (*it)->linesize / 4, itr->Width(), itr->Height()))
        continue;
      auto overlay =
          std::make_shared<CDVDOverlayImage>(*(*it), itr->x1, itr->y1, itr->Width(), itr->Height());
      add.push_back(overlay);
    }

    it = plane.o.erase(it);
    plane.o.insert(it, add.begin(), add.end());
  }
#endif
}

void CDVDInputStreamBluray::DeliverParkedOverlayIfDue()
{
  const bool eventsDrained =
      m_lastReadEvent == BD_EVENT_NONE || m_lastReadEvent == BD_EVENT_IDLE;
  if ((IsNaturalChainBoundaryInFlight() || !eventsDrained) && m_hold != HOLD_STILL)
    return;

  std::lock_guard lock(m_overlayLock);
  if (!m_pendingOverlayGroup)
    return;

  std::shared_ptr<CDVDOverlay> pending;
  pending.swap(m_pendingOverlayGroup);
  logComponentM(LOGDEBUG, LOGBLURAY,
                "CDVDInputStreamBluray::Read - delivering parked menu overlay composition");
  m_player->OnDiscNavResult(static_cast<void*>(&pending), BD_EVENT_MENU_OVERLAY);
}

void CDVDInputStreamBluray::OverlayFlush(int64_t pts)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  const bool vmDiag = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                      CServiceBroker::GetLogging().CanLogComponent(LOGBLURAY);
  std::chrono::steady_clock::time_point lockEnter;
  if (vmDiag)
    lockEnter = std::chrono::steady_clock::now();
  std::lock_guard lock(m_overlayLock);
  if (vmDiag)
  {
    const auto held = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                              lockEnter)
            .count());
    if (held > m_vmDiagLockHoldMaxUs)
      m_vmDiagLockHoldMaxUs = held;
    ++m_vmDiagFlushes;
  }
  auto group = std::make_shared<CDVDOverlayGroup>();
  group->bForced       = true;
  group->iPTSStartTime = static_cast<double>(pts);
  group->iPTSStopTime  = 0;
  group->SetDiscMenuOverlay(true);
  group->SetOverlayContainerFlushable(false);

  size_t subOverlayCount = 0;
  size_t menuOverlayCount = 0;
  bool hasVisibleBdjOverlay = false;
  for (size_t i = 0; i < sizeof(m_planes) / sizeof(m_planes[0]); ++i)
  {
    SPlane& plane = m_planes[i];
    for(auto it = plane.o.begin(); it != plane.o.end(); ++it)
    {
      (*it)->SetDiscMenuOverlay(true);
      group->m_overlays.push_back(*it);
      if (i == BD_OVERLAY_IG && (*it)->palette.empty())
        hasVisibleBdjOverlay = true;
    }
    subOverlayCount += plane.o.size();
    if (i == BD_OVERLAY_IG)
      menuOverlayCount = plane.o.size();
  }
  if (m_bdjOverlay && m_bdjOverlay->m_persistentSurface)
  {
    auto& surface = *m_bdjOverlay->m_persistentSurface;
    const bool isHdrPq = m_pqAuthoredGraphics.load(std::memory_order_relaxed);
    surface.isHdrPq.store(isHdrPq, std::memory_order_relaxed);
    if (m_bdjSurfaceDirty)
    {
      std::lock_guard<std::mutex> surfLock(surface.mutex);
      surface.revision.fetch_add(1, std::memory_order_release);
      m_bdjSurfaceDirty = false;
    }
    if (m_bdjVisiblePixels > 0)
    {
      m_bdjOverlay->SetDiscMenuOverlay(true);
      group->m_overlays.push_back(m_bdjOverlay);
      ++subOverlayCount;
      ++menuOverlayCount;
      hasVisibleBdjOverlay = true;
    }
  }
  m_hasMenuOverlay = menuOverlayCount > 0;
  m_hasVisibleBdjOverlay = hasVisibleBdjOverlay;
  UpdateBdjPopup(hasVisibleBdjOverlay);
#ifdef HAVE_LIBBLURAY_BDJ_VIDEO_ACTIVE
  group->m_waitForStartupVideo = hasVisibleBdjOverlay && bd_bdj_video_active(m_bd);
#endif
  m_hasOverlay = subOverlayCount > 0;
  if (menuOverlayCount > 0)
    m_overlayCloseDeferred = false;

  LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGBLURAY, 1000,
                "OverlayFlush pts={} isInMenu={} m_menu={} m_hold={} m_hasOverlay={} "
                "bdjVisible={} m_menuBurstObserved={} sub_count={} menu_count={} | AMLPlaneState: {}",
                pts, IsInMenu(), m_menu.load(), static_cast<int>(m_hold.load()), m_hasOverlay.load(),
                m_hasVisibleBdjOverlay.load(), m_menuBurstObserved.load(), subOverlayCount,
                menuOverlayCount, aml_get_plane_state_diag());

  // With CDVDMenuPlane the menu page is stored independently of the video/subtitle pipeline,
  // so deliver immediately without parking on demux read boundaries.
  m_pendingOverlayGroup.reset();
  m_player->OnDiscNavResult(static_cast<void*>(&group), BD_EVENT_MENU_OVERLAY);
  const auto now = std::chrono::steady_clock::now();
  m_recentFlushes[m_flushIdx] = now;
  m_flushIdx = (m_flushIdx + 1) % FLUSH_WINDOW;
  if (!m_menuBurstObserved)
  {
    bool allRecent = true;
    for (const auto& t : m_recentFlushes)
    {
      if (now - t > std::chrono::milliseconds(500))
      {
        allRecent = false;
        break;
      }
    }
    if (allRecent)
    {
      m_menuBurstObserved = true;
      m_menuBurstPlaylist = m_playlist;
    }
  }
  const bool menuVisuallyActive =
      (!m_isBdjTitle.load(std::memory_order_relaxed) && m_menu) || m_hasVisibleBdjOverlay ||
      (m_hold == HOLD_STILL && m_hasOverlay);
  aml_set_bdj_overlay_active(menuVisuallyActive);
#endif
}

void CDVDInputStreamBluray::OverlayCallback(const BD_OVERLAY * const ov)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  std::lock_guard lock(m_overlayLock);
  if(ov == nullptr || ov->cmd == BD_OVERLAY_CLOSE)
  {
    OverlayClose(ov != nullptr && ov->plane == BD_OVERLAY_IG);
    return;
  }

  if (ov->plane > 1)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray - Ignoring overlay with multiple planes");
    return;
  }

  SPlane& plane(m_planes[ov->plane]);

  if (ov->cmd == BD_OVERLAY_CLEAR)
  {
    plane.o.clear();
    if (ov->plane == BD_OVERLAY_IG)
      ResetBdjSurfaceLocked();
    return;
  }

  if (ov->cmd == BD_OVERLAY_INIT)
  {
    OverlayInit(plane, ov->w, ov->h);
    if (ov->plane == BD_OVERLAY_IG)
      ResetBdjSurfaceLocked();
    return;
  }

  if (ov->cmd == BD_OVERLAY_HIDE)
  {
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray - overlay HIDE plane {}", ov->plane);
    plane.o.clear();
    if (ov->plane == BD_OVERLAY_IG)
      ResetBdjSurfaceLocked();
    OverlayFlush(ov->pts);
    return;
  }

  if (ov->cmd == BD_OVERLAY_DRAW && ov->palette_update_flag)
  {
    if (ov->palette)
    {
      std::vector<uint32_t> pal(256);
      for (unsigned i = 0; i < 256; i++)
        pal[i] = build_rgba(ov->palette[i]);
      for (SOverlay& o : plane.o)
      {
        if (o->palette.empty())
          continue;
        SOverlay copy =
            std::make_shared<CDVDOverlayImage>(*o, o->x, o->y, o->width, o->height);
        copy->palette = pal;
        o = copy;
      }
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGBLURAY, 1000,
                    "CDVDInputStreamBluray - palette-only update plane {} ({} overlays)",
                    ov->plane, plane.o.size());
    }
    return;
  }

  if (ov->cmd == BD_OVERLAY_DRAW &&
      (ov->w == 0 || ov->h == 0 || ov->x >= plane.w || ov->y >= plane.h))
  {
    CLog::Log(
        LOGWARNING,
        "CDVDInputStreamBluray - invalid overlay rectangle {}x{} at {},{} on plane {} ({}x{})",
        ov->w, ov->h, ov->x, ov->y, ov->plane, plane.w, plane.h);
    return;
  }

  if (ov->cmd == BD_OVERLAY_DRAW || ov->cmd == BD_OVERLAY_WIPE)
    OverlayClear(plane, ov->x, ov->y, ov->w, ov->h);

  /* uncompress and draw bitmap */
  if (ov->img && ov->cmd == BD_OVERLAY_DRAW)
  {
    auto overlay = std::make_shared<CDVDOverlayImage>();

    if (ov->palette)
    {
      overlay->palette.resize(256);

      for(unsigned i = 0; i < 256; i++)
        overlay->palette[i] = build_rgba(ov->palette[i]);
    }
    else
      overlay->palette.clear();

    const BD_PG_RLE_ELEM *rlep = ov->img;
    const size_t bytes = static_cast<size_t>(ov->w) * ov->h;
    overlay->pixels.resize(bytes);

    for (size_t i = 0; i < bytes; i += rlep->len, rlep++)
    {
      if (rlep->len > bytes - i)
      {
        CLog::Log(LOGWARNING, "CDVDInputStreamBluray - invalid overlay RLE run");
        return;
      }
      memset(overlay->pixels.data() + i, rlep->color, rlep->len);
    }

    overlay->linesize = ov->w;
    overlay->x = ov->x;
    overlay->y = ov->y;
    overlay->height = ov->h;
    overlay->width = ov->w;
    overlay->source_height = plane.h;
    overlay->source_width = plane.w;
    if (ov->w > plane.w - ov->x || ov->h > plane.h - ov->y)
      overlay = std::make_shared<CDVDOverlayImage>(*overlay, ov->x, ov->y,
                                                   std::min<int>(ov->w, plane.w - ov->x),
                                                   std::min<int>(ov->h, plane.h - ov->y));
    overlay->m_isHdrPq = m_pqAuthoredGraphics.load(std::memory_order_relaxed);
    plane.o.push_back(overlay);

    if (CServiceBroker::GetLogging().CanLogComponent(LOGBLURAY))
    {
      ++m_vmDiagHdmvDraws;
      if (plane.o.size() > m_vmDiagPlaneMax)
        m_vmDiagPlaneMax = static_cast<uint32_t>(plane.o.size());
    }
  }

  if (ov->cmd == BD_OVERLAY_FLUSH)
    OverlayFlush(ov->pts);
#endif
}

#ifdef HAVE_LIBBLURAY_BDJ
void CDVDInputStreamBluray::OverlayCallbackARGB(const struct bd_argb_overlay_s * const ov)
{
  std::lock_guard lock(m_overlayLock);
  if(ov == nullptr || ov->cmd == BD_ARGB_OVERLAY_CLOSE)
  {
    OverlayClose();
    return;
  }

  if (ov->plane > 1)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray - Ignoring overlay with multiple planes");
    return;
  }

  SPlane& plane(m_planes[ov->plane]);

  if (ov->cmd == BD_ARGB_OVERLAY_INIT)
  {
    OverlayInit(plane, ov->w, ov->h);
    if (ov->plane == BD_OVERLAY_IG)
      InitBdjSurfaceLocked(ov->w, ov->h);
    return;
  }

  if (ov->plane == BD_OVERLAY_IG)
  {
    if (ov->argb && ov->cmd == BD_ARGB_OVERLAY_DRAW)
    {
      if (!m_bdjOverlay || !m_bdjOverlay->m_persistentSurface)
      {
        const int surfW = plane.w > 0 ? plane.w : static_cast<int>(ov->x + ov->w);
        const int surfH = plane.h > 0 ? plane.h : static_cast<int>(ov->y + ov->h);
        if (surfW > 0 && surfH > 0)
          InitBdjSurfaceLocked(surfW, surfH);
      }

      if (m_bdjOverlay && m_bdjOverlay->m_persistentSurface)
      {
        auto& surface = *m_bdjOverlay->m_persistentSurface;
        const int surfW = surface.width;
        const int surfH = surface.height;
        const bool withinPlane = ov->x < surfW && ov->y < surfH;
        const int drawW = withinPlane ? std::min<int>(ov->w, surfW - ov->x) : 0;
        const int drawH = withinPlane ? std::min<int>(ov->h, surfH - ov->y) : 0;
        if (drawW > 0 && drawH > 0 && ov->stride >= drawW)
        {
          int64_t visDelta = 0;

          {
            std::lock_guard<std::mutex> surfLock(surface.mutex);
            const bool bakeCoherent =
                surface.desiredKey.mode != BDJ_COLOR::BakeMode::NONE &&
                surface.bakedKey == surface.desiredKey;
            const BDJ_COLOR::BakeKey activeKey =
                bakeCoherent ? surface.desiredKey
                             : BDJ_COLOR::BakeKey{BDJ_COLOR::BakeMode::PASSTHROUGH_SDR};
            if (!bakeCoherent)
              surface.bakedKey.mode = BDJ_COLOR::BakeMode::NONE;
            const BDJ_COLOR::PreparedContext bakeCtx(activeKey);

            const auto bakeRes = BDJ_COLOR::BakeRectDiff(
                ov->argb, static_cast<int>(ov->stride), surface.pixelsRaw.data(),
                surface.pixelsPma.data(), surfW, surfH, static_cast<int>(ov->x),
                static_cast<int>(ov->y), drawW, drawH, bakeCtx, &surface.bakePool);
            visDelta = bakeRes.visDelta;

            if (bakeRes.changedX1 >= bakeRes.changedX0 && bakeRes.changedY1 >= bakeRes.changedY0)
            {
              if (surface.dirtyX1 < surface.dirtyX0)
              {
                surface.dirtyX0 = bakeRes.changedX0;
                surface.dirtyY0 = bakeRes.changedY0;
                surface.dirtyX1 = bakeRes.changedX1;
                surface.dirtyY1 = bakeRes.changedY1;
              }
              else
              {
                surface.dirtyX0 = std::min(surface.dirtyX0, bakeRes.changedX0);
                surface.dirtyY0 = std::min(surface.dirtyY0, bakeRes.changedY0);
                surface.dirtyX1 = std::max(surface.dirtyX1, bakeRes.changedX1);
                surface.dirtyY1 = std::max(surface.dirtyY1, bakeRes.changedY1);
              }
              m_bdjSurfaceDirty = true;
            }
          }

          const int64_t nextVis = static_cast<int64_t>(m_bdjVisiblePixels) + visDelta;
          m_bdjVisiblePixels = nextVis > 0 ? static_cast<uint64_t>(nextVis) : 0u;

          if (CServiceBroker::GetLogging().CanLogComponent(LOGBLURAY))
          {
            ++m_vmDiagArgbDraws;
            m_vmDiagArgbBytes += static_cast<uint64_t>(drawW) * static_cast<uint64_t>(drawH) * 4u;
            if (m_bdjVisiblePixels > 0 && m_vmDiagPlaneMax < 1)
              m_vmDiagPlaneMax = 1;
          }
        }
      }
    }

    if (ov->cmd == BD_ARGB_OVERLAY_FLUSH)
      OverlayFlush(ov->pts);
    return;
  }

  if (ov->cmd == BD_ARGB_OVERLAY_DRAW)
    OverlayClear(plane, ov->x, ov->y, ov->w, ov->h);

  /* uncompress and draw bitmap */
  if (ov->argb && ov->cmd == BD_ARGB_OVERLAY_DRAW)
  {
    const bool withinPlane =
        (plane.w <= 0 || ov->x < plane.w) && (plane.h <= 0 || ov->y < plane.h);
    const int drawW = withinPlane
                          ? (plane.w > 0 ? std::min<int>(ov->w, plane.w - ov->x)
                                         : static_cast<int>(ov->w))
                          : 0;
    const int drawH = withinPlane
                          ? (plane.h > 0 ? std::min<int>(ov->h, plane.h - ov->y)
                                         : static_cast<int>(ov->h))
                          : 0;
    if (drawW > 0 && drawH > 0 && ov->stride >= drawW &&
        HasVisibleArgbPixels(ov->argb, ov->stride, drawW, drawH))
    {
      auto overlay = std::make_shared<CDVDOverlayImage>();

      overlay->palette.clear();
      const size_t rowBytes = static_cast<size_t>(drawW) * 4;
      const size_t bytes = rowBytes * static_cast<size_t>(drawH);
      overlay->pixels.resize(bytes);
      for (int row = 0; row < drawH; ++row)
      {
        memcpy(overlay->pixels.data() + static_cast<size_t>(row) * rowBytes,
               ov->argb + static_cast<size_t>(row) * ov->stride, rowBytes);
      }

      overlay->linesize = static_cast<int>(rowBytes);
      overlay->x = ov->x;
      overlay->y = ov->y;
      overlay->height = drawH;
      overlay->width = drawW;
      overlay->source_height = plane.h;
      overlay->source_width = plane.w;
      overlay->m_isHdrPq = m_pqAuthoredGraphics.load(std::memory_order_relaxed);
      plane.o.push_back(overlay);

      if (CServiceBroker::GetLogging().CanLogComponent(LOGBLURAY))
      {
        ++m_vmDiagArgbDraws;
        m_vmDiagArgbBytes += bytes;
        if (plane.o.size() > m_vmDiagPlaneMax)
          m_vmDiagPlaneMax = static_cast<uint32_t>(plane.o.size());
      }
    }
  }

  if(ov->cmd == BD_ARGB_OVERLAY_FLUSH)
    OverlayFlush(ov->pts);
}
#endif


int CDVDInputStreamBluray::GetTotalTime()
{
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  if(m_titleInfo)
    return static_cast<int>(m_titleInfo->duration / 90);
  else
    return m_endedTitleTime;
}

int CDVDInputStreamBluray::GetTime()
{
  return m_dispTimeBeforeRead;
}

bool CDVDInputStreamBluray::PosTime(int ms)
{
  if (m_navmode && (m_uoMask.load() & BLURAY_UO_TIME_SEARCH_MASK))
  {
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray::PosTime - time search masked by disc UO");
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Blu-ray",
                                          g_localizeStrings.Get(60655));
    return false;
  }

  const bool joinHeld = IsJoinHeld();
  // A seek preview arriving after an unanswered popup request is not a popup menu.
  BdjPopup opening = BdjPopup::OPENING;
  m_bdjPopup.compare_exchange_strong(opening, BdjPopup::CLOSED);
  m_seamlessPlayItem = false;
  if(bd_seek_time(m_bd, ms * 90) < 0)
    return false;

  ResetTitleEnd();
  if (joinHeld)
  {
    m_event.event = BD_EVENT_NONE;
    m_hold = HOLD_NONE;
  }
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    EMPTY_QUEUE(m_clipQueue);
  }
  // a held still belongs to the position the seek left
  if (m_event.event != BD_EVENT_NONE && m_event.event != BD_EVENT_STILL_TIME)
    ProcessEvent();
  const bool heldJump = ProcessHeldEvents();
  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();
  if (heldJump && (m_hold == HOLD_NONE || m_hold == HOLD_DATA))
    m_hold = HOLD_HELD;
  // the seek flushes the item a jump left
  m_itemAbandoned = false;
  // the player's own seek repositions an external subtitle
  m_itemSeeked = false;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (m_clip)
      RecordTitleTimeSpan(m_clip, true);
  }

  if (m_bMVCPlayback)
  {
    OpenNextStream();
    SeekMVCDemux(ms - m_clipStartTime);
  }
  return true;
}

int CDVDInputStreamBluray::GetChapterCount()
{
  if(m_titleInfo)
    return static_cast<int>(m_titleInfo->chapter_count);
  else
    return 0;
}

int CDVDInputStreamBluray::GetChapter()
{
  if(m_titleInfo)
    return static_cast<int>(bd_get_current_chapter(m_bd) + 1);
  else
    return 0;
}

void CDVDInputStreamBluray::GetChapterName(std::string& name, int ch)
{
  if (ch == -1 || ch > GetChapterCount())
    ch = GetChapter();

  if (m_titleInfo && ch > 0 && static_cast<uint32_t>(ch) <= m_titleInfo->chapter_count &&
      m_titleInfo->chapters[ch - 1].chapter_name)
    name = m_titleInfo->chapters[ch - 1].chapter_name;
}

bool CDVDInputStreamBluray::SeekChapter(int ch)
{
  const bool joinHeld = IsJoinHeld();
  m_seamlessPlayItem = false;
  if(m_titleInfo && bd_seek_chapter(m_bd, ch-1) < 0)
    return false;

  ResetTitleEnd();
  if (joinHeld)
  {
    m_event.event = BD_EVENT_NONE;
    m_hold = HOLD_NONE;
  }
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    EMPTY_QUEUE(m_clipQueue);
  }
  if (m_event.event != BD_EVENT_NONE && m_event.event != BD_EVENT_STILL_TIME)
    ProcessEvent();
  const bool heldJump = ProcessHeldEvents();
  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();
  if (heldJump && (m_hold == HOLD_NONE || m_hold == HOLD_DATA))
    m_hold = HOLD_HELD;
  m_itemAbandoned = false;
  m_itemSeeked = false;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (m_clip)
      RecordTitleTimeSpan(m_clip, true);
  }

  if (m_bMVCPlayback)
  {
    OpenNextStream();
    SeekMVCDemux(GetChapterPos(ch) * 1000 - m_clipStartTime);
  }
  return true;
}

int64_t CDVDInputStreamBluray::GetChapterPos(int ch)
{
  if (ch == -1 || ch > GetChapterCount())
    ch = GetChapter();

  if (m_titleInfo && m_titleInfo->chapters)
    return m_titleInfo->chapters[ch - 1].start / 90000;
  else
    return 0;
}

int64_t CDVDInputStreamBluray::Seek(int64_t offset, int whence)
{
#if LIBBLURAY_BYTESEEK
  if(whence == SEEK_POSSIBLE)
    return 1;
  else if(whence == SEEK_CUR)
  {
    if(offset == 0)
      return bd_tell(m_bd);
    else
      offset += bd_tell(m_bd);
  }
  else if(whence == SEEK_END)
    offset += bd_get_title_size(m_bd);
  else if(whence != SEEK_SET)
    return -1;

  int64_t pos = bd_seek(m_bd, offset);
  if(pos < 0)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Seek - seek to {}, failed with {}", offset, pos);
    return -1;
  }

  if(pos != offset)
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray::Seek - seek to {}, ended at {}", offset, pos);

  return offset;
#else
  if(whence == SEEK_POSSIBLE)
    return 0;
  return -1;
#endif
}

int64_t CDVDInputStreamBluray::GetLength()
{
  return static_cast<int64_t>(bd_get_title_size(m_bd));
}

static bool find_stream(int pid, BLURAY_STREAM_INFO *info, int count, std::string &language)
{
  int i=0;
  for(;i<count;i++,info++)
  {
    if(info->pid == static_cast<uint16_t>(pid))
      break;
  }
  if(i==count)
    return false;
  language = reinterpret_cast<char*>(info->lang);
  return true;
}

void CDVDInputStreamBluray::GetStreamInfo(int pid, std::string &language) const {
  if(!m_titleInfo || !m_clip)
    return;

  if (pid == HDMV_PID_VIDEO || pid == HDMV_PID_VIDEO_EL)
    find_stream(pid, m_clip->video_streams, m_clip->video_stream_count, language);
  else if (HDMV_PID_AUDIO_FIRST <= pid && pid <= HDMV_PID_AUDIO_LAST)
    find_stream(pid, m_clip->audio_streams, m_clip->audio_stream_count, language);
  else if (HDMV_PID_PG_FIRST <= pid && pid <= HDMV_PID_PG_LAST)
    find_stream(pid, m_clip->pg_streams, m_clip->pg_stream_count, language);
  else if (HDMV_PID_PG_HDR_FIRST <= pid && pid <= HDMV_PID_PG_HDR_LAST)
    find_stream(pid, m_clip->pg_streams, m_clip->pg_stream_count, language);
  else if (HDMV_PID_IG_FIRST <= pid && pid <= HDMV_PID_IG_LAST)
    find_stream(pid, m_clip->ig_streams, m_clip->ig_stream_count, language);
  else
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::GetStreamInfo - unhandled pid {}", pid);
}

void CDVDInputStreamBluray::GetDiscStreamHdrMetadata(int pid,
                                                     bool& isDolbyVision,
                                                     bool& isHdrPlus) const
{
  isDolbyVision = false;
  isHdrPlus = false;

  const BLURAY_CLIP_INFO* clip = m_clip;
  if (!clip && m_titleInfo && m_titleInfo->clip_count > 0)
    clip = &m_titleInfo->clips[0];
  if (!clip)
    return;

  for (uint8_t i = 0; i < clip->dv_stream_count; ++i)
  {
    if (clip->dv_streams[i].pid == pid)
    {
      isDolbyVision = true;
      return;
    }
  }

  for (uint8_t i = 0; i < clip->video_stream_count; ++i)
  {
    const BLURAY_STREAM_INFO& stream = clip->video_streams[i];
    if (stream.pid != pid)
      continue;
    if (stream.dynamic_range_type == BLURAY_DYNAMIC_RANGE_DOLBY_VISION)
      isDolbyVision = true;
    if (stream.hdr_plus_flag)
      isHdrPlus = true;
    return;
  }
}

void CDVDInputStreamBluray::UpdateGraphicsRegime()
{
  bool pq = false;

  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (m_titleInfo && m_titleInfo->clip_count > 0)
    {
      const BLURAY_CLIP_INFO& clip = m_titleInfo->clips[0];
      for (uint8_t i = 0; i < clip.video_stream_count; ++i)
      {
        const uint8_t range = clip.video_streams[i].dynamic_range_type;
        if (range == BLURAY_DYNAMIC_RANGE_HDR10 || range == BLURAY_DYNAMIC_RANGE_DOLBY_VISION)
        {
          pq = true;
          break;
        }
      }
    }
  }

  if (pq != m_pqAuthoredGraphics.load(std::memory_order_relaxed))
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "CDVDInputStreamBluray - playlist graphics regime: {}",
                  pq ? "BT.2020 PQ" : "SDR");

  m_pqAuthoredGraphics.store(pq, std::memory_order_relaxed);
}

void CDVDInputStreamBluray::EnableStream(uint32_t blurayStreamType, int pid, bool enable)
{
  if (!m_bd || !m_clip)
    return;
  const BLURAY_STREAM_INFO* streams = nullptr;
  uint8_t count = 0;
  if (blurayStreamType == BLURAY_AUDIO_STREAM)
  {
    streams = m_clip->audio_streams;
    count = m_clip->audio_stream_count;
  }
  else if (blurayStreamType == BLURAY_PG_TEXTST_STREAM)
  {
    streams = m_clip->pg_streams;
    count = m_clip->pg_stream_count;
  }
  else
    return;
  for (uint8_t i = 0; i < count; ++i)
  {
    if (streams[i].pid == pid)
    {
      logComponentM(LOGDEBUG, LOGBLURAY,
                    "CDVDInputStreamBluray::EnableStream type={} pid=0x{:x} idx={} enable={}",
                    blurayStreamType, pid, i + 1, enable ? 1 : 0);
      bd_select_stream(m_bd, blurayStreamType, i + 1, enable ? 1 : 0);
      return;
    }
  }
}

CDVDInputStream::ENextStream CDVDInputStreamBluray::NextStream()
{
  if (!m_navmode || m_hold == HOLD_EXIT || m_hold == HOLD_ERROR)
    return NEXTSTREAM_NONE;

  // the title the disc program ends on keeps its time until the next stream opens
  const bool titleEndWait = m_titleEndWait;
  if (!titleEndWait)
    m_endedTitleTime = 0;

  bool clipKnown = false;
  bool programJump = false;
  bool programStop = false;
  const auto processEvent = [this, &clipKnown, &programJump, &programStop]
  {
    // Once the disc provides a new stream position, release the previous play item's still.
    if (m_hold == HOLD_STILL &&
        (m_event.event == BD_EVENT_PLAYLIST || m_event.event == BD_EVENT_PLAYITEM ||
         m_event.event == BD_EVENT_SEEK || m_event.event == BD_EVENT_DISCONTINUITY))
    {
      m_hold = HOLD_HELD;
      if (!m_bdStillActive)
      {
        int still = 0;
        m_player->OnDiscNavResult(&still, BD_EVENT_STILL);
      }
    }

    switch (m_event.event)
    {
      case BD_EVENT_PLAYITEM:
        clipKnown = true;
        break;
      case BD_EVENT_TITLE:
      case BD_EVENT_PLAYLIST:
        programJump = true;
        [[fallthrough]];
      case BD_EVENT_ANGLE:
      case BD_EVENT_END_OF_TITLE:
        clipKnown = false;
        break;
      case BD_EVENT_PLAYLIST_STOP:
        programStop = programJump;
        break;
      default:
        break;
    }
    CheckPresentedLeft();
    ProcessEvent();
  };

  /* process any current event */
  processEvent();
  while (TakeHeldEvent())
    processEvent();

  // the old item has entered the decoders, so the disc program may learn the playlist ended
  // once its last picture is presented; after that the wait only takes events
  if (m_titleEndWait)
  {
    while (bd_get_event(m_bd, &m_event))
      processEvent();
    if (IsTitleEndHeld())
    {
      if (m_lastPictureTime >= 0 && (m_presentedTime >= 0 || m_clockTime >= 0) &&
          m_presentedTime < m_lastPictureTime && m_clockTime < m_lastPictureTime)
        return NEXTSTREAM_RETRY;

      ReleaseTitle();

      // libbluray delivers the BD-J end or resumes the HDMV program, which runs in the polls
      ReportPresented(BD_PRESENTED_ALL);
      while (true)
      {
        if (bd_read_ext(m_bd, nullptr, 0, &m_event) < 0)
        {
          m_hold = HOLD_ERROR;
          return NEXTSTREAM_NONE;
        }
        if (m_event.event == BD_EVENT_NONE)
          break;
        processEvent();
        if (m_hold == HOLD_ERROR || m_hold == HOLD_EXIT)
          return NEXTSTREAM_NONE;
      }
      // an HDMV program has run to its next suspension
      if (m_title && !m_title->bdj)
        m_titleEndWait = false;
    }
  }

  /* process all queued up events */
  while (bd_get_event(m_bd, &m_event))
    processEvent();

  if (m_hold == HOLD_ERROR || m_hold == HOLD_EXIT)
    return NEXTSTREAM_NONE;

  // an HDMV program runs only in a read and stops at its first queued event; run it to its next
  // suspension so the play item of the playlist it starts is known before the old stream closes.
  // A still also needs the VM to handle menu actions even when there is no demuxer reading.
  if (m_title && !m_title->bdj && !m_titleEndWait &&
      ((programJump && !clipKnown) || m_hold == HOLD_STILL))
  {
    while (m_title && !m_title->bdj)
    {
      if (bd_read_ext(m_bd, nullptr, 0, &m_event) < 0)
      {
        m_hold = HOLD_ERROR;
        return NEXTSTREAM_NONE;
      }
      if (m_event.event == BD_EVENT_NONE)
        break;
      processEvent();
      if (m_hold == HOLD_ERROR || m_hold == HOLD_EXIT)
        return NEXTSTREAM_NONE;
    }
  }

  if (m_hold == HOLD_STILL || m_titleEndWait)
    return NEXTSTREAM_RETRY;

  if (titleEndWait)
    m_endedTitleTime = 0;

  m_crossPlaylistPending = false;
  if (m_prevTitleInfo)
  {
    if (m_prevClip && m_clip && m_playlist != m_prevPlaylist && !m_bMVCPlayback && !m_prevWasMVC)
    {
      m_videoCompatBoundary = AreClipVideoStreamsCompatible(m_prevClip, m_clip);
      logComponentM(LOGDEBUG, LOGBLURAY,
                    "CDVDInputStreamBluray::NextStream - boundary playlist {} to {} video-compatible: {}",
                    m_prevPlaylist, m_playlist, m_videoCompatBoundary);
    }
    FreePrevTitleInfo();
  }

  m_menuInput = false;
  m_itemAbandoned = false;
  m_hold = HOLD_DATA;
  m_lastPictureTime = -1;
  m_nextItemInPlaylist = !m_playlistLeft;
  m_nextItemSeekTime.reset();
  if (m_nextItemInPlaylist && m_itemSeeked)
    m_nextItemSeekTime = static_cast<int>(bd_tell_time(m_bd) / 90);
  m_playlistLeft = false;
  m_itemSeeked = false;
  // a join within the title plays the old item out to the start of the new one
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    m_playedItemEnd = !m_presentedLeft && clipKnown && !programStop && m_clip &&
                              m_clip->start_time > 0
                          ? static_cast<int64_t>(m_clip->start_time) - 1
                          : -1;
  }
  m_presentedLeft = false;
  SelectPendingStreams();
  m_discontinuityFlush = true;
  return NEXTSTREAM_OPEN;
}

bool CDVDInputStreamBluray::IsTitleEndHeld() const
{
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  return m_titleEndWait && m_titleInfo != nullptr;
}

bool CDVDInputStreamBluray::IsJoinHeld() const
{
  return m_navmode && m_hold == HOLD_HELD &&
         (m_event.event == BD_EVENT_PLAYITEM || m_event.event == BD_EVENT_DISCONTINUITY);
}

bool CDVDInputStreamBluray::IsTailInTitle() const
{
  if (IsTitleEndHeld())
    return true;
  return IsJoinHeld();
}

bool CDVDInputStreamBluray::IsReadHeld() const
{
  const EHoldState hold = m_hold.load();
  return m_navmode &&
         (hold == HOLD_HELD || hold == HOLD_STILL || hold == HOLD_ERROR || hold == HOLD_EXIT ||
          m_titleEndWait);
}

void CDVDInputStreamBluray::CheckPresentedLeft()
{
  switch (m_event.event)
  {
    case BD_EVENT_SEEK:
    case BD_EVENT_PLAYLIST:
    case BD_EVENT_TITLE:
    case BD_EVENT_PLAYLIST_STOP:
      if (m_hold != HOLD_DATA)
        m_presentedLeft = true;
      break;
    default:
      break;
  }
}

void CDVDInputStreamBluray::UpdatePendingGraphicsRegime(int64_t titleTime)
{
  if (!m_pendingPqGraphics || titleTime < m_pendingPqGraphicsStart)
    return;
  m_pqAuthoredGraphics.store(*m_pendingPqGraphics, std::memory_order_relaxed);
  m_pendingPqGraphics.reset();
}

void CDVDInputStreamBluray::SetPresentedTime(double pts)
{
  const int64_t titleTime =
      (pts == DVD_NOPTS_VALUE || m_presentedLeft) ? -1 : ConvertPtsToTitleTime90k(pts);
  m_presentedTime = titleTime;
  if (titleTime >= 0)
    UpdatePendingGraphicsRegime(titleTime);
  ReportPresented(titleTime);
}

void CDVDInputStreamBluray::SetPresentationClock(double pts, double speed)
{
  if (!m_bd || !m_navmode)
    return;

  const int64_t titleTime =
      (pts == DVD_NOPTS_VALUE || m_presentedLeft) ? -1 : ConvertPtsToTitleTime90k(pts);
  bd_set_presentation_clock(m_bd, titleTime,
                            static_cast<int>(std::lround(speed * BLURAY_RATE_NORMAL)));
}

void CDVDInputStreamBluray::SetLastPictureTime(double lastVideoPts, double clockPts)
{
  m_lastPictureTime = ConvertPtsToTitleTime90k(lastVideoPts);
  m_clockTime = ConvertPtsToTitleTime90k(clockPts);
}

void CDVDInputStreamBluray::SetReadPresented()
{
  if (!m_presentedLeft)
    ReportPresented(BD_PRESENTED_ALL);
}

void CDVDInputStreamBluray::ReportItemPlayedOut()
{
  if (m_playedItemEnd >= 0)
    ReportPresented(m_playedItemEnd);
  m_playedItemEnd = -1;
}

void CDVDInputStreamBluray::ReportPresented(int64_t titleTime)
{
  if (!m_bd || !m_navmode)
    return;

  // with notifications held, libbluray is asked again when one is due, after a read or when the
  // time becomes unknown; a pending start is released only after a read or once all data read
  // is presented
  const bool known = titleTime >= 0;
  if (m_heldDue != -2 && known == (m_reportedTime >= 0) &&
      (!known || (!m_readSinceReport &&
                  (m_heldDue < 0 || titleTime < m_heldDue ||
                   (m_heldDue == 0 && titleTime != BD_PRESENTED_ALL)))))
    return;

  m_heldDue = bd_presented(m_bd, titleTime);
  m_notificationsHeld = m_heldDue != -2;
  m_readSinceReport = false;
  m_reportedTime = titleTime;
}

std::optional<double> CDVDInputStreamBluray::GetHeldItemEnd() const
{
  if (!IsJoinHeld())
    return std::nullopt;

  uint64_t outTime = 0;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (!m_clip || m_clip->out_time <= m_clip->in_time)
      return std::nullopt;
    outTime = m_clip->out_time;
  }
  double seamOffset = 0.0;
  {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_seamOffsetMutex));
    seamOffset = m_seamTimeOffset;
  }
  const int64_t end90k = static_cast<int64_t>(outTime) + std::llround(seamOffset * 90000.0);
  return static_cast<double>(
      av_rescale_q(end90k, BLURAY_CLIP_TIME_BASE, AVRational{1, DVD_TIME_BASE}));
}

void CDVDInputStreamBluray::PollEvents()
{
  if (!m_bd || !m_navmode || m_hold == HOLD_EXIT || m_hold == HOLD_ERROR)
    return;

  if (m_hold == HOLD_HELD)
  {
    if (IsJoinHeld() && m_title && !m_title->bdj)
      PollHeldJoin();
    return;
  }

  if (m_hold == HOLD_STILL)
    return;

  while (true)
  {
    if (bd_read_ext(m_bd, nullptr, 0, &m_event) < 0)
    {
      m_hold = HOLD_ERROR;
      return;
    }

    // a read without data joins no play item in line
    const bool menuJump = TakeMenuJump(false, false);
    if (m_event.event == BD_EVENT_NONE)
      return;
    CheckPresentedLeft();

    switch (m_event.event)
    {
      case BD_EVENT_STILL_TIME:
        // an infinite still starts here as in the read
        if (m_event.param == 0)
          break;
        m_hold = HOLD_HELD;
        return;

      case BD_EVENT_PLAYITEM:
      case BD_EVENT_SEEK:
      case BD_EVENT_TITLE:
      case BD_EVENT_ANGLE:
      case BD_EVENT_PLAYLIST:
      case BD_EVENT_DISCONTINUITY:
        if (m_hold != HOLD_DATA)
        {
          if (AbandonsItem(menuJump, false))
            m_itemAbandoned = true;
          if (m_event.event == BD_EVENT_TITLE || m_event.event == BD_EVENT_PLAYLIST)
            StashBoundaryClip();
          m_hold = HOLD_HELD;
          return;
        }
        break;

      case BD_EVENT_END_OF_TITLE:
        if (m_event.param != 0 || (m_title && m_title->bdj))
        {
          m_atTitleEnd = true;
          m_titleEndWait = true;
        }
        break;

      default:
        break;
    }

    ProcessEvent();
    if (m_hold == HOLD_STILL || m_hold == HOLD_ERROR || m_hold == HOLD_EXIT ||
        m_hold == HOLD_HELD)
      return;
  }
}

// an HDMV program waits in the playlist while a join is held and runs only on a key, a menu call
// or a page action; a stream or menu change it makes applies to the playing item at once, other
// events stay behind the held event, and a jump leaves the held item
void CDVDInputStreamBluray::PollHeldJoin()
{
  const BD_EVENT held = m_event;
  while (m_hold != HOLD_EXIT && m_hold != HOLD_ERROR)
  {
    if (bd_read_ext(m_bd, nullptr, 0, &m_event) < 0)
    {
      m_hold = HOLD_ERROR;
      break;
    }
    if (m_event.event == BD_EVENT_NONE)
      break;
    if (m_event.event == BD_EVENT_ERROR)
    {
      ProcessEvent();
      break;
    }
    CheckPresentedLeft();

    switch (m_event.event)
    {
      case BD_EVENT_AUDIO_STREAM:
      case BD_EVENT_PG_TEXTST:
      case BD_EVENT_PG_TEXTST_STREAM:
      case BD_EVENT_IG_STREAM:
      case BD_EVENT_SECONDARY_AUDIO:
      case BD_EVENT_SECONDARY_AUDIO_STREAM:
      case BD_EVENT_SECONDARY_VIDEO:
      case BD_EVENT_SECONDARY_VIDEO_SIZE:
      case BD_EVENT_SECONDARY_VIDEO_STREAM:
      case BD_EVENT_MENU:
      case BD_EVENT_POPUP:
      case BD_EVENT_UO_MASK_CHANGED:
      case BD_EVENT_SOUND_EFFECT:
      case BD_EVENT_KEY_INTEREST_TABLE:
        if (m_heldEvents.empty())
        {
          ProcessEvent();
          continue;
        }
        break;

      case BD_EVENT_SEEK:
      case BD_EVENT_TITLE:
      case BD_EVENT_PLAYLIST:
        m_itemAbandoned = true;
        break;

      default:
        break;
    }
    m_heldEvents.push(m_event);
  }
  m_event = held;
}

bool CDVDInputStreamBluray::TakeHeldEvent()
{
  if (m_heldEvents.empty())
    return false;
  m_event = m_heldEvents.front();
  m_heldEvents.pop();
  return true;
}

// true when the disc program left the playlist during the hold, which the next stream still opens
bool CDVDInputStreamBluray::ProcessHeldEvents()
{
  bool jump = false;
  while (TakeHeldEvent())
  {
    jump = jump || m_event.event == BD_EVENT_TITLE || m_event.event == BD_EVENT_PLAYLIST;
    ProcessEvent();
  }
  return jump;
}

// the disc program answers a menu key with its next seek, play item, playlist or title
bool CDVDInputStreamBluray::TakeMenuJump(bool joinsInLine, bool dataRead)
{
  const bool menuJump =
      m_menuInput &&
      (m_event.event == BD_EVENT_SEEK || m_event.event == BD_EVENT_TITLE ||
       m_event.event == BD_EVENT_PLAYLIST || (m_event.event == BD_EVENT_PLAYITEM && !joinsInLine));
  // an HDMV program has run to its suspension once a read returns data or reports nothing
  if (menuJump || ((dataRead || m_event.event == BD_EVENT_NONE) && m_title && !m_title->bdj))
    m_menuInput = false;
  return menuJump;
}

// a jump with no data that answers the key leaves the item unplayed on HDMV and BD-J titles alike,
// as does a BD-J play item or seek with a notification that came with its picture; a still has
// nothing read past it
bool CDVDInputStreamBluray::AbandonsItem(bool menuJump, bool fromStill) const
{
  if (!m_title)
    return false;
  if (!m_title->bdj)
    return menuJump && !fromStill;
  return menuJump || (m_notificationsHeld && !fromStill &&
                      (m_event.event == BD_EVENT_PLAYITEM || m_event.event == BD_EVENT_SEEK));
}

// the next jump of the disc program answers the key; an HDMV program takes the events queued
// before the key first
void CDVDInputStreamBluray::MarkMenuInput()
{
  if (m_title && !m_title->bdj && m_playerThread == std::this_thread::get_id())
    PollEvents();
  m_menuInput = true;
}

bool CDVDInputStreamBluray::TakeItemAbandoned()
{
  const bool abandoned = m_itemAbandoned;
  m_itemAbandoned = false;
  return abandoned;
}

const BLURAY_CLIP_INFO* CDVDInputStreamBluray::GetActiveClipLocked() const
{
  if (m_clip)
    return m_clip;
  if (m_titleInfo && m_titleInfo->clip_count > 0)
    return &m_titleInfo->clips[0];
  return nullptr;
}

bool CDVDInputStreamBluray::ListsEnhancementLayer(int pid) const
{
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 5, 0))
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  const BLURAY_CLIP_INFO* clip = GetActiveClipLocked();
  if (!clip)
    return true;
  return std::any_of(clip->dv_streams, clip->dv_streams + clip->dv_stream_count,
                     [pid](const BLURAY_STREAM_INFO& info)
                     { return info.pid == static_cast<uint16_t>(pid); });
#else
  return true;
#endif
}

bool CDVDInputStreamBluray::GetDemuxerGraphicsPlane(int pid, int& width, int& height) const
{
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  const BLURAY_CLIP_INFO* clip = GetActiveClipLocked();
  if (!clip || clip->video_stream_count == 0 ||
      std::none_of(clip->pg_streams, clip->pg_streams + clip->pg_stream_count,
                   [pid](const BLURAY_STREAM_INFO& info)
                   { return info.pid == static_cast<uint16_t>(pid); }))
    return false;

  // the plane follows the primary video, except that a UHD play item keeps the HD plane
  switch (clip->video_streams[0].format)
  {
    case BLURAY_VIDEO_FORMAT_480I:
    case BLURAY_VIDEO_FORMAT_480P:
      width = 720;
      height = 480;
      return true;
    case BLURAY_VIDEO_FORMAT_576I:
    case BLURAY_VIDEO_FORMAT_576P:
      width = 720;
      height = 576;
      return true;
    case BLURAY_VIDEO_FORMAT_720P:
      width = 1280;
      height = 720;
      return true;
    case BLURAY_VIDEO_FORMAT_1080I:
    case BLURAY_VIDEO_FORMAT_1080P:
    case BLURAY_VIDEO_FORMAT_2160P:
      width = 1920;
      height = 1080;
      return true;
    default:
      return false;
  }
}

bool CDVDInputStreamBluray::GetDemuxerFrameRate(int pid, int& rate, int& scale) const
{
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  const BLURAY_CLIP_INFO* clip = GetActiveClipLocked();
  if (!clip || clip->video_stream_count == 0)
    return false;

  // a stream the play item does not list, such as an MVC view, runs with the primary video
  const BLURAY_STREAM_INFO* info{&clip->video_streams[0]};
  const auto pick = [pid, &info](const BLURAY_STREAM_INFO* streams, uint8_t count)
  {
    for (uint8_t i = 0; i < count; ++i)
    {
      if (streams[i].pid == static_cast<uint16_t>(pid))
        info = &streams[i];
    }
  };
  pick(clip->video_streams, clip->video_stream_count);
  pick(clip->sec_video_streams, clip->sec_video_stream_count);
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 5, 0))
  pick(clip->dv_streams, clip->dv_stream_count);
#endif

  switch (info->format)
  {
    case BLURAY_VIDEO_FORMAT_480P:
    case BLURAY_VIDEO_FORMAT_576P:
    case BLURAY_VIDEO_FORMAT_720P:
    case BLURAY_VIDEO_FORMAT_1080P:
    case BLURAY_VIDEO_FORMAT_2160P:
      break;
    default:
      return false;
  }

  switch (info->rate)
  {
    case BLURAY_VIDEO_RATE_24000_1001:
      rate = 24000;
      scale = 1001;
      return true;
    case BLURAY_VIDEO_RATE_24:
      rate = 24;
      scale = 1;
      return true;
    case BLURAY_VIDEO_RATE_25:
      rate = 25;
      scale = 1;
      return true;
    case BLURAY_VIDEO_RATE_30000_1001:
      rate = 30000;
      scale = 1001;
      return true;
    case BLURAY_VIDEO_RATE_50:
      rate = 50;
      scale = 1;
      return true;
    case BLURAY_VIDEO_RATE_60000_1001:
      rate = 60000;
      scale = 1001;
      return true;
    default:
      return false;
  }
}

bool CDVDInputStreamBluray::CanSelectStream() const
{
  // libbluray already plays the play item of a held event, which m_clip does not describe yet
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  return m_bd && m_navmode && m_titleInfo && m_clip && m_hold != HOLD_HELD &&
         m_event.event == BD_EVENT_NONE && m_heldEvents.empty();
}

// a pick or toggle while the read is held is written once the read resumes, for the play item
// libbluray then plays
void CDVDInputStreamBluray::SelectPendingStreams()
{
  if ((!m_audioPidPending && !m_pgPidPending && !m_pgVisiblePending) || !CanSelectStream())
    return;

  if (const auto pid = std::exchange(m_audioPidPending, std::nullopt))
    SetActiveAudioStream(*pid);
  if (const auto pid = std::exchange(m_pgPidPending, std::nullopt))
    SetActiveSubtitleStream(*pid, m_pgVisiblePending.value_or(m_pgEnabled));
  else if (m_pgVisiblePending)
    SetSubtitleVisible(*m_pgVisiblePending);
}

bool CDVDInputStreamBluray::SetActiveAudioStream(int pid)
{
  if (!m_bd || !m_navmode)
    return false;
  if (!CanSelectStream())
  {
    m_audioPidPending = pid;
    return true;
  }
  m_audioPidPending.reset();

  unsigned int number = 0;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (!m_clip)
      return false;
    number = stream_number(pid, m_clip->audio_streams, m_clip->audio_stream_count);
  }
  if (number == 0)
    return false;

  bd_select_stream(m_bd, BLURAY_AUDIO_STREAM, number, 1);
  return true;
}

bool CDVDInputStreamBluray::SetActiveSubtitleStream(int pid, bool visible)
{
  if (!m_bd || !m_navmode)
    return false;
  if (!CanSelectStream())
  {
    m_pgPidPending = pid;
    m_pgVisiblePending = visible;
    return true;
  }
  m_pgPidPending.reset();
  m_pgVisiblePending.reset();

  unsigned int number = 0;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (!m_clip)
      return false;
    number = stream_number(pid, m_clip->pg_streams, m_clip->pg_stream_count);
  }
  if (number == 0)
    return false;

  // the flag rides along with the stream number, so keep the disc's while it forbids toggling
  bd_select_stream(m_bd, BLURAY_PG_TEXTST_STREAM, number,
                   CanToggleSubtitle() ? visible : m_pgEnabled);
  m_pgStreamNumber = number;
  return true;
}

void CDVDInputStreamBluray::SetSubtitleVisible(bool visible)
{
  if (!m_bd || !m_navmode)
    return;
  if (!CanSelectStream())
  {
    m_pgVisiblePending = visible;
    return;
  }
  m_pgVisiblePending.reset();

  uint32_t pgCount = 0;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (!m_clip)
      return;
    pgCount = static_cast<uint32_t>(m_clip->pg_stream_count);
  }
  if (!CanToggleSubtitle() || m_pgStreamNumber == 0 || m_pgStreamNumber > pgCount)
    return;

  bd_select_stream(m_bd, BLURAY_PG_TEXTST_STREAM, m_pgStreamNumber, visible);
}

bool CDVDInputStreamBluray::CanChangeAudioStream() const
{
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 4, 1))
  return !m_navmode || !(m_uoMask.load() & BLURAY_UO_PRIMARY_AUDIO_CHANGE_MASK);
#else
  return true;
#endif
}

bool CDVDInputStreamBluray::CanChangeSubtitleStream() const
{
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 4, 1))
  return !m_navmode || !(m_uoMask.load() & BLURAY_UO_PG_TEXTST_CHANGE_MASK);
#else
  return true;
#endif
}

bool CDVDInputStreamBluray::CanToggleSubtitle() const
{
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 4, 1))
  return !m_navmode || !(m_uoMask.load() & BLURAY_UO_PG_TEXTST_ENABLE_DISABLE_MASK);
#else
  return true;
#endif
}

void CDVDInputStreamBluray::UserInput(bd_vk_key_e vk)
{
  if(m_bd == nullptr || !m_navmode)
  {
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "CDVDInputStreamBluray::UserInput - key {} skipped (bd={} navmode={})",
                  static_cast<int>(vk), m_bd != nullptr, m_navmode.load());
    return;
  }

  // only an activation key makes a BD-J program jump away from the item; an HDMV move can select
  // a button that acts at once
  if (vk == BD_VK_ENTER || vk == BD_VK_POPUP || vk == BD_VK_ROOT_MENU ||
      (vk >= BD_VK_0 && vk <= BD_VK_9) || (m_title && !m_title->bdj))
    MarkMenuInput();
  // the events of the key reach the player thread through its next read or poll
  int ret = bd_user_input(m_bd, -1, vk);
  logComponentM(LOGDEBUG, LOGBLURAY,
                "CDVDInputStreamBluray::UserInput - key {} → bd_user_input ret={}",
                static_cast<int>(vk), ret);
  if (ret < 0)
  {
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray::UserInput - user input failed");
  }
}

bool CDVDInputStreamBluray::MouseMove(const CPoint &point) const {
  if (m_bd == nullptr || !m_navmode)
    return false;

  // Disable mouse selection for BD-J menus, since it's not implemented in libbluray as of version 1.0.2
  if (m_title && m_title->bdj == 1)
    return false;

  if (bd_mouse_select(m_bd, -1, static_cast<uint16_t>(point.x), static_cast<uint16_t>(point.y)) < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseMove - mouse select failed");
    return false;
  }

  return true;
}

bool CDVDInputStreamBluray::MouseClick(const CPoint &point) {
  if (m_bd == nullptr || !m_navmode)
    return false;

  // Disable mouse selection for BD-J menus, since it's not implemented in libbluray as of version 1.0.2
  if (m_title && m_title->bdj == 1)
    return false;

  if (bd_mouse_select(m_bd, -1, static_cast<uint16_t>(point.x), static_cast<uint16_t>(point.y)) < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseClick - mouse select failed");
    return false;
  }

  MarkMenuInput();
  if (bd_user_input(m_bd, -1, BD_VK_MOUSE_ACTIVATE) >= 0)
    return true;

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseClick - mouse click (user input) failed");
  return false;
}

bool CDVDInputStreamBluray::OnColorKey(int key)
{
  if (m_bd == nullptr || !m_navmode)
    return false;

  bd_vk_key_e vk;
  switch (key)
  {
    case 0:
      vk = BD_VK_RED;
      break;
    case 1:
      vk = BD_VK_GREEN;
      break;
    case 2:
      vk = BD_VL_YELLOW;
      break;
    case 3:
      vk = BD_VK_BLUE;
      break;
    default:
      return false;
  }
  logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray::OnColorKey - key {}", key);
  MarkMenuInput();
  return bd_user_input(m_bd, -1, vk) >= 0;
}

bool CDVDInputStreamBluray::OpenPopup()
{
  bool trackOpening = false;
  if (m_isBdjTitle && !m_isInMainMenu)
  {
    std::lock_guard lock(m_overlayLock);
    // BD-J reports the lifetime of the graphics plane, not whether its popup is visible.
    // Track the popup we request over a clear plane and confirm it on the first visible flush.
    trackOpening = !m_hasVisibleBdjOverlay;
    if (trackOpening)
    {
      m_bdjPopupDeadline = std::chrono::steady_clock::now() + 2s;
      m_bdjPopup = BdjPopup::OPENING;
    }
  }

  MarkMenuInput();
  if (bd_user_input(m_bd, -1, BD_VK_POPUP) >= 0)
    return true;

  if (trackOpening)
    m_bdjPopup = BdjPopup::CLOSED;
  m_menuInput = false;
  return false;
}

void CDVDInputStreamBluray::UpdateBdjPopup(bool visible)
{
  // This runs with m_overlayLock held by OverlayFlush.
  auto popup = m_bdjPopup.load();
  switch (popup)
  {
    case BdjPopup::OPENING:
      if (std::chrono::steady_clock::now() >= m_bdjPopupDeadline)
        m_bdjPopup.compare_exchange_strong(popup, BdjPopup::CLOSED);
      else if (visible && m_bdjPopup.compare_exchange_strong(popup, BdjPopup::OPEN))
        CLog::Log(LOGDEBUG, "Blu-ray popup: visible after menu request");
      break;
    case BdjPopup::OPEN:
    case BdjPopup::CLOSING:
      if (!visible)
        m_bdjPopup.compare_exchange_strong(popup, BdjPopup::CLOSED);
      break;
    case BdjPopup::CLOSED:
      break;
  }
}

bool CDVDInputStreamBluray::CanClosePopup() const
{
  if (!m_bd || !m_navmode || m_isInMainMenu ||
      (m_uoMask.load() & BLURAY_UO_POPUP_OFF_MASK))
    return false;

  if (!m_isBdjTitle)
    return m_popupAvailable && m_menu;

  const auto popup = m_bdjPopup.load();
  return m_hasVisibleBdjOverlay && (popup == BdjPopup::OPEN || popup == BdjPopup::CLOSING);
}

void CDVDInputStreamBluray::ClosePopup()
{
  const bool bdj = m_isBdjTitle;
  if (bdj)
  {
    std::lock_guard lock(m_overlayLock);
    const auto now = std::chrono::steady_clock::now();
    if (!CanClosePopup() || (m_bdjPopup == BdjPopup::CLOSING && now < m_bdjPopupDeadline))
      return;
    // Repeated Back presses during the close animation must not toggle the menu back on.
    m_bdjPopupDeadline = now + 2s;
    m_bdjPopup = BdjPopup::CLOSING;
  }

  MarkMenuInput();
  const int result = bd_user_input(m_bd, -1, BD_VK_POPUP);
  CLog::Log(LOGDEBUG, "Blu-ray popup: Back sent POP UP MENU, result={}", result);
  if (bdj && result < 0)
  {
    BdjPopup closing = BdjPopup::CLOSING;
    m_bdjPopup.compare_exchange_strong(closing, BdjPopup::OPEN);
  }
}

bool CDVDInputStreamBluray::OnMenu(MenuCall call)
{
  if(m_bd == nullptr || !m_navmode)
  {
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray::OnMenu - navigation mode not enabled");
    return false;
  }

  auto uoBlocked = [this]() -> bool {
    if (!(m_uoMask.load() & BLURAY_UO_MENU_CALL))
      return false;
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray::OnMenu - menu call masked by disc UO");
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Blu-ray",
                                          g_localizeStrings.Get(60654));
    return true;
  };

  const bool bdjMenuAllowed =
      !m_topMenuIsBdj || CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
                             CSettings::SETTING_DISC_ALLOW_BDJ_TOP_MENU);
  const bool popupUoMasked = (m_uoMask.load() & BLURAY_UO_POPUP_ON_MASK) != 0;

  auto popupBlockedUo = [this]() {
    logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray::OnMenu - popup masked by disc UO");
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Blu-ray",
                                          g_localizeStrings.Get(60654));
  };

  auto tryTop = [this]() -> bool {
    MarkMenuInput();
    if (bd_user_input(m_bd, -1, BD_VK_ROOT_MENU) >= 0)
      return true;
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "CDVDInputStreamBluray::OnMenu - root key failed, trying bd_menu_call");
    if (bd_menu_call(m_bd, -1) > 0)
      return true;
    m_menuInput = false;
    return false;
  };

  auto blockedBdjMenu = [this]() {
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "CDVDInputStreamBluray::OnMenu - BD-J disc menu blocked by disc.allowbdjtopmenu");
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "Blu-ray",
                                          g_localizeStrings.Get(60659));
  };

  switch (call)
  {
    case MenuCall::Popup:
      if (!bdjMenuAllowed)
      {
        blockedBdjMenu();
        return false;
      }
      if (popupUoMasked)
      {
        popupBlockedUo();
        return false;
      }
      return OpenPopup();
    case MenuCall::Top:
      if (uoBlocked())
        return false;
      if (!bdjMenuAllowed)
      {
        blockedBdjMenu();
        return false;
      }
      return tryTop();
    case MenuCall::Auto:
    default:
      if (bdjMenuAllowed && !popupUoMasked && OpenPopup())
        return true;
      logComponentM(LOGDEBUG, LOGBLURAY, "CDVDInputStreamBluray::OnMenu - popup unavailable, trying root");
      if (uoBlocked())
        return false;
      if (!bdjMenuAllowed)
      {
        blockedBdjMenu();
        return false;
      }
      return tryTop();
  }
}

bool CDVDInputStreamBluray::IsInMenu()
{
  const bool isBdj = m_isBdjTitle.load(std::memory_order_relaxed);
  const bool seekProhibited = (m_uoMask.load() & BLURAY_UO_TIME_SEARCH_MASK) != 0;

  // For HDMV titles, libbluray's BD_EVENT_MENU (m_menu) accurately tracks GC_STATUS_MENU_OPEN.
  // For BD-J titles, libbluray sets BD_EVENT_MENU(1) once on BD_ARGB_OVERLAY_INIT and keeps it
  // set for the entire title lifetime even while playing full-screen trailers/intros/movies with
  // a completely transparent ARGB plane. Gate L1 on !isBdj and rely on pixel-accurate
  // m_hasVisibleBdjOverlay (L2) for BD-J so OK opens Kodi's OSD during full-screen BD-J video.
  bool result;
  const char* layer;
  if (m_bd == nullptr || !m_navmode)             { result = false; layer = "no_bd_or_navmode"; }
  else if (!isBdj && m_menu)                     { result = true;  layer = "L1_hdmv_m_menu"; }
  else if (m_hasVisibleBdjOverlay)               { result = true;  layer = "L2_bdj_visible_overlay"; }
  else if (m_hasMenuOverlay && seekProhibited)   { result = true;  layer = "L3_hdmv_menu_graphics"; }
  else                                           { result = false; layer = "no_match"; }

  const int now = result ? 1 : 0;
  if (m_lastIsInMenuLogged.exchange(now) != now)
  {
    logComponentM(LOGDEBUG, LOGBLURAY,
                  "IsInMenu -> {} via {} (bd={} navmode={} isBdj={} m_menu={} m_hold={} "
                  "m_hasOverlay={} m_hasMenuOverlay={} m_hasVisibleBdjOverlay={} "
                  "seekProhibited={} uoMask=0x{:x})",
                  result, layer, m_bd != nullptr, m_navmode.load(), isBdj, m_menu.load(),
                  static_cast<int>(m_hold.load()), m_hasOverlay.load(), m_hasMenuOverlay.load(),
                  m_hasVisibleBdjOverlay.load(), seekProhibited, m_uoMask.load());
  }
  return result;
}

namespace
{
constexpr uint64_t MENU_DOMAIN_MAX_PLAYLIST_DURATION = 600ULL * 90000ULL;
}

bool CDVDInputStreamBluray::PlaylistWithinMenuDurationBound() const
{
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  return m_titleInfo && m_titleInfo->duration <= MENU_DOMAIN_MAX_PLAYLIST_DURATION;
}

bool CDVDInputStreamBluray::TitleCarriesAlwaysOnMenuComposition() const
{
  std::lock_guard<std::mutex> lock(m_clipTableMutex);
  if (!m_titleInfo)
    return false;
  if (m_titleInfo->duration > MENU_DOMAIN_MAX_PLAYLIST_DURATION && !m_hasMenuOverlay)
    return false;
  for (uint32_t i = 0; i < m_titleInfo->clip_count; ++i)
  {
    if (m_titleInfo->clips[i].ig_stream_count > 0)
      return true;
  }
  return false;
}

bool CDVDInputStreamBluray::IsMenuDomainSegment() const
{
  const bool isBdj = m_isBdjTitle.load(std::memory_order_relaxed);
  bool result;
  const char* reason;
  if (m_bd == nullptr || !m_navmode)
  {
    result = false;
    reason = "no_bd_or_navmode";
  }
  else if (!isBdj && m_menu)
  {
    result = true;
    reason = "hdmv_menu_flag";
  }
  else if (isBdj && m_hasVisibleBdjOverlay && PlaylistWithinMenuDurationBound())
  {
    result = true;
    reason = "bdj_visible_menu";
  }
  else if (m_hasMenuOverlay && (m_uoMask.load() & BLURAY_UO_TIME_SEARCH_MASK) != 0)
  {
    result = true;
    reason = "menu_graphics";
  }
  else if (m_popupAvailable)
  {
    result = false;
    reason = "popup_ig_over_content";
  }
  else if (TitleCarriesAlwaysOnMenuComposition())
  {
    result = true;
    reason = "stn_ig";
  }
  else if (const uint32_t titleNumber = m_titleNumber.load();
           (titleNumber == BLURAY_TITLE_TOP_MENU || titleNumber == BLURAY_TITLE_FIRST_PLAY) &&
           PlaylistWithinMenuDurationBound())
  {
    result = true;
    reason = "menu_title";
  }
  else
  {
    result = false;
    reason = "no_match";
  }
  const int now = result ? 1 : 0;
  if (m_lastMenuDomainLogged.exchange(now) != now)
    logComponentM(LOGDEBUG, LOGBLURAY, "IsMenuDomainSegment -> {} via {}", result, reason);
  return result;
}

bool CDVDInputStreamBluray::ConsumeDiscontinuityFlush()
{
  return m_discontinuityFlush.exchange(false);
}

void CDVDInputStreamBluray::SkipStill()
{
  if(m_bd == nullptr || !m_navmode)
    return;

  if ( m_hold == HOLD_STILL)
  {
    m_hold = HOLD_HELD;
    bd_read_skip_still(m_bd);

    /* process all queued up events */
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }
}

bool CDVDInputStreamBluray::CanSeek()
{
  if (m_navmode && (m_uoMask.load() & BLURAY_UO_TIME_SEARCH_MASK))
    return false;
  return !IsInMenu() || !m_isInMainMenu;
}


MenuType CDVDInputStreamBluray::GetSupportedMenuType()
{
  if (m_navmode)
  {
    return MenuType::NATIVE;
  }
  return MenuType::NONE;
}

bool CDVDInputStreamBluray::ProcessItem(int playitem)
{
  ReplaceTitleInfo(bd_get_playlist_info(m_bd, playitem, m_angle));

  UpdateGraphicsRegime();

  if (!m_bMVCDisabled)
  {
    m_bMVCPlayback = false;
    m_nMVCSubPathIndex = 0;
    if (!m_titleInfo)
    {
      logM(LOGWARNING, "CDVDInputStreamBluray::ProcessItem - no title info for playlist {}",
           playitem);
      CloseMVCDemux();
      return false;
    }

    uint8_t mvcBaseViewRFlag = 0;
    {
      std::lock_guard<std::mutex> lock(m_clipTableMutex);
      if (m_titleInfo)
        mvcBaseViewRFlag = m_titleInfo->mvc_base_view_r_flag;
    }

    MPLS_PL * mpls = bd_get_title_mpls(m_bd);
    if (mpls)
    {
      for (int i = 0; i < mpls->ext_sub_count; i++)
      {
        if (mpls->ext_sub_path[i].type == 8
          && mpls->ext_sub_path[i].sub_playitem_count == mpls->list_count)
        {
          CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - Enabling BD3D MVC demuxing");
          logComponentM(LOGDEBUG, LOGBLURAY, "MVC_Base_view_R_flag: {}", mvcBaseViewRFlag);
          m_bMVCPlayback = true;
          m_nMVCSubPathIndex = i;
          m_bFlipEyes = mvcBaseViewRFlag != 0;
          break;
        }
      }
    }
  }
  CloseMVCDemux();
  return true;
}

int CDVDInputStreamBluray::Get3dSubtitlePlane(uint16_t pid) const {
  if (!m_bMVCDisabled)
  {
    MPLS_PL *mpls = bd_get_title_mpls(m_bd);
    if (mpls)
    {
      for (int i = 0; i < mpls->list_count; i++)
      {
        for (int s = 0; s < mpls->play_item[i].stn.num_pg; s++)
        {
          if (mpls->play_item[i].stn.pg[s].pid == pid && mpls->play_item[i].stn.pg[s].ss_offset_sequence_id != 0xff)
            return mpls->play_item[i].stn.pg[s].ss_offset_sequence_id;
        }
      }
    }
  }

  return 0;
}

bool CDVDInputStreamBluray::OpenNextStream()
{
  int clip = 0;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (m_clipQueue.empty())
      return false;

    clip = m_clipQueue.front();
    m_clipQueue.pop();
  }

  auto pMVCDemux = dynamic_cast<CDemuxMVC*>(m_pMVCDemux);
  if (!pMVCDemux) {
    // either it's not a CDemuxMVC or it's 2D playback
    CloseMVCDemux();
    return OpenMVCDemux(clip);
  }

  // save start time for the next clip
  int64_t start_time = pMVCDemux->GetStartTime();

  CloseMVCDemux();

  bool res = OpenMVCDemux(clip);
  if (res) {
    auto nextDemux = dynamic_cast<CDemuxMVC*>(m_pMVCDemux);
    if (nextDemux) {
      // set start time for next clip
      auto menu = dynamic_cast<CDVDInputStream::IMenus*>(this);
      nextDemux->SetStartTime(start_time, menu->GetSupportedMenuType());
    }
  }

  return res;
}

bool CDVDInputStreamBluray::OpenMVCDemux(int playItem)
{
  MPLS_PL *pl = bd_get_title_mpls(m_bd);
  if (!pl)
    return false;

  if (m_nMVCSubPathIndex < 0 || m_nMVCSubPathIndex >= pl->ext_sub_count)
  {
    logM(LOGWARNING,
         "CDVDInputStreamBluray::OpenMVCDemux - subpath index {} out of range ({} ext subpaths)",
         m_nMVCSubPathIndex, pl->ext_sub_count);
    return false;
  }

  const int subItems =
      static_cast<int>(pl->ext_sub_path[m_nMVCSubPathIndex].sub_playitem_count);
  if (playItem < 0 || playItem >= subItems)
  {
    logM(LOGWARNING,
         "CDVDInputStreamBluray::OpenMVCDemux - playitem {} out of range (subpath {} has {} sub "
         "playitems)",
         playItem, m_nMVCSubPathIndex, subItems);
    return false;
  }

  std::string strFileName;
  strFileName.append(m_root);
  strFileName.append("/BDMV/STREAM/");
  strFileName.append(pl->ext_sub_path[m_nMVCSubPathIndex].sub_play_item[playItem].clip->clip_id);
  strFileName.append(".m2ts");

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OpenMVCDemuxer(): Opening MVC extension stream at {}", strFileName);

  std::string relPath = "BDMV/STREAM/";
  relPath.append(pl->ext_sub_path[m_nMVCSubPathIndex].sub_play_item[playItem].clip->clip_id);
  relPath.append(".m2ts");

  BD_FILE_H* bdFile = bd_open_file_dec(m_bd, relPath.c_str());
  if (bdFile)
  {
    int64_t length = bdFile->seek(bdFile, 0, SEEK_END);
    if (length >= 0 && bdFile->seek(bdFile, 0, SEEK_SET) >= 0)
      m_pMVCInput = new CDVDInputStreamBlurayFile(bdFile, strFileName, length);
    else
    {
      bdFile->close(bdFile);
      bdFile = nullptr;
    }
  }
  if (!bdFile)
  {
    unsigned int flags = 0;
#ifdef BD_FILE_OPEN_DIRECT_IO_HINT
    if (m_navmode && IsRemoteUdfImage(m_rootPath))
      flags = XFILE::READ_NO_CACHE;
#endif
    CFileItem fileitem(CURL(strFileName), false);
    m_pMVCInput = new CDVDInputStreamFile(fileitem, flags);
  }

  // Try to open the MVC stream
  if (!m_pMVCInput->Open())
  {
    CloseMVCDemux();
    m_bMVCPlayback = false;
    return false;
  }

  if (m_pMVCDemux)
    delete m_pMVCDemux;

  auto pMVCDemux = new CDemuxMVC;
  m_pMVCDemux = pMVCDemux;

  if (!pMVCDemux->Open(m_pMVCInput))
  {
    CloseMVCDemux();
    m_bMVCPlayback = false;
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (m_titleInfo && static_cast<uint32_t>(playItem) < m_titleInfo->clip_count)
      m_nMVCClip = m_titleInfo->clips + playItem;
    else
      LOG_THROTTLE_PERIODIC(LOGWARNING, LOGBLURAY, 1000,
                            "CDVDInputStreamBluray::OpenMVCDemux - clip table changed during open, "
                            "playitem {} not tracked (clips={})",
                            playItem, m_titleInfo ? m_titleInfo->clip_count : 0u);
  }
  return true;
}

bool CDVDInputStreamBluray::CloseMVCDemux()
{
  if (m_pMVCDemux)
  {
    delete m_pMVCDemux;
    m_pMVCDemux = nullptr;
  }

  delete m_pMVCInput;
  m_pMVCInput = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    m_nMVCClip = nullptr;
  }
  return true;
}

void CDVDInputStreamBluray::SeekMVCDemux(int64_t time)
{
  if (!m_bMVCPlayback || !m_pMVCDemux)
    return;

  auto pMVCDemux = dynamic_cast<CDemuxMVC*>(m_pMVCDemux);
  if (pMVCDemux)
    pMVCDemux->SeekTimeRelative(static_cast<double>(time), true);
  else
    m_pMVCDemux->SeekTime(static_cast<double>(time), time < GetTime());
}

void CDVDInputStreamBluray::SetupPlayerSettings() const {
  int region = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_BLURAY_PLAYERREGION);
  if ( region != BLURAY_REGION_A
    && region != BLURAY_REGION_B
    && region != BLURAY_REGION_C)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray::Open - Blu-ray region must be set in setting, assuming region A");
    region = BLURAY_REGION_A;
  }
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_REGION_CODE, static_cast<uint32_t>(region));
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PARENTAL, 99);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_3D_CAP,
                        aml_display_support_3d() ? 0xffffffff : 0);
#if (BLURAY_VERSION < BLURAY_VERSION_CODE(1, 1, 0))
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 0, 2))
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE, BLURAY_PLAYER_PROFILE_6_v3_1);
  ApplyUHDCapabilities();
#else
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE, BLURAY_PLAYER_PROFILE_5_v2_4);
#endif
#endif
  ApplyAudioCapability();

  std::string langCode;
  g_LangCodeExpander.ConvertToISO6392T(g_langInfo.GetDVDAudioLanguage(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, langCode.c_str());

  g_LangCodeExpander.ConvertToISO6392T(g_langInfo.GetDVDSubtitleLanguage(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_PG_LANG, langCode.c_str());

  g_LangCodeExpander.ConvertToISO6392T(g_langInfo.GetDVDMenuLanguage(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_MENU_LANG, langCode.c_str());

  g_LangCodeExpander.ConvertToISO6391(g_langInfo.GetRegionLocale(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_COUNTRY_CODE, langCode.c_str());

#ifdef HAVE_LIBBLURAY_BDJ
  std::string cacheDir = CSpecialProtocol::TranslatePath("special://userdata/cache/bluray/cache");
  std::string persistentDir = CSpecialProtocol::TranslatePath("special://userdata/cache/bluray/persistent");
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_PERSISTENT_ROOT, persistentDir.c_str());
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_CACHE_ROOT, cacheDir.c_str());
#endif
}

void CDVDInputStreamBluray::ApplyUHDCapabilities() const
{
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 0, 2))
  const bool dvChain = (aml_dv_mode() != DV_MODE_OFF) && aml_display_support_dv();
  uint32_t uhdCap = 0x01;
  uint32_t uhdDisplayCap = 0x02;
  if (aml_display_support_4k())
    uhdDisplayCap |= 0x01;
  if (dvChain)
    uhdCap |= 0x02;
  if (aml_display_support_hdr_hlg())
    uhdCap |= 0x04;
  if (aml_display_support_hdr10plus())
    uhdCap |= 0x20;
  if (aml_display_support_dv())
    uhdDisplayCap |= 0x04;
  if (aml_display_support_hdr_hlg())
    uhdDisplayCap |= 0x08;
  if (aml_display_support_hdr10plus())
    uhdDisplayCap |= 0x10;
  uint32_t hdrPreference;
  if (dvChain)
    hdrPreference = 0x02;
  else if (aml_display_support_hdr10plus())
    hdrPreference = 0x20;
  else
    hdrPreference = 0x01;
  logM(LOGINFO,
       "CDVDInputStreamBluray: UHD capability PSRs: UHD_CAP 0x{:02x} UHD_DISPLAY_CAP 0x{:02x} HDR_PREFERENCE 0x{:02x}",
       uhdCap, uhdDisplayCap, hdrPreference);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_UHD_CAP, uhdCap);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_UHD_DISPLAY_CAP, uhdDisplayCap);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_HDR_PREFERENCE, hdrPreference);
#endif
}

void CDVDInputStreamBluray::ApplyAudioCapability() const
{
  const AMLHdmiAudioCaps caps = aml_get_hdmi_audio_caps();
  if (!caps.valid)
  {
    logM(LOGINFO,
         "CDVDInputStreamBluray: audio capability PSR15 - no aud_cap node, keeping libbluray default");
    return;
  }
  auto pick = [](int ch, uint32_t surroundBit, uint32_t stereoBit) -> uint32_t {
    if (ch == 0)
      return 0;
    return (ch > 2 || ch < 0) ? surroundBit : stereoBit;
  };
  auto combine = [](int a, int b) -> int {
    if (a == 0)
      return b;
    if (b == 0)
      return a;
    if (a < 0 || b < 0)
      return -1;
    return a > b ? a : b;
  };
  uint32_t acap = 0;
  acap |= pick(caps.pcm_ch != 0 ? caps.pcm_ch : -1, BLURAY_ACAP_LPCM_48_96_SURROUND,
               BLURAY_ACAP_LPCM_48_96_STEREO_ONLY);
  if (caps.pcm_192k)
    acap |= pick(caps.pcm_ch, BLURAY_ACAP_LPCM_192_SURROUND, BLURAY_ACAP_LPCM_192_STEREO_ONLY);
  acap |= pick(caps.truehd_ch, BLURAY_ACAP_MLP_SURROUND, BLURAY_ACAP_MLP_STEREO_ONLY);
  acap |= pick(caps.ddp_ch, BLURAY_ACAP_DDPLUS_SURROUND, BLURAY_ACAP_DDPLUS_STEREO_ONLY);
  if (caps.ddp_atmos)
    acap |= pick(caps.ddp_ch, BLURAY_ACAP_DDPLUS_DEP_SURROUND,
                 BLURAY_ACAP_DDPLUS_DEP_STEREO_ONLY);
  acap |= pick(caps.ac3_ch, BLURAY_ACAP_DD_SURROUND, BLURAY_ACAP_DD_STEREO_ONLY);
  acap |= pick(combine(caps.dtshd_ch, caps.dts_ch), BLURAY_ACAP_DTSHD_CORE_SURROUND,
               BLURAY_ACAP_DTSHD_CORE_STEREO_ONLY);
  acap |= pick(caps.dtshd_ch, BLURAY_ACAP_DTSHD_EXT_SURROUND,
               BLURAY_ACAP_DTSHD_EXT_STEREO_ONLY);
  logM(LOGINFO, "CDVDInputStreamBluray: audio capability PSR15 0x{:04x} from HDMI sink", acap);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_AUDIO_CAP, acap);
}


bool CDVDInputStreamBluray::GetState(std::string& xmlstate)
{
  if (!m_bd || !m_titleInfo)
  {
    return false;
  }

  BlurayState blurayState;
  blurayState.playlistId = m_titleInfo->playlist;

  if (!m_blurayStateSerializer.BlurayStateToXML(xmlstate, blurayState))
  {
    CLog::LogF(LOGWARNING, "Failed to serialize Bluray state");
    return false;
  }

  return true;
}

bool CDVDInputStreamBluray::SetState(const std::string& xmlstate)
{
  if (!m_bd)
    return false;

  BlurayState blurayState;
  if (!m_blurayStateSerializer.XMLToBlurayState(blurayState, xmlstate))
  {
    CLog::LogF(LOGWARNING, "Failed to deserialize Bluray state");
    return false;
  }

  ReplaceTitleInfo(bd_get_playlist_info(m_bd, blurayState.playlistId, 0));
  if (!m_titleInfo)
  {
    CLog::LogF(LOGERROR, "Open - failed to get title info");
    return false;
  }

  UpdateGraphicsRegime();

  uint32_t playlist = 0;
  uint32_t idx = 0;
  {
    std::lock_guard<std::mutex> lock(m_clipTableMutex);
    if (!m_titleInfo)
      return false;

    playlist = m_titleInfo->playlist;
    idx = m_titleInfo->idx;
  }

  if (!bd_select_playlist(m_bd, playlist))
  {
    logM(LOGERROR, "failed to select playlist {}", idx);
    return false;
  }

  return true;
}
