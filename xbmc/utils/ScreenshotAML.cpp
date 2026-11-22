/*
 *  Copyright (C) 2015-2018 Team Kodi
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/ScreenshotAML.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/DataCacheCore.h"
#include "platform/linux/SysfsPath.h"
#include "utils/AMLCaptureConvert.h"
#include "utils/AMLUtils.h"
#include "utils/Geometry.h"
#include "utils/IScreenshotSurface.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace
{

// Legacy /dev/amvideocap0 fallback definitions
#define AMVIDEOCAP_IOC_MAGIC 'V'
#define AMVIDEOCAP_IOW_SET_WANTFRAME_WIDTH _IOW(AMVIDEOCAP_IOC_MAGIC, 0x02, int)
#define AMVIDEOCAP_IOW_SET_WANTFRAME_HEIGHT _IOW(AMVIDEOCAP_IOC_MAGIC, 0x03, int)
#define AMVIDEOCAP_IOW_SET_WANTFRAME_AT_FLAGS _IOW(AMVIDEOCAP_IOC_MAGIC, 0x06, int)
constexpr unsigned long LEGACY_CAPTURE_AT_CURRENT = 0;
constexpr const char* LEGACY_CAPTURE_DEVICEPATH = "/dev/amvideocap0";
constexpr int LEGACY_CAPTURE_MAX_WIDTH = 3840;
constexpr int LEGACY_CAPTURE_MAX_HEIGHT = 2160;

// /dev/vdin1 single-frame 10-bit 4:2:2 Full-Pack snapshot ABI (matches kernel tvin.h)
constexpr const char* VDIN1_DEVICEPATH = "/dev/vdin1";
constexpr uint32_t VDIN_SNAP_PORT_VD1 = 0;
constexpr uint32_t VDIN_SNAP_FLAG_RAW = (1U << 0);
constexpr uint32_t VDIN_SNAP_FLAG_COSITED_CHROMA = (1U << 1);
constexpr uint32_t VDIN_SNAP_FLAG_INTERLACED = (1U << 2);

struct VdinSnapshotReq
{
  uint32_t port;
  uint32_t width;
  uint32_t height;
  uint32_t flags;
  uint32_t src_width;
  uint32_t src_height;
  uint32_t stride;
  uint32_t bytes_copied;
  uint64_t dst_ptr;
  uint64_t dst_size;
};
static_assert(sizeof(VdinSnapshotReq) == 48, "VdinSnapshotReq ABI size mismatch");

#define TVIN_IOC_CAPTURE_SNAPSHOT _IOWR('T', 0x53, VdinSnapshotReq)

constexpr float MAPPED_PEAK_NITS = 1000.0f;
constexpr float TVLED_PEAK_NITS = 4000.0f;
constexpr float HLG_PEAK_NITS = 1000.0f;
constexpr float PQ_PEAK_NITS = 10000.0f;
constexpr float CROP_END_EPSILON = 0.01f;
constexpr int FULL_SPAN_8 = 255;
constexpr int CODE_SPAN_8_TO_16 = 257;
constexpr float SDR_REFERENCE_WHITE_NITS = 100.0f;
constexpr float HLG_REFERENCE_WHITE_NITS = 203.0f;

std::mutex g_vdinCaptureMutex;

struct VdinSnapshot
{
  std::vector<uint8_t> data;
  int width{0};
  int height{0};
  int srcWidth{0};
  int srcHeight{0};
  int stride{0};
  bool cositedChroma{false};
  bool interlaced{false};
};

double PQCodeToNits(uint16_t pq12)
{
  if (pq12 == 0)
    return 0.0;
  constexpr double m1 = 2610.0 / 16384.0;
  constexpr double m2 = 2523.0 / 4096.0 * 128.0;
  constexpr double c1 = 3424.0 / 4096.0;
  constexpr double c2 = 2413.0 / 4096.0 * 32.0;
  constexpr double c3 = 2392.0 / 4096.0 * 32.0;
  const double e = std::clamp(static_cast<double>(pq12) / 4095.0, 0.0, 1.0);
  const double p = std::pow(e, 1.0 / m2);
  const double lin = std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
  return lin * 10000.0;
}

float GetTitlePeakNits(float fallback)
{
  auto& dataCache = CDataCacheCore::GetInstance();
  const HDRStaticMetadataInfo hdr = dataCache.GetVideoHDRStaticMetadataInfo();
  if (hdr.has_cll_metadata && hdr.max_cll > 0)
    return std::clamp(static_cast<float>(hdr.max_cll), 100.0f, 10000.0f);
  if (hdr.has_mdcv_metadata && hdr.max_lum > 0)
    return std::clamp(static_cast<float>(hdr.max_lum), 100.0f, 10000.0f);

  const DOVIStreamMetadata dovi = dataCache.GetVideoDoViStreamMetadata();
  if (dovi.has_level6_metadata && dovi.level6_max_cll > 0)
    return std::clamp(static_cast<float>(dovi.level6_max_cll), 100.0f, 10000.0f);
  if (dovi.has_level6_metadata && dovi.level6_max_lum > 0)
    return std::clamp(static_cast<float>(dovi.level6_max_lum), 100.0f, 10000.0f);
  if (dovi.source_max_pq > 0)
  {
    const double nits = PQCodeToNits(dovi.source_max_pq);
    if (nits >= 100.0)
      return std::clamp(static_cast<float>(nits), 100.0f, 10000.0f);
  }
  return fallback;
}

const char* SignalName(AMLCaptureSignal signal)
{
  switch (signal)
  {
    case AMLCaptureSignal::SDR:
      return "SDR";
    case AMLCaptureSignal::PQ:
      return "PQ";
    case AMLCaptureSignal::HLG:
      return "HLG";
    case AMLCaptureSignal::DV_TV_LED:
      return "DV_TV_LED";
    case AMLCaptureSignal::DV_LL:
      return "DV_LL";
    case AMLCaptureSignal::DV_HDR10:
      return "DV_HDR10";
    case AMLCaptureSignal::DV_SDR:
      return "DV_SDR";
    default:
      return "UNKNOWN";
  }
}

AMLCaptureColor DetectCaptureColor()
{
  AMLCaptureColor color;
  auto& dataCache = CDataCacheCore::GetInstance();
  const StreamHdrType sourceHdr = dataCache.GetVideoSourceHdrType() != StreamHdrType::HDR_TYPE_NONE
                                      ? dataCache.GetVideoSourceHdrType()
                                      : dataCache.GetVideoHdrType();

  const bool dvEnable = aml_is_dv_enable();
  const unsigned int dvStatus =
      CSysfsPath{"/sys/module/amdolby_vision/parameters/dolby_vision_status"}
          .Get<unsigned int>()
          .value_or(0);
  const unsigned int dvMode = aml_dv_dolby_vision_mode();

  if (dvEnable && dvStatus > 0 && dvMode != DOLBY_VISION_OUTPUT_MODE_BYPASS)
  {
    const unsigned int llPolicy =
        CSysfsPath{"/sys/module/amdolby_vision/parameters/dolby_vision_ll_policy"}
            .Get<unsigned int>()
            .value_or(0);
    const unsigned int dvFlags =
        CSysfsPath{"/sys/module/amdolby_vision/parameters/dolby_vision_flags"}
            .Get<unsigned int>()
            .value_or(0);

    switch (dvMode)
    {
      case DOLBY_VISION_OUTPUT_MODE_IPT:
      case DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL:
        if (llPolicy >= DOLBY_VISION_LL_YUV422 || (dvFlags & FLAG_FORCE_DOVI_LL))
        {
          color.signal = AMLCaptureSignal::DV_LL;
          color.displayMapped = true;
          color.luminance = MAPPED_PEAK_NITS;
        }
        else
        {
          color.signal = AMLCaptureSignal::DV_TV_LED;
          color.displayMapped = sourceHdr != StreamHdrType::HDR_TYPE_DOLBYVISION;
          color.luminance = color.displayMapped ? MAPPED_PEAK_NITS : GetTitlePeakNits(TVLED_PEAK_NITS);
        }
        return color;

      case DOLBY_VISION_OUTPUT_MODE_HDR10:
        color.signal = AMLCaptureSignal::DV_HDR10;
        color.displayMapped = true;
        color.luminance = MAPPED_PEAK_NITS;
        return color;

      case DOLBY_VISION_OUTPUT_MODE_SDR10:
      case 4: // DOLBY_VISION_OUTPUT_MODE_SDR8
        color.signal = AMLCaptureSignal::DV_SDR;
        color.displayMapped = true;
        color.luminance = 0.0f;
        return color;

      default:
        break;
    }
  }

  const std::string hdrStatus =
      CSysfsPath{"/sys/class/amhdmitx/amhdmitx0/hdmi_hdr_status"}.Get<std::string>().value_or("");

  if (hdrStatus.find("HLG") != std::string::npos)
  {
    color.signal = AMLCaptureSignal::HLG;
    color.luminance = HLG_PEAK_NITS;
    return color;
  }
  if (hdrStatus.find("HDR10") != std::string::npos || hdrStatus.find("ST2084") != std::string::npos)
  {
    color.signal = AMLCaptureSignal::PQ;
    color.displayMapped = sourceHdr == StreamHdrType::HDR_TYPE_NONE;
    color.luminance = color.displayMapped ? MAPPED_PEAK_NITS : GetTitlePeakNits(PQ_PEAK_NITS);
    return color;
  }
  if (hdrStatus.find("SDR") != std::string::npos)
  {
    color.signal = AMLCaptureSignal::SDR;
    color.luminance = 0.0f;
    return color;
  }

  // Fallback to stream HDR metadata when hdmi_hdr_status is unavailable
  switch (dataCache.GetVideoHdrType())
  {
    case StreamHdrType::HDR_TYPE_HLG:
      color.signal = AMLCaptureSignal::HLG;
      color.luminance = HLG_PEAK_NITS;
      break;
    case StreamHdrType::HDR_TYPE_HDR10:
    case StreamHdrType::HDR_TYPE_HDR10PLUS:
    case StreamHdrType::HDR_TYPE_DOLBYVISION:
      color.signal = AMLCaptureSignal::PQ;
      color.luminance = GetTitlePeakNits(PQ_PEAK_NITS);
      break;
    default:
      color.signal = AMLCaptureSignal::SDR;
      color.luminance = 0.0f;
      break;
  }
  return color;
}

bool IsHwVideoVisible()
{
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (!appPlayer || !appPlayer->IsPlayingVideo())
    return false;

  if (!CDataCacheCore::GetInstance().IsVideoHwDecoder())
    return false;

  const int disabled = CSysfsPath{"/sys/class/video/disable_video"}.Get<int>().value_or(1);
  return disabled == 0;
}

CRect GetNormalizedVideoRect(int srcWidth, int srcHeight, bool interlaced)
{
  if (srcWidth <= 0 || srcHeight <= 0)
    return CRect(0.0f, 0.0f, 1.0f, 1.0f);

  const std::string axis = CSysfsPath{"/sys/class/video/axis"}.Get<std::string>().value_or("");
  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
  if (std::sscanf(axis.c_str(), "%d %d %d %d", &x1, &y1, &x2, &y2) != 4 || x2 <= x1 || y2 <= y1)
    return CRect(0.0f, 0.0f, 1.0f, 1.0f);

  const float fullW = static_cast<float>(srcWidth);
  const float fullH = static_cast<float>(interlaced ? srcHeight * 2 : srcHeight);
  const float nx1 = std::clamp(static_cast<float>(x1) / fullW, 0.0f, 1.0f);
  const float ny1 = std::clamp(static_cast<float>(y1) / fullH, 0.0f, 1.0f);
  const float nx2 = std::clamp(static_cast<float>(x2 + 1) / fullW, nx1, 1.0f);
  const float ny2 = std::clamp(static_cast<float>(y2 + 1) / fullH, ny1, 1.0f);
  if (nx2 - nx1 <= 0.001f || ny2 - ny1 <= 0.001f)
    return CRect(0.0f, 0.0f, 1.0f, 1.0f);
  return CRect(nx1, ny1, nx2, ny2);
}

AMLCaptureImage CropFullPack(const AMLCaptureImage& image, const CRect& rect)
{
  if (rect.IsEmpty())
    return image;

  const int x1 = std::clamp(static_cast<int>(rect.x1 * image.width) & ~1, 0,
                            std::max(image.width - 2, 0));
  const int y1 = std::clamp(static_cast<int>(rect.y1 * image.height), 0,
                            std::max(image.height - 1, 0));
  const int x2 = std::clamp(
      (static_cast<int>(std::ceil(rect.x2 * image.width - CROP_END_EPSILON)) + 1) & ~1,
      std::min(x1 + 2, image.width), image.width);
  const int y2 = std::clamp(
      static_cast<int>(std::ceil(rect.y2 * image.height - CROP_END_EPSILON)),
      std::min(y1 + 1, image.height), image.height);

  AMLCaptureImage cropped = image;
  cropped.width = x2 - x1;
  cropped.height = y2 - y1;
  cropped.data[0] = image.data[0] + static_cast<std::ptrdiff_t>(y1) * image.linesize[0] +
                    (x1 / 2) * AML_FULL_PACK_PAIR_BYTES;
  return cropped;
}

int Edge(float v, int pixels)
{
  return std::clamp(static_cast<int>(std::lround(v * pixels)), 0, pixels);
}

AMLCaptureImage OffsetImage(AMLCaptureImage image, int x, int y, int pixelBytes)
{
  image.data[0] += static_cast<std::ptrdiff_t>(y) * image.linesize[0] + x * pixelBytes;
  return image;
}

template<typename Fill>
void FillAround(int width, int height, int x1, int y1, int w, int h, const Fill& fill)
{
  for (int y = 0; y < height; y++)
  {
    if (y < y1 || y >= y1 + h)
      fill(y, 0, width);
    else
    {
      fill(y, 0, x1);
      fill(y, x1 + w, width);
    }
  }
}

bool CaptureVdin1Snapshot(uint32_t port, int reqWidth, int reqHeight, VdinSnapshot& out)
{
  std::lock_guard<std::mutex> lock(g_vdinCaptureMutex);

  const int fd = open(VDIN1_DEVICEPATH, O_RDWR | O_CLOEXEC);
  if (fd < 0)
  {
    CLog::Log(LOGDEBUG, "CScreenshotAML: open {} failed ({})", VDIN1_DEVICEPATH, strerror(errno));
    return false;
  }

  const size_t estW = static_cast<size_t>(std::max(reqWidth, 1920));
  const size_t estH = static_cast<size_t>(std::max(reqHeight, 1080));
  std::vector<uint8_t> buf(((estW + 1) / 2) * AML_FULL_PACK_PAIR_BYTES * estH);

  VdinSnapshotReq req{};
  int rc = -1;
  for (int attempt = 0; attempt < 2; attempt++)
  {
    req = {};
    req.port = port;
    req.width = reqWidth > 0 ? static_cast<uint32_t>(reqWidth) : 0;
    req.height = reqHeight > 0 ? static_cast<uint32_t>(reqHeight) : 0;
    req.flags = VDIN_SNAP_FLAG_RAW;
    req.dst_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buf.data()));
    req.dst_size = static_cast<uint64_t>(buf.size());

    rc = ioctl(fd, TVIN_IOC_CAPTURE_SNAPSHOT, &req);
    if (rc == 0)
      break;
    if (errno == ENOSPC && req.bytes_copied > buf.size() && req.bytes_copied <= 64u * 1024u * 1024u)
    {
      buf.resize(req.bytes_copied);
      continue;
    }
    break;
  }

  const int err = errno;
  close(fd);

  if (rc != 0 || req.width == 0 || req.height == 0 || req.stride == 0 ||
      req.bytes_copied < req.stride * req.height)
  {
    CLog::Log(LOGWARNING, "CScreenshotAML: vdin1 snapshot ioctl failed (rc={} errno={} {})", rc, err,
              strerror(err));
    return false;
  }

  buf.resize(req.bytes_copied);
  out.data = std::move(buf);
  out.width = static_cast<int>(req.width);
  out.height = static_cast<int>(req.height);
  out.srcWidth = static_cast<int>(req.src_width);
  out.srcHeight = static_cast<int>(req.src_height);
  out.stride = static_cast<int>(req.stride);
  out.cositedChroma = (req.flags & VDIN_SNAP_FLAG_COSITED_CHROMA) != 0;
  out.interlaced = (req.flags & VDIN_SNAP_FLAG_INTERLACED) != 0;
  return true;
}

void UndoLimitedRange(uint8_t* bgra, int width, int height)
{
  const int pixels = width * height;
  for (int i = 0; i < pixels; i++, bgra += 4)
  {
    const int a = bgra[3];
    if (a == 0)
      continue;
    const float offset = (16.0f / 255.0f) * static_cast<float>(a);
    constexpr float scale = 255.0f / 219.0f;
    for (int c = 0; c < 3; c++)
    {
      const float v = (static_cast<float>(bgra[c]) - offset) * scale;
      bgra[c] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, static_cast<long>(a)));
    }
  }
}

void UndoHlgGuiAdjust(uint8_t* bgra, int width, int height, float gain, float sat)
{
  const bool invertGain = gain > 1e-4f && std::fabs(gain - 1.0f) > 0.001f;
  const bool invertSat = sat > 0.05f && std::fabs(sat - 1.0f) > 0.001f;
  if (!invertGain && !invertSat)
    return;

  const int pixels = width * height;
  for (int i = 0; i < pixels; i++, bgra += 4)
  {
    const int a = bgra[3];
    if (a == 0)
      continue;

    const float invA = 1.0f / static_cast<float>(a);
    float rgb[3] = {
        std::clamp(static_cast<float>(bgra[2]) * invA, 0.0f, 1.0f),
        std::clamp(static_cast<float>(bgra[1]) * invA, 0.0f, 1.0f),
        std::clamp(static_cast<float>(bgra[0]) * invA, 0.0f, 1.0f),
    };

    for (float& c : rgb)
      c = std::pow(std::max(c, 0.0f), 1.0f / 0.45f);

    if (invertGain)
    {
      for (float& c : rgb)
      {
        if (gain > 1.0f)
          c = c / std::max(gain - c * (gain - 1.0f), 1e-4f);
        else
          c = c / gain;
        c = std::clamp(c, 0.0f, 1.0f);
      }
    }

    if (invertSat)
    {
      const float luma = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
      for (float& c : rgb)
        c = std::clamp(luma + (c - luma) / sat, 0.0f, 1.0f);
    }

    for (float& c : rgb)
      c = std::pow(std::max(c, 0.0f), 0.45f);

    bgra[2] = static_cast<uint8_t>(std::clamp(std::lround(rgb[0] * a), 0L, static_cast<long>(a)));
    bgra[1] = static_cast<uint8_t>(std::clamp(std::lround(rgb[1] * a), 0L, static_cast<long>(a)));
    bgra[0] = static_cast<uint8_t>(std::clamp(std::lround(rgb[2] * a), 0L, static_cast<long>(a)));
  }
}

void ConvertPqGuiToSdr(unsigned char* buffer, int width, int height, const AMLCaptureCoding& coding)
{
  CAMLCaptureConvert convert;
  const AMLCaptureImage comp = convert.GetCompositeImage(width, height);
  auto* compData = reinterpret_cast<uint16_t*>(comp.data[0]);
  const size_t pixels = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < pixels; i++)
  {
    const uint8_t* bgra = buffer + i * 4;
    uint16_t* rgba = compData + i * 4;
    const unsigned int alpha = bgra[3];
    for (int c = 0; c < 3; c++)
    {
      // Remove premultiplication before the nonlinear conversion.
      rgba[c] = alpha ? (std::min<unsigned int>(bgra[2 - c], alpha) * 65535 + alpha / 2) / alpha
                      : 0;
    }
    rgba[3] = alpha * CODE_SPAN_8_TO_16;
  }

  const AMLCaptureImage dst = AMLCaptureImage::Packed(AV_PIX_FMT_BGRA, width, height, buffer, width * 4);
  convert.MapToSdr(comp, coding, dst, nullptr, coding.whiteNits);

  for (size_t i = 0; i < pixels; i++)
  {
    uint8_t* bgra = buffer + i * 4;
    const unsigned int alpha = compData[i * 4 + 3] / CODE_SPAN_8_TO_16;
    for (int c = 0; c < 3; c++)
      bgra[c] = (bgra[c] * alpha + FULL_SPAN_8 / 2) / FULL_SPAN_8;
    bgra[3] = alpha;
  }
}

bool LegacyCaptureVideoFrame(unsigned char* buffer, int iWidth, int iHeight, bool bBlendToBuffer)
{
  if (!buffer || iWidth <= 0 || iWidth > LEGACY_CAPTURE_MAX_WIDTH || iHeight <= 0 ||
      iHeight > LEGACY_CAPTURE_MAX_HEIGHT)
    return false;

  const int captureFd = open(LEGACY_CAPTURE_DEVICEPATH, O_RDWR | O_CLOEXEC, 0);
  if (captureFd < 0)
    return false;

  const int stride = ((iWidth + 31) & ~31) * 3;
  const int buffSize = stride * iHeight;
  std::vector<unsigned char> videoBuffer(static_cast<size_t>(buffSize));

  ssize_t readSize = -1;
  if (ioctl(captureFd, AMVIDEOCAP_IOW_SET_WANTFRAME_AT_FLAGS, LEGACY_CAPTURE_AT_CURRENT) == 0 &&
      ioctl(captureFd, AMVIDEOCAP_IOW_SET_WANTFRAME_WIDTH, stride / 3) == 0 &&
      ioctl(captureFd, AMVIDEOCAP_IOW_SET_WANTFRAME_HEIGHT, iHeight) == 0)
    readSize = pread(captureFd, videoBuffer.data(), buffSize, 0);
  close(captureFd);

  if (readSize != buffSize)
    return false;

  unsigned char* dst = buffer;
  for (int y = 0; y < iHeight; ++y)
  {
    const unsigned char* videoPtr = videoBuffer.data() + static_cast<size_t>(y) * stride;
    for (int x = 0; x < iWidth; ++x, dst += 4, videoPtr += 3)
    {
      if (bBlendToBuffer)
      {
        const int alpha = dst[3];
        const int invAlpha = FULL_SPAN_8 - alpha;
        for (int c = 0; c < 3; c++)
          dst[c] = std::min(dst[c] + invAlpha * videoPtr[c] / FULL_SPAN_8, FULL_SPAN_8);
      }
      else
      {
        std::memcpy(dst, videoPtr, 3);
      }
      dst[3] = 0xff;
    }
  }
  return true;
}

} // namespace

bool CScreenshotAML::CaptureVideoFrame(unsigned char* buffer, int iWidth, int iHeight)
{
  // RenderCapture can run every frame. Use the current-frame device here for bounded capture.
  return LegacyCaptureVideoFrame(buffer, iWidth, iHeight, false);
}

void CScreenshotAML::CompositeScreenshot(unsigned char* guiBuffer,
                                         int iWidth,
                                         int iHeight,
                                         const AMLScreenshotGuiState& guiState,
                                         ScreenshotPng* png)
{
  if (!guiBuffer || iWidth <= 0 || iHeight <= 0)
    return;

  if (guiState.limitedColor)
    UndoLimitedRange(guiBuffer, iWidth, iHeight);

  if (guiState.guiHdr == GuiHdr::HDR)
    UndoHlgGuiAdjust(guiBuffer, iWidth, iHeight, guiState.sdrPeakLuminance, guiState.sdrSaturation);

  const bool guiPq = guiState.guiHdr == GuiHdr::HDR_PQ;
  AMLCaptureCoding coding;
  if (guiPq)
  {
    coding.transfer = AMLCaptureCoding::Transfer::PQ;
    const float nits = guiState.sdrPeakLuminance * 100.0f;
    coding.whiteNits = std::clamp(nits > 0.0f ? nits : 100.0f, 30.0f, 10000.0f);
  }

  if (!IsHwVideoVisible())
  {
    if (guiPq)
      ConvertPqGuiToSdr(guiBuffer, iWidth, iHeight, coding);
    CLog::Log(LOGINFO, "CScreenshotAML: GUI-only screenshot {}x{} (guiHdr={})", iWidth, iHeight,
              static_cast<int>(guiState.guiHdr));
    return;
  }

  const auto captureFallback = [&]()
  {
    if (png)
      *png = {};
    if (guiPq)
      ConvertPqGuiToSdr(guiBuffer, iWidth, iHeight, coding);
    if (!LegacyCaptureVideoFrame(guiBuffer, iWidth, iHeight, true))
      CLog::Log(LOGWARNING, "CScreenshotAML: amvideocap0 frame unavailable, saving GUI only");
  };

  VdinSnapshot snap;
  if (!CaptureVdin1Snapshot(VDIN_SNAP_PORT_VD1, iWidth, iHeight, snap))
  {
    CLog::Log(LOGWARNING, "CScreenshotAML: vdin1 unavailable, falling back to amvideocap0");
    captureFallback();
    return;
  }

  const AMLCaptureColor color = DetectCaptureColor();
  const CRect videoRect = GetNormalizedVideoRect(snap.srcWidth, snap.srcHeight, snap.interlaced);
  CLog::Log(LOGINFO,
            "CScreenshotAML: vdin1 VD1 {}x{} (src={}x{} interlaced={} cosited={}) "
            "signal={} lum={:.0f} guiHdr={} whiteNits={:.0f} rect=({:.3f},{:.3f})-({:.3f},{:.3f})",
            snap.width, snap.height, snap.srcWidth, snap.srcHeight, snap.interlaced,
            snap.cositedChroma, SignalName(color.signal), color.luminance,
            static_cast<int>(guiState.guiHdr), coding.whiteNits, videoRect.x1, videoRect.y1,
            videoRect.x2, videoRect.y2);

  AMLCaptureImage src =
      AMLCaptureImage::Packed(AV_PIX_FMT_NONE, snap.width, snap.height, snap.data.data(), snap.stride);
  src.fullPack = true;
  src.cositedChroma = snap.cositedChroma;

  CAMLCaptureConvert convert;
  const size_t pixels = static_cast<size_t>(iWidth) * iHeight;
  AMLCaptureImage sdr16;
  if (png)
  {
    png->sdr.resize(pixels * 4);
    sdr16 = AMLCaptureImage::Packed(AV_PIX_FMT_RGBA64LE, iWidth, iHeight,
                                   reinterpret_cast<uint8_t*>(png->sdr.data()), iWidth * 8);
  }
  const AMLCaptureImage comp = png && !guiPq ? sdr16 : convert.GetCompositeImage(iWidth, iHeight);
  uint16_t* composite = reinterpret_cast<uint16_t*>(comp.data[0]);

  const AMLCaptureImage overlay =
      AMLCaptureImage::Packed(AV_PIX_FMT_BGRA, iWidth, iHeight, guiBuffer, iWidth * 4);
  std::vector<uint8_t> outBuf;
  AMLCaptureImage outDst;
  if (guiPq)
  {
    if (png)
      outDst = sdr16;
    else
    {
      outBuf.resize(pixels * 4);
      outDst = AMLCaptureImage::Packed(AV_PIX_FMT_BGRA, iWidth, iHeight, outBuf.data(), iWidth * 4);
    }
  }

  const int x1 = Edge(videoRect.x1, iWidth);
  const int y1 = Edge(videoRect.y1, iHeight);
  AMLCaptureImage place = OffsetImage(comp, x1, y1, 8);
  place.width = Edge(videoRect.x2, iWidth) - x1;
  place.height = Edge(videoRect.y2, iHeight) - y1;

  const AMLCaptureImage overlayPlace = OffsetImage(overlay, x1, y1, 4);
  const AMLCaptureImage dstPlace = guiPq ? OffsetImage(outDst, x1, y1, png ? 8 : 4) : AMLCaptureImage{};

  if (place.width > 0 && place.height > 0)
  {
    const AMLCaptureCoding::Transfer transfer = CAMLCaptureConvert::GetNativeTransfer(color.signal);
    const bool captureHdr = png && transfer != AMLCaptureCoding::Transfer::SDR;
    AMLCaptureImage hdr;
    AMLCaptureImage hdrPlace;
    if (captureHdr)
    {
      png->hdr.resize(pixels * 3);
      png->hdrTransfer = transfer == AMLCaptureCoding::Transfer::HLG ? ScreenshotPng::Transfer::HLG
                                                                    : ScreenshotPng::Transfer::PQ;
      hdr = AMLCaptureImage::Packed(AV_PIX_FMT_RGB48BE, iWidth, iHeight,
                                    reinterpret_cast<uint8_t*>(png->hdr.data()), iWidth * 6);
      hdrPlace = OffsetImage(hdr, x1, y1, 6);
      hdrPlace.width = place.width;
      hdrPlace.height = place.height;
    }

    const AMLCaptureImage cropped =
        CropFullPack(src, CRect(0.0f, 0.0f, videoRect.Width(), videoRect.Height()));
    if (!convert.Convert(cropped, color, coding, place, guiPq ? &overlayPlace : nullptr,
                         guiPq ? &dstPlace : nullptr, captureHdr ? &hdrPlace : nullptr))
    {
      CLog::Log(LOGWARNING, "CScreenshotAML: CAMLCaptureConvert::Convert failed");
      captureFallback();
      return;
    }
    if (captureHdr)
    {
      AMLCaptureCoding hdrGui = coding;
      if (!guiPq)
        hdrGui.whiteNits = transfer == AMLCaptureCoding::Transfer::HLG ? HLG_REFERENCE_WHITE_NITS
                                                                       : SDR_REFERENCE_WHITE_NITS;
      CAMLCaptureConvert::CompositeHdrGui(hdr, overlay, hdrGui, transfer);
    }
  }

  FillAround(iWidth, iHeight, x1, y1, place.width, place.height,
             [&](int y, int from, int to)
             {
               const size_t first = static_cast<size_t>(y) * iWidth + from;
               std::fill(composite + first * 4, composite + (first + to - from) * 4, 0);
               if (guiPq && !png)
               {
                 uint8_t* out = outBuf.data() + first * 4;
                 for (int x = from; x < to; x++, out += 4)
                 {
                   out[0] = 0;
                   out[1] = 0;
                   out[2] = 0;
                   out[3] = 255;
                 }
               }
             });

  for (int y = 0; y < iHeight; y++)
  {
    const uint8_t* gui = guiBuffer + static_cast<size_t>(y) * iWidth * 4;
    uint16_t* out = composite + static_cast<size_t>(y) * iWidth * 4;
    for (int x = 0; x < iWidth; x++, gui += 4, out += 4)
    {
      if (guiPq && CAMLCaptureConvert::IsClear(gui))
        continue;
      const int alpha = gui[3];
      const int channel[3] = {gui[2], gui[1], gui[0]}; // BGRA -> R, G, B
      for (int c = 0; c < 3; c++)
      {
        const int value =
            channel[c] * CODE_SPAN_8_TO_16 + (FULL_SPAN_8 - alpha) * out[c] / FULL_SPAN_8;
        out[c] = static_cast<uint16_t>(std::min(value, 65535));
      }
      out[3] = 65535;
    }
  }

  if (!guiPq)
  {
    if (png)
      return;
    uint8_t* dst = guiBuffer;
    const uint16_t* in = composite;
    for (size_t i = 0; i < pixels; i++, in += 4, dst += 4)
    {
      dst[0] = static_cast<uint8_t>((in[2] + CODE_SPAN_8_TO_16 / 2) / CODE_SPAN_8_TO_16); // B
      dst[1] = static_cast<uint8_t>((in[1] + CODE_SPAN_8_TO_16 / 2) / CODE_SPAN_8_TO_16); // G
      dst[2] = static_cast<uint8_t>((in[0] + CODE_SPAN_8_TO_16 / 2) / CODE_SPAN_8_TO_16); // R
      dst[3] = 255;
    }
    return;
  }

  convert.MapToSdr(comp, coding, outDst, &overlay, color.luminance);
  if (!png)
    std::memcpy(guiBuffer, outBuf.data(), outBuf.size());
}
