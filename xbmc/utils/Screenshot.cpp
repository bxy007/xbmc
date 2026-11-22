/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Screenshot.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/LocalizeStrings.h"
#include "pictures/Picture.h"
#include "settings/SettingPath.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/windows/GUIControlSettings.h"
#include "utils/JobManager.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <cstdint>
#include <mutex>
#include <set>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
}

using namespace XFILE;

namespace
{
std::mutex g_screenshotNamesMutex;
std::multiset<std::string> g_screenshotNames;

bool IsPng(const std::string& filename)
{
  return StringUtils::EqualsNoCase(URIUtils::GetExtension(filename), ".png");
}

std::string ScreenshotNameKey(const std::string& filename)
{
  CURL url(CSpecialProtocol::TranslatePath(URIUtils::SubstitutePath(filename)));
  url.SetOptions("");
  url.SetProtocolOptions("");
  return URIUtils::GetRealPath(url.GetWithoutUserDetails());
}

class PendingScreenshot
{
public:
  PendingScreenshot(const std::string& path, bool numbered)
  {
    if (numbered)
    {
      const std::string pattern = URIUtils::AddFileToFolder(path, "screenshot{:05}.png");
      m_filename = CUtil::GetNextFilename(
          pattern, 65535, [this](const std::string& name) { return Reserve(name, true); });
    }
    else
    {
      Reserve(path, false);
      m_filename = path;
    }
  }

  ~PendingScreenshot()
  {
    std::lock_guard lock(g_screenshotNamesMutex);
    for (auto reservation : m_reservations)
      g_screenshotNames.erase(reservation);
  }

  PendingScreenshot(const PendingScreenshot&) = delete;
  PendingScreenshot& operator=(const PendingScreenshot&) = delete;

  const std::string& Filename() const { return m_filename; }
  const std::string& HdrFilename() const { return m_hdrFilename; }

private:
  bool Reserve(const std::string& name, bool numbered)
  {
    std::string companion = IsPng(name) ? URIUtils::ReplaceExtension(name, "-hdr.png") : "";
    const std::string key = ScreenshotNameKey(name);
    const std::string companionKey = ScreenshotNameKey(companion);
    std::lock_guard lock(g_screenshotNamesMutex);
    if (numbered && (g_screenshotNames.count(key) != 0 || CFile::Exists(name, false) ||
                     g_screenshotNames.count(companionKey) != 0 || CFile::Exists(companion, false)))
      return false;

    m_reservations.push_back(g_screenshotNames.insert(key));
    if (!companion.empty())
      m_reservations.push_back(g_screenshotNames.insert(companionKey));
    m_hdrFilename = std::move(companion);
    return true;
  }

  std::string m_filename;
  std::string m_hdrFilename;
  std::vector<std::multiset<std::string>::iterator> m_reservations;
};

bool WriteScreenshotPng(const IScreenshotSurface& surface, const std::string& filename, bool hdr)
{
  const auto& png = surface.GetPng();
  const int width = surface.GetWidth();
  const int height = surface.GetHeight();
  const bool highDepthSdr = !png.sdr.empty();
  constexpr int bits = 10;
  constexpr uint32_t maxCode = (1U << bits) - 1;
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
  if (!codec || width <= 0 || height <= 0)
    return false;

  const auto freeContext = [](AVCodecContext* context) { avcodec_free_context(&context); };
  const auto freeFrame = [](AVFrame* frame) { av_frame_free(&frame); };
  const auto freePacket = [](AVPacket* packet) { av_packet_free(&packet); };
  std::unique_ptr<AVCodecContext, decltype(freeContext)> context(avcodec_alloc_context3(codec),
                                                              freeContext);
  std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
  std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
  if (!context || !frame || !packet)
    return false;

  context->width = frame->width = width;
  context->height = frame->height = height;
  context->pix_fmt = AV_PIX_FMT_RGB48BE;
  context->bits_per_raw_sample = bits;
  context->compression_level = 3;
  context->thread_count = 1;
  context->time_base = {1, 1};
  frame->format = context->pix_fmt;
  frame->color_primaries = hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
  frame->color_range = AVCOL_RANGE_JPEG;
  frame->colorspace = AVCOL_SPC_RGB;
  frame->color_trc = AVCOL_TRC_IEC61966_2_1;
  if (hdr)
    frame->color_trc = png.hdrTransfer == ScreenshotPng::Transfer::HLG ? AVCOL_TRC_ARIB_STD_B67
                                                                      : AVCOL_TRC_SMPTE2084;

  if (avcodec_open2(context.get(), codec, nullptr) < 0 || av_frame_get_buffer(frame.get(), 0) < 0)
    return false;

  for (int y = 0; y < height; y++)
  {
    const auto pixel = static_cast<size_t>(y) * width;
    const uint8_t* bgra = surface.GetBuffer() + static_cast<size_t>(y) * surface.GetStride();
    uint8_t* out = frame->data[0] + static_cast<size_t>(y) * frame->linesize[0];
    for (int x = 0; x < width; x++, out += 6)
    {
      for (int c = 0; c < 3; c++)
      {
        uint32_t value;
        if (hdr)
          value = AV_RB16(png.hdr.data() + (pixel + x) * 3 + c);
        else if (highDepthSdr)
          value = png.sdr[(pixel + x) * 4 + c];
        else
          value = bgra[x * 4 + 2 - c] * 257U;

        // Quantize before scaling to PNG's supported 16-bit storage. Decoders can recover
        // the original code from the high bits using the encoder's sBIT chunk.
        const uint32_t sample = (value * maxCode + 32767U) / 65535U;
        AV_WB16(out + c * 2, (sample * 65535U + maxCode / 2) / maxCode);
      }
    }
  }

  if (avcodec_send_frame(context.get(), frame.get()) < 0 ||
      avcodec_receive_packet(context.get(), packet.get()) < 0)
    return false;

  CFile file;
  return file.OpenForWrite(filename, true) && file.Write(packet->data, packet->size) == packet->size;
}

void SaveScreenshot(const IScreenshotSurface& surface, const PendingScreenshot& output, bool numbered)
{
  // Stage beside the destination so publishing the closed file is a rename on the same filesystem.
  const std::string& filename = output.Filename();
  const std::string folder = URIUtils::GetDirectory(filename);
  const std::string staging =
      URIUtils::AddFileToFolder(folder, ".kodi-screenshot-" + StringUtils::CreateUUID());
  const std::string temporary =
      URIUtils::AddFileToFolder(staging, "capture" + URIUtils::GetExtension(filename));

  if (!CDirectory::Create(staging))
  {
    CLog::Log(LOGERROR, "Unable to create screenshot staging directory for {}",
              CURL::GetRedacted(filename));
    return;
  }

  const auto publish = [numbered](const std::string& temp, const std::string& name, bool written)
  {
    if (!written || (numbered && CFile::Exists(name, false)) || !CFile::Rename(temp, name))
    {
      CLog::Log(LOGERROR, "Unable to publish screenshot {}", CURL::GetRedacted(name));
      CFile::Delete(temp);
      return false;
    }
    CLog::Log(LOGDEBUG, "Published screenshot {}", CURL::GetRedacted(name));
    return true;
  };

  if (IsPng(filename))
  {
    if (publish(temporary, filename, WriteScreenshotPng(surface, temporary, false)) &&
        !surface.GetPng().hdr.empty())
    {
      const std::string hdrTemp = URIUtils::AddFileToFolder(staging, "capture-hdr.png");
      publish(hdrTemp, output.HdrFilename(), WriteScreenshotPng(surface, hdrTemp, true));
    }
  }
  else
  {
    publish(temporary, filename,
            CPicture::CreateThumbnailFromSurface(surface.GetBuffer(), surface.GetWidth(),
                                                 surface.GetHeight(), surface.GetStride(), temporary));
  }

  CDirectory::Remove(staging);
}
} // namespace

std::vector<std::function<std::unique_ptr<IScreenshotSurface>()>> CScreenShot::m_screenShotSurfaces;

void CScreenShot::Register(const std::function<std::unique_ptr<IScreenshotSurface>()>& createFunc)
{
  m_screenShotSurfaces.emplace_back(createFunc);
}

void CScreenShot::TakeScreenshot(const std::string& filename, bool sync)
{
  TakeScreenshot(filename, sync, false);
}

void CScreenShot::TakeScreenshotToDirectory(const std::string& directory, bool sync)
{
  TakeScreenshot(directory, sync, true);
}

void CScreenShot::TakeScreenshot(const std::string& path, bool sync, bool numbered)
{
  if (path.empty())
    return;

  auto output = std::make_unique<PendingScreenshot>(path, numbered);
  const std::string& filename = output->Filename();
  if (filename.empty())
  {
    CLog::Log(LOGWARNING, "Too many screenshots or invalid folder {}", CURL::GetRedacted(path));
    return;
  }

  auto surface = m_screenShotSurfaces.back()();

  if (!surface)
  {
    CLog::Log(LOGERROR, "failed to create screenshot surface");
    return;
  }

  const bool png = IsPng(filename);
  if (!(png ? surface->CapturePng() : surface->Capture()))
  {
    CLog::Log(LOGERROR, "Screenshot {} failed", CURL::GetRedacted(filename));
    return;
  }

  surface->CaptureVideo(true);

  CLog::Log(LOGDEBUG, "Saving screenshot {}", CURL::GetRedacted(filename));

  // The saved image is opaque.
  for (int y = 0; !png && y < surface->GetHeight(); y++)
  {
    unsigned char* row = surface->GetBuffer() + y * surface->GetStride();
    for (int x = 0; x < surface->GetWidth(); x++)
      row[x * 4 + 3] = 0xFF;
  }

  auto save = [surface = std::move(surface), output = std::move(output), numbered]
  { SaveScreenshot(*surface, *output, numbered); };
  if (sync)
    save();
  else
    CServiceBroker::GetJobManager()->Submit(std::move(save));
}

void CScreenShot::TakeScreenshot()
{
  std::shared_ptr<CSettingPath> screenshotSetting = std::static_pointer_cast<CSettingPath>(CServiceBroker::GetSettingsComponent()->GetSettings()->GetSetting(CSettings::SETTING_DEBUG_SCREENSHOTPATH));
  if (!screenshotSetting)
    return;

  std::string strDir = screenshotSetting->GetValue();
  if (strDir.empty())
  {
    if (!CGUIControlButtonSetting::GetPath(screenshotSetting, &g_localizeStrings))
      return;

    strDir = screenshotSetting->GetValue();
  }

  TakeScreenshotToDirectory(strDir, false);
}
