/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define BDJ_COLOR_HAS_NEON 1
#else
#define BDJ_COLOR_HAS_NEON 0
#endif

namespace BDJ_COLOR
{

enum class BakeMode : uint8_t
{
  NONE = 0,
  PASSTHROUGH_SDR = 1,
  HDR_PGS_SDR_OUT = 2,
  HDR_PGS_PQ_OUT = 3,
  SDR_SUBS_PQ_OUT = 4,
  SDR_GUI_HDR_OUT = 5,
};

struct BakeKey
{
  BakeMode mode{BakeMode::NONE};
  bool limitedRange{false};
  bool srgbDecode{false};
  float peak{1.0f};
  float saturation{1.0f};

  bool operator==(const BakeKey& o) const
  {
    return mode == o.mode && limitedRange == o.limitedRange && srgbDecode == o.srgbDecode &&
           peak == o.peak && saturation == o.saturation;
  }

  bool operator!=(const BakeKey& o) const { return !(*this == o); }
};

inline uint32_t PremultiplyArgbPixel(uint32_t px)
{
  const uint32_t a = px >> 24;
  if (a == 255u)
    return px;
  if (a == 0u)
    return 0u;

  const uint32_t r = ((px >> 16) & 0xFFu) * a;
  const uint32_t g = ((px >> 8) & 0xFFu) * a;
  const uint32_t b = (px & 0xFFu) * a;
  return (a << 24) | (((r + 1u + (r >> 8)) >> 8) << 16) | (((g + 1u + (g >> 8)) >> 8) << 8) |
         ((b + 1u + (b >> 8)) >> 8);
}

struct ColorLuts
{
  static constexpr uint32_t kMinExp = 97u;  // 2^-30 (~9.3e-10)
  static constexpr uint32_t kMaxExp = 127u; // 2^0   (1.0)
  static constexpr uint32_t kMinBucket = kMinExp << 7;
  static constexpr uint32_t kLutSize = (kMaxExp - kMinExp) * 128u + 1u; // 3841 bytes
  static constexpr uint32_t kSqrtLutSize = 1025u;                       // 1 KB L1 LUT for pow045

  std::array<float, 256> pqDecode{};
  std::array<float, 256> pqDecode100{};
  std::array<float, 256> srgbDecode{};
  std::array<float, 256> gammaInv045{};
  std::array<uint8_t, 256> limitedRange{};
  std::array<uint8_t, kLutSize> pow045{};
  std::array<uint8_t, kLutSize> pqEncode{};
  std::array<uint8_t, kSqrtLutSize> sqrtPow045{};

  static double EvalDecodePQ(double x)
  {
    constexpr double m1 = 2610.0 / (4096.0 * 4.0);
    constexpr double m2 = (2523.0 / 4096.0) * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = (2413.0 / 4096.0) * 32.0;
    constexpr double c3 = (2392.0 / 4096.0) * 32.0;

    x = std::clamp(x, 0.0, 1.0);
    const double p = std::pow(std::max(x, 1e-10), 1.0 / m2);
    const double num = std::max(p - c1, 0.0);
    const double den = std::max(c2 - c3 * p, 1e-6);
    return std::pow(std::max(num / den, 1e-10), 1.0 / m1);
  }

  static double EvalEncodePQ(double x)
  {
    constexpr double m1 = 2610.0 / (4096.0 * 4.0);
    constexpr double m2 = (2523.0 / 4096.0) * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = (2413.0 / 4096.0) * 32.0;
    constexpr double c3 = (2392.0 / 4096.0) * 32.0;

    const double p = std::pow(std::max(x, 1e-10), m1);
    const double y = (c1 + c2 * p) / (1.0 + c3 * p);
    return std::clamp(std::pow(y, m2), 0.0, 1.0);
  }

  static uint8_t Quantize8(double v)
  {
    const double clamped = std::clamp(v, 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(clamped * 255.0));
  }

  ColorLuts()
  {
    for (int i = 0; i < 256; ++i)
    {
      const double x = static_cast<double>(i) / 255.0;
      const double pqLin = EvalDecodePQ(x);
      pqDecode[i] = static_cast<float>(pqLin);
      pqDecode100[i] = static_cast<float>(pqLin * 100.0);
      srgbDecode[i] = static_cast<float>(
          x < 0.04045 ? (x / 12.92) : std::pow(std::max((x + 0.055) / 1.055, 1e-10), 2.4));
      gammaInv045[i] = static_cast<float>(std::pow(std::max(x, 1e-10), 1.0 / 0.45));
      limitedRange[i] = Quantize8(x * ((235.0 - 16.0) / 255.0) + (16.0 / 255.0));
    }

    for (uint32_t idx = 0; idx < kLutSize - 1u; ++idx)
    {
      const uint32_t bits = ((kMinBucket + idx) << 16) | 0x8000u;
      float fval = 0.0f;
      std::memcpy(&fval, &bits, sizeof(fval));
      const double x = static_cast<double>(fval);
      pow045[idx] = Quantize8(std::pow(std::max(x, 1e-10), 0.45));
      pqEncode[idx] = Quantize8(EvalEncodePQ(x));
    }
    pow045[kLutSize - 1u] = 255u;
    pqEncode[kLutSize - 1u] = 255u;

    sqrtPow045[0] = 0u;
    for (uint32_t i = 1u; i < kSqrtLutSize; ++i)
    {
      const double s = static_cast<double>(i) / 1024.0;
      sqrtPow045[i] = Quantize8(std::pow(s * s, 0.45));
    }
  }

  static const ColorLuts& Instance()
  {
    static const ColorLuts instance;
    return instance;
  }

  static inline uint8_t LookupFloat(float x, const std::array<uint8_t, kLutSize>& lut)
  {
    if (x <= 0.0f)
      return 0u;
    if (x >= 1.0f)
      return 255u;
    uint32_t bits = 0u;
    std::memcpy(&bits, &x, sizeof(bits));
    const uint32_t bucket = bits >> 16;
    if (bucket < kMinBucket)
      return 0u;
    const uint32_t idx = bucket - kMinBucket;
    return lut[idx < (kLutSize - 1u) ? idx : (kLutSize - 1u)];
  }
};

struct PreparedContext
{
  BakeKey key{};
  const ColorLuts* luts{nullptr};
  float hdrPgsTargetNorm{0.05f};
  uint8_t hdrPgsIdentityMaxByte{0};
  float sdrPgsPeakScale{0.01f};
  bool sdrPgsZero{false};
  float hdrGain{1.0f};
  bool unitSat{true};

  explicit PreparedContext(const BakeKey& k) : key(k), luts(&ColorLuts::Instance())
  {
    unitSat = std::fabs(k.saturation - 1.0f) < 1e-5f;
    if (k.mode == BakeMode::HDR_PGS_PQ_OUT)
    {
      hdrPgsTargetNorm = 0.05f * k.peak;
      uint8_t maxByte = 0u;
      for (int i = 0; i < 256; ++i)
      {
        if (luts->pqDecode[i] <= hdrPgsTargetNorm)
          maxByte = static_cast<uint8_t>(i);
        else
          break;
      }
      hdrPgsIdentityMaxByte = maxByte;
    }
    else if (k.mode == BakeMode::SDR_SUBS_PQ_OUT)
    {
      const double peakNits =
          100.0 * std::pow(std::max(static_cast<double>(k.peak), 1e-10), 3.3219281);
      sdrPgsZero = (peakNits < 0.001);
      sdrPgsPeakScale = static_cast<float>(peakNits / 10000.0);
    }
    else if (k.mode == BakeMode::SDR_GUI_HDR_OUT)
    {
      hdrGain = std::max(k.peak, 0.0f);
    }
  }
};

inline uint32_t ConvertPixel(uint32_t rawArgb, const PreparedContext& ctx)
{
  const uint32_t a = rawArgb >> 24;
  if (a == 0u)
    return 0u;

  const uint32_t r = (rawArgb >> 16) & 0xFFu;
  const uint32_t g = (rawArgb >> 8) & 0xFFu;
  const uint32_t b = rawArgb & 0xFFu;
  const ColorLuts& luts = *ctx.luts;

  uint32_t outR = r;
  uint32_t outG = g;
  uint32_t outB = b;

  switch (ctx.key.mode)
  {
    case BakeMode::NONE:
    case BakeMode::PASSTHROUGH_SDR:
      break;

    case BakeMode::HDR_PGS_SDR_OUT:
    {
      const float lr = luts.pqDecode100[r];
      const float lg = luts.pqDecode100[g];
      const float lb = luts.pqDecode100[b];
      // Column-major mat3 bt2020ToBt709 matching gles_shader_texture_noblend.frag
      float r7 = std::max(0.0f, 1.6605f * lr - 0.5876f * lg - 0.0728f * lb);
      float g7 = std::max(0.0f, -0.1246f * lr + 1.1329f * lg - 0.0083f * lb);
      float b7 = std::max(0.0f, -0.0182f * lr - 0.1006f * lg + 1.1187f * lb);
      const float maxC = std::max(r7, std::max(g7, b7));
      if (maxC > 1.0f)
      {
        const float inv = 1.0f / maxC;
        r7 *= inv;
        g7 *= inv;
        b7 *= inv;
      }
      outR = ColorLuts::LookupFloat(r7, luts.pow045);
      outG = ColorLuts::LookupFloat(g7, luts.pow045);
      outB = ColorLuts::LookupFloat(b7, luts.pow045);
      break;
    }

    case BakeMode::HDR_PGS_PQ_OUT:
    {
      if (ctx.unitSat && r <= ctx.hdrPgsIdentityMaxByte && g <= ctx.hdrPgsIdentityMaxByte &&
          b <= ctx.hdrPgsIdentityMaxByte)
      {
        break;
      }
      float lr = luts.pqDecode[r];
      float lg = luts.pqDecode[g];
      float lb = luts.pqDecode[b];
      if (!ctx.unitSat)
      {
        const float luma = 0.2627f * lr + 0.6780f * lg + 0.0593f * lb;
        const float s = ctx.key.saturation;
        lr = std::max(0.0f, luma + (lr - luma) * s);
        lg = std::max(0.0f, luma + (lg - luma) * s);
        lb = std::max(0.0f, luma + (lb - luma) * s);
      }
      const float maxC = std::max(lr, std::max(lg, lb));
      if (maxC > ctx.hdrPgsTargetNorm && maxC > 0.0f)
      {
        const float scale = ctx.hdrPgsTargetNorm / maxC;
        lr *= scale;
        lg *= scale;
        lb *= scale;
      }
      outR = ColorLuts::LookupFloat(lr, luts.pqEncode);
      outG = ColorLuts::LookupFloat(lg, luts.pqEncode);
      outB = ColorLuts::LookupFloat(lb, luts.pqEncode);
      break;
    }

    case BakeMode::SDR_SUBS_PQ_OUT:
    {
      if (ctx.sdrPgsZero)
      {
        outR = outG = outB = 0u;
        break;
      }
      const auto& dec = ctx.key.srgbDecode ? luts.srgbDecode : luts.gammaInv045;
      const float lr = dec[r];
      const float lg = dec[g];
      const float lb = dec[b];
      // Column-major mat3 BT.709 -> BT.2020 matching gles_shader_texture_noblend.frag
      float r20 = std::max(0.0f, 0.627402f * lr + 0.329292f * lg + 0.043306f * lb);
      float g20 = std::max(0.0f, 0.069095f * lr + 0.919544f * lg + 0.011360f * lb);
      float b20 = std::max(0.0f, 0.016394f * lr + 0.088028f * lg + 0.895578f * lb);
      if (!ctx.unitSat)
      {
        const float luma = 0.2627f * r20 + 0.6780f * g20 + 0.0593f * b20;
        const float s = ctx.key.saturation;
        r20 = std::max(0.0f, luma + (r20 - luma) * s);
        g20 = std::max(0.0f, luma + (g20 - luma) * s);
        b20 = std::max(0.0f, luma + (b20 - luma) * s);
      }
      outR = ColorLuts::LookupFloat(r20 * ctx.sdrPgsPeakScale, luts.pqEncode);
      outG = ColorLuts::LookupFloat(g20 * ctx.sdrPgsPeakScale, luts.pqEncode);
      outB = ColorLuts::LookupFloat(b20 * ctx.sdrPgsPeakScale, luts.pqEncode);
      break;
    }

    case BakeMode::SDR_GUI_HDR_OUT:
    {
      float lr = luts.gammaInv045[r];
      float lg = luts.gammaInv045[g];
      float lb = luts.gammaInv045[b];
      if (!ctx.unitSat)
      {
        const float luma = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
        const float s = ctx.key.saturation;
        lr = std::max(0.0f, luma + (lr - luma) * s);
        lg = std::max(0.0f, luma + (lg - luma) * s);
        lb = std::max(0.0f, luma + (lb - luma) * s);
      }
      const float gain = ctx.hdrGain;
      if (gain > 1.0f)
      {
        const float gm1 = gain - 1.0f;
        lr = (lr * gain) / (1.0f + lr * gm1);
        lg = (lg * gain) / (1.0f + lg * gm1);
        lb = (lb * gain) / (1.0f + lb * gm1);
      }
      else
      {
        lr *= gain;
        lg *= gain;
        lb *= gain;
      }
      outR = ColorLuts::LookupFloat(lr, luts.pow045);
      outG = ColorLuts::LookupFloat(lg, luts.pow045);
      outB = ColorLuts::LookupFloat(lb, luts.pow045);
      break;
    }
  }

  if (ctx.key.limitedRange)
  {
    outR = luts.limitedRange[outR];
    outG = luts.limitedRange[outG];
    outB = luts.limitedRange[outB];
  }

  return PremultiplyArgbPixel((a << 24) | (outR << 16) | (outG << 8) | outB);
}

#if BDJ_COLOR_HAS_NEON
inline float32x4_t FastRecipNeon(float32x4_t x)
{
  float32x4_t r = vrecpeq_f32(x);
  r = vmulq_f32(vrecpsq_f32(x, r), r);
  return r;
}

inline uint8x8_t LookupPow045_4Neon(float32x4_t x, const uint8_t* __restrict sqrtLut)
{
  const float32x4_t vMin = vdupq_n_f32(1e-12f);
  const float32x4_t safe = vmaxq_f32(x, vMin);
  float32x4_t rs = vrsqrteq_f32(safe);
  rs = vmulq_f32(vrsqrtsq_f32(vmulq_f32(safe, rs), rs), rs);
  const float32x4_t sq = vminq_f32(vdupq_n_f32(1.0f), vmulq_f32(x, rs));
  const uint32x4_t idx = vcvtq_u32_f32(vmlaq_n_f32(vdupq_n_f32(0.5f), sq, 1024.0f));
  uint8x8_t out = vdup_n_u8(0);
  out = vset_lane_u8(sqrtLut[vgetq_lane_u32(idx, 0)], out, 0);
  out = vset_lane_u8(sqrtLut[vgetq_lane_u32(idx, 1)], out, 1);
  out = vset_lane_u8(sqrtLut[vgetq_lane_u32(idx, 2)], out, 2);
  out = vset_lane_u8(sqrtLut[vgetq_lane_u32(idx, 3)], out, 3);
  return out;
}

inline uint8x8_t LookupFloat4Neon(float32x4_t v, const uint8_t* __restrict lut)
{
  const float32x4_t vZero = vdupq_n_f32(0.0f);
  const uint32x4_t vMinBucket = vdupq_n_u32(ColorLuts::kMinBucket);
  const uint32x4_t vMaxIdx = vdupq_n_u32(ColorLuts::kLutSize - 1u);
  const float32x4_t vClamped = vmaxq_f32(vZero, v);
  const uint32x4_t bits = vreinterpretq_u32_f32(vClamped);
  const uint32x4_t bucket = vshrq_n_u32(bits, 16);
  const uint32x4_t underflow = vcltq_u32(bucket, vMinBucket);
  const uint32x4_t idx = vminq_u32(vqsubq_u32(bucket, vMinBucket), vMaxIdx);

  uint8x8_t out = vdup_n_u8(0);
  out = vset_lane_u8(vgetq_lane_u32(underflow, 0) ? 0u : lut[vgetq_lane_u32(idx, 0)], out, 0);
  out = vset_lane_u8(vgetq_lane_u32(underflow, 1) ? 0u : lut[vgetq_lane_u32(idx, 1)], out, 1);
  out = vset_lane_u8(vgetq_lane_u32(underflow, 2) ? 0u : lut[vgetq_lane_u32(idx, 2)], out, 2);
  out = vset_lane_u8(vgetq_lane_u32(underflow, 3) ? 0u : lut[vgetq_lane_u32(idx, 3)], out, 3);
  return out;
}

inline void PackAndPremultiply4NeonU8(uint8x8_t a8,
                                      uint8x8_t r8,
                                      uint8x8_t g8,
                                      uint8x8_t b8,
                                      bool limitedRange,
                                      const ColorLuts& luts,
                                      uint32_t out[4])
{
  if (limitedRange)
  {
    const uint8_t* lim = luts.limitedRange.data();
    r8 = vset_lane_u8(lim[vget_lane_u8(r8, 0)], r8, 0);
    r8 = vset_lane_u8(lim[vget_lane_u8(r8, 1)], r8, 1);
    r8 = vset_lane_u8(lim[vget_lane_u8(r8, 2)], r8, 2);
    r8 = vset_lane_u8(lim[vget_lane_u8(r8, 3)], r8, 3);
    g8 = vset_lane_u8(lim[vget_lane_u8(g8, 0)], g8, 0);
    g8 = vset_lane_u8(lim[vget_lane_u8(g8, 1)], g8, 1);
    g8 = vset_lane_u8(lim[vget_lane_u8(g8, 2)], g8, 2);
    g8 = vset_lane_u8(lim[vget_lane_u8(g8, 3)], g8, 3);
    b8 = vset_lane_u8(lim[vget_lane_u8(b8, 0)], b8, 0);
    b8 = vset_lane_u8(lim[vget_lane_u8(b8, 1)], b8, 1);
    b8 = vset_lane_u8(lim[vget_lane_u8(b8, 2)], b8, 2);
    b8 = vset_lane_u8(lim[vget_lane_u8(b8, 3)], b8, 3);
  }

  const uint16x8_t vOne16 = vdupq_n_u16(1u);
  const uint16x8_t rm = vmull_u8(r8, a8);
  const uint16x8_t gm = vmull_u8(g8, a8);
  const uint16x8_t bm = vmull_u8(b8, a8);

  uint8x8x4_t packed;
  packed.val[0] = vshrn_n_u16(vaddq_u16(vaddq_u16(bm, vOne16), vshrq_n_u16(bm, 8)), 8);
  packed.val[1] = vshrn_n_u16(vaddq_u16(vaddq_u16(gm, vOne16), vshrq_n_u16(gm, 8)), 8);
  packed.val[2] = vshrn_n_u16(vaddq_u16(vaddq_u16(rm, vOne16), vshrq_n_u16(rm, 8)), 8);
  packed.val[3] = a8;

  // Store 4 interleaved BGRA pixels via vst4_lane or zip (avoiding 32-byte overwrite).
  const uint8x8x2_t bg = vzip_u8(packed.val[0], packed.val[1]);
  const uint8x8x2_t ra = vzip_u8(packed.val[2], packed.val[3]);
  const uint16x4x2_t bgra =
      vzip_u16(vreinterpret_u16_u8(bg.val[0]), vreinterpret_u16_u8(ra.val[0]));
  vst1_u32(out + 0, vreinterpret_u32_u16(bgra.val[0]));
  vst1_u32(out + 2, vreinterpret_u32_u16(bgra.val[1]));
}

template<BakeMode Mode, bool Limited>
inline void Convert4PixelsNeonT(const uint32_t px[4], const PreparedContext& ctx, uint32_t out[4])
{
  const ColorLuts& luts = *ctx.luts;
  const uint32_t p0 = px[0];
  const uint32_t p1 = px[1];
  const uint32_t p2 = px[2];
  const uint32_t p3 = px[3];

  uint8x8_t a8 = vdup_n_u8(0);
  a8 = vset_lane_u8(static_cast<uint8_t>(p0 >> 24), a8, 0);
  a8 = vset_lane_u8(static_cast<uint8_t>(p1 >> 24), a8, 1);
  a8 = vset_lane_u8(static_cast<uint8_t>(p2 >> 24), a8, 2);
  a8 = vset_lane_u8(static_cast<uint8_t>(p3 >> 24), a8, 3);

  const uint32_t r0 = (p0 >> 16) & 0xFFu;
  const uint32_t r1 = (p1 >> 16) & 0xFFu;
  const uint32_t r2 = (p2 >> 16) & 0xFFu;
  const uint32_t r3 = (p3 >> 16) & 0xFFu;
  const uint32_t g0 = (p0 >> 8) & 0xFFu;
  const uint32_t g1 = (p1 >> 8) & 0xFFu;
  const uint32_t g2 = (p2 >> 8) & 0xFFu;
  const uint32_t g3 = (p3 >> 8) & 0xFFu;
  const uint32_t b0 = p0 & 0xFFu;
  const uint32_t b1 = p1 & 0xFFu;
  const uint32_t b2 = p2 & 0xFFu;
  const uint32_t b3 = p3 & 0xFFu;

  auto makeU8 = [](uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) -> uint8x8_t {
    uint8x8_t v = vdup_n_u8(0);
    v = vset_lane_u8(static_cast<uint8_t>(v0), v, 0);
    v = vset_lane_u8(static_cast<uint8_t>(v1), v, 1);
    v = vset_lane_u8(static_cast<uint8_t>(v2), v, 2);
    v = vset_lane_u8(static_cast<uint8_t>(v3), v, 3);
    return v;
  };

  if constexpr (Mode == BakeMode::NONE || Mode == BakeMode::PASSTHROUGH_SDR)
  {
    PackAndPremultiply4NeonU8(a8, makeU8(r0, r1, r2, r3), makeU8(g0, g1, g2, g3),
                              makeU8(b0, b1, b2, b3), Limited, luts, out);
  }
  else if constexpr (Mode == BakeMode::HDR_PGS_SDR_OUT)
  {
    const float* pq100 = luts.pqDecode100.data();
    const float32x4_t lr = {pq100[r0], pq100[r1], pq100[r2], pq100[r3]};
    const float32x4_t lg = {pq100[g0], pq100[g1], pq100[g2], pq100[g3]};
    const float32x4_t lb = {pq100[b0], pq100[b1], pq100[b2], pq100[b3]};
    const float32x4_t vZero = vdupq_n_f32(0.0f);
    const float32x4_t vOne = vdupq_n_f32(1.0f);

    float32x4_t r7 = vmaxq_f32(
        vZero, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, 1.6605f), lg, -0.5876f), lb, -0.0728f));
    float32x4_t g7 = vmaxq_f32(
        vZero, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, -0.1246f), lg, 1.1329f), lb, -0.0083f));
    float32x4_t b7 = vmaxq_f32(
        vZero, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, -0.0182f), lg, -0.1006f), lb, 1.1187f));

    const float32x4_t maxC = vmaxq_f32(vOne, vmaxq_f32(r7, vmaxq_f32(g7, b7)));
    const float32x4_t inv = FastRecipNeon(maxC);
    r7 = vmulq_f32(r7, inv);
    g7 = vmulq_f32(g7, inv);
    b7 = vmulq_f32(b7, inv);

    const uint8_t* sqrtLut = luts.sqrtPow045.data();
    PackAndPremultiply4NeonU8(a8, LookupPow045_4Neon(r7, sqrtLut), LookupPow045_4Neon(g7, sqrtLut),
                              LookupPow045_4Neon(b7, sqrtLut), Limited, luts, out);
  }
  else if constexpr (Mode == BakeMode::HDR_PGS_PQ_OUT)
  {
    if (ctx.unitSat)
    {
      const uint32_t maxB = ctx.hdrPgsIdentityMaxByte;
      if (r0 <= maxB && g0 <= maxB && b0 <= maxB && r1 <= maxB && g1 <= maxB && b1 <= maxB &&
          r2 <= maxB && g2 <= maxB && b2 <= maxB && r3 <= maxB && g3 <= maxB && b3 <= maxB)
      {
        PackAndPremultiply4NeonU8(a8, makeU8(r0, r1, r2, r3), makeU8(g0, g1, g2, g3),
                                  makeU8(b0, b1, b2, b3), Limited, luts, out);
        return;
      }
    }
    const float* pq = luts.pqDecode.data();
    float32x4_t lr = {pq[r0], pq[r1], pq[r2], pq[r3]};
    float32x4_t lg = {pq[g0], pq[g1], pq[g2], pq[g3]};
    float32x4_t lb = {pq[b0], pq[b1], pq[b2], pq[b3]};
    const float32x4_t vZero = vdupq_n_f32(0.0f);

    if (!ctx.unitSat)
    {
      const float32x4_t luma =
          vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, 0.2627f), lg, 0.6780f), lb, 0.0593f);
      const float s = ctx.key.saturation;
      lr = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(lr, luma), s));
      lg = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(lg, luma), s));
      lb = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(lb, luma), s));
    }

    const float32x4_t vTarget = vdupq_n_f32(ctx.hdrPgsTargetNorm);
    const float32x4_t maxC = vmaxq_f32(lr, vmaxq_f32(lg, lb));
    const uint32x4_t over = vcgtq_f32(maxC, vTarget);
    const float32x4_t safeMax = vmaxq_f32(maxC, vdupq_n_f32(1e-10f));
    const float32x4_t scale =
        vbslq_f32(over, vmulq_f32(vTarget, FastRecipNeon(safeMax)), vdupq_n_f32(1.0f));
    lr = vmulq_f32(lr, scale);
    lg = vmulq_f32(lg, scale);
    lb = vmulq_f32(lb, scale);

    const uint8_t* pqLut = luts.pqEncode.data();
    PackAndPremultiply4NeonU8(a8, LookupFloat4Neon(lr, pqLut), LookupFloat4Neon(lg, pqLut),
                              LookupFloat4Neon(lb, pqLut), Limited, luts, out);
  }
  else if constexpr (Mode == BakeMode::SDR_SUBS_PQ_OUT)
  {
    if (ctx.sdrPgsZero)
    {
      const uint8x8_t z = vdup_n_u8(0);
      PackAndPremultiply4NeonU8(a8, z, z, z, Limited, luts, out);
      return;
    }
    const float* dec = ctx.key.srgbDecode ? luts.srgbDecode.data() : luts.gammaInv045.data();
    const float32x4_t lr = {dec[r0], dec[r1], dec[r2], dec[r3]};
    const float32x4_t lg = {dec[g0], dec[g1], dec[g2], dec[g3]};
    const float32x4_t lb = {dec[b0], dec[b1], dec[b2], dec[b3]};
    const float32x4_t vZero = vdupq_n_f32(0.0f);

    float32x4_t r20 = vmaxq_f32(
        vZero, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, 0.627402f), lg, 0.329292f), lb, 0.043306f));
    float32x4_t g20 = vmaxq_f32(
        vZero, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, 0.069095f), lg, 0.919544f), lb, 0.011360f));
    float32x4_t b20 = vmaxq_f32(
        vZero, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, 0.016394f), lg, 0.088028f), lb, 0.895578f));

    if (!ctx.unitSat)
    {
      const float32x4_t luma =
          vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(r20, 0.2627f), g20, 0.6780f), b20, 0.0593f);
      const float s = ctx.key.saturation;
      r20 = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(r20, luma), s));
      g20 = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(g20, luma), s));
      b20 = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(b20, luma), s));
    }

    const float scale = ctx.sdrPgsPeakScale;
    const uint8_t* pqLut = luts.pqEncode.data();
    PackAndPremultiply4NeonU8(a8, LookupFloat4Neon(vmulq_n_f32(r20, scale), pqLut),
                              LookupFloat4Neon(vmulq_n_f32(g20, scale), pqLut),
                              LookupFloat4Neon(vmulq_n_f32(b20, scale), pqLut), Limited, luts, out);
  }
  else if constexpr (Mode == BakeMode::SDR_GUI_HDR_OUT)
  {
    const float* dec = luts.gammaInv045.data();
    float32x4_t lr = {dec[r0], dec[r1], dec[r2], dec[r3]};
    float32x4_t lg = {dec[g0], dec[g1], dec[g2], dec[g3]};
    float32x4_t lb = {dec[b0], dec[b1], dec[b2], dec[b3]};
    const float32x4_t vZero = vdupq_n_f32(0.0f);

    if (!ctx.unitSat)
    {
      const float32x4_t luma =
          vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(lr, 0.2126f), lg, 0.7152f), lb, 0.0722f);
      const float s = ctx.key.saturation;
      lr = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(lr, luma), s));
      lg = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(lg, luma), s));
      lb = vmaxq_f32(vZero, vmlaq_n_f32(luma, vsubq_f32(lb, luma), s));
    }

    const float gain = ctx.hdrGain;
    if (gain > 1.0f)
    {
      const float gm1 = gain - 1.0f;
      const float32x4_t vOne = vdupq_n_f32(1.0f);
      lr = vmulq_f32(vmulq_n_f32(lr, gain), FastRecipNeon(vmlaq_n_f32(vOne, lr, gm1)));
      lg = vmulq_f32(vmulq_n_f32(lg, gain), FastRecipNeon(vmlaq_n_f32(vOne, lg, gm1)));
      lb = vmulq_f32(vmulq_n_f32(lb, gain), FastRecipNeon(vmlaq_n_f32(vOne, lb, gm1)));
    }
    else
    {
      lr = vmulq_n_f32(lr, gain);
      lg = vmulq_n_f32(lg, gain);
      lb = vmulq_n_f32(lb, gain);
    }

    const uint8_t* sqrtLut = luts.sqrtPow045.data();
    PackAndPremultiply4NeonU8(a8, LookupPow045_4Neon(lr, sqrtLut), LookupPow045_4Neon(lg, sqrtLut),
                              LookupPow045_4Neon(lb, sqrtLut), Limited, luts, out);
  }
}

inline void Convert4PixelsNeon(const uint32_t px[4], const PreparedContext& ctx, uint32_t out[4])
{
  const bool lim = ctx.key.limitedRange;
  switch (ctx.key.mode)
  {
    case BakeMode::NONE:
    case BakeMode::PASSTHROUGH_SDR:
      if (lim)
        Convert4PixelsNeonT<BakeMode::PASSTHROUGH_SDR, true>(px, ctx, out);
      else
        Convert4PixelsNeonT<BakeMode::PASSTHROUGH_SDR, false>(px, ctx, out);
      break;
    case BakeMode::HDR_PGS_SDR_OUT:
      if (lim)
        Convert4PixelsNeonT<BakeMode::HDR_PGS_SDR_OUT, true>(px, ctx, out);
      else
        Convert4PixelsNeonT<BakeMode::HDR_PGS_SDR_OUT, false>(px, ctx, out);
      break;
    case BakeMode::HDR_PGS_PQ_OUT:
      if (lim)
        Convert4PixelsNeonT<BakeMode::HDR_PGS_PQ_OUT, true>(px, ctx, out);
      else
        Convert4PixelsNeonT<BakeMode::HDR_PGS_PQ_OUT, false>(px, ctx, out);
      break;
    case BakeMode::SDR_SUBS_PQ_OUT:
      if (lim)
        Convert4PixelsNeonT<BakeMode::SDR_SUBS_PQ_OUT, true>(px, ctx, out);
      else
        Convert4PixelsNeonT<BakeMode::SDR_SUBS_PQ_OUT, false>(px, ctx, out);
      break;
    case BakeMode::SDR_GUI_HDR_OUT:
      if (lim)
        Convert4PixelsNeonT<BakeMode::SDR_GUI_HDR_OUT, true>(px, ctx, out);
      else
        Convert4PixelsNeonT<BakeMode::SDR_GUI_HDR_OUT, false>(px, ctx, out);
      break;
  }
}
#endif // BDJ_COLOR_HAS_NEON

// Lightweight persistent worker pool owned by PersistentSurface so threads are
// reused across BD-J frames with zero per-frame spawn overhead and joined
// deterministically when the surface is destroyed.
class BakePool
{
public:
  using TaskFn = void (*)(int sliceIdx, int sliceStart, int sliceEnd, void* userData);

  BakePool() = default;
  BakePool(const BakePool&) = delete;
  BakePool& operator=(const BakePool&) = delete;

  ~BakePool()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stop = true;
      ++m_generation;
    }
    m_workCv.notify_all();
    for (auto& t : m_workers)
    {
      if (t.joinable())
        t.join();
    }
  }

  int MaxSlices() const
  {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 1u)
      return 1;
    return static_cast<int>(std::min<unsigned>(hw, 4u));
  }

  void ParallelFor(int totalItems, int minItemsPerSlice, TaskFn fn, void* userData)
  {
    const int maxSlices = MaxSlices();
    if (maxSlices <= 1 || totalItems < minItemsPerSlice * 2)
    {
      fn(0, 0, totalItems, userData);
      return;
    }

    const int slices = std::min(maxSlices, totalItems / minItemsPerSlice);
    if (slices <= 1)
    {
      fn(0, 0, totalItems, userData);
      return;
    }

    EnsureWorkersStarted(maxSlices - 1);

    const int workerSlices = slices - 1;
    const int step = (totalItems + slices - 1) / slices;

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_taskFn = fn;
      m_userData = userData;
      m_totalItems = totalItems;
      m_step = step;
      m_activeWorkers = workerSlices;
      m_remaining.store(workerSlices, std::memory_order_release);
      ++m_generation;
    }
    m_workCv.notify_all();

    // Caller runs slice 0.
    const int s0End = std::min(step, totalItems);
    fn(0, 0, s0End, userData);

    // Wait for workers to finish slices 1..workerSlices.
    if (m_remaining.load(std::memory_order_acquire) != 0)
    {
      std::unique_lock<std::mutex> lock(m_doneMutex);
      m_doneCv.wait(lock, [this]() { return m_remaining.load(std::memory_order_acquire) == 0; });
    }
  }

private:
  void EnsureWorkersStarted(int numWorkers)
  {
    if (!m_workers.empty() || numWorkers <= 0)
      return;

    m_workers.reserve(static_cast<size_t>(numWorkers));
    for (int i = 0; i < numWorkers; ++i)
    {
      m_workers.emplace_back([this, workerIdx = i + 1]() { WorkerLoop(workerIdx); });
    }
  }

  void WorkerLoop(int sliceIdx)
  {
    uint64_t seenGen = 0;
    while (true)
    {
      TaskFn fn = nullptr;
      void* userData = nullptr;
      int start = 0;
      int end = 0;

      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_workCv.wait(lock, [this, seenGen]() { return m_generation != seenGen || m_stop; });
        if (m_stop)
          return;
        seenGen = m_generation;
        if (sliceIdx > m_activeWorkers)
          continue;
        fn = m_taskFn;
        userData = m_userData;
        start = sliceIdx * m_step;
        end = std::min(start + m_step, m_totalItems);
      }

      if (fn && start < end)
        fn(sliceIdx, start, end, userData);

      if (m_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
      {
        std::lock_guard<std::mutex> doneLock(m_doneMutex);
        m_doneCv.notify_one();
      }
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_workCv;
  std::mutex m_doneMutex;
  std::condition_variable m_doneCv;
  std::vector<std::thread> m_workers;
  TaskFn m_taskFn{nullptr};
  void* m_userData{nullptr};
  int m_totalItems{0};
  int m_step{0};
  int m_activeWorkers{0};
  uint64_t m_generation{0};
  bool m_stop{false};
  std::atomic<int> m_remaining{0};
};

struct BakeRectDiffResult
{
  int changedX0{0};
  int changedY0{0};
  int changedX1{-1};
  int changedY1{-1};
  int64_t visDelta{0};
};

template<BakeMode Mode, bool Limited>
inline void BakeRowDiffT(const uint32_t* __restrict srcRow,
                         uint32_t* __restrict rawRow,
                         uint32_t* __restrict pmaRow,
                         int drawW,
                         const PreparedContext& ctx,
                         int& rowMinX,
                         int& rowMaxX,
                         int64_t& visDelta)
{
  uint32_t lastRaw = 0u;
  uint32_t lastBaked = 0u;
  int col = 0;

#if BDJ_COLOR_HAS_NEON
  for (; col + 4 <= drawW; col += 4)
  {
    const uint32_t s0 = srcRow[col + 0];
    const uint32_t s1 = srcRow[col + 1];
    const uint32_t s2 = srcRow[col + 2];
    const uint32_t s3 = srcRow[col + 3];
    const uint32_t o0 = rawRow[col + 0];
    const uint32_t o1 = rawRow[col + 1];
    const uint32_t o2 = rawRow[col + 2];
    const uint32_t o3 = rawRow[col + 3];

    if (((s0 ^ o0) | (s1 ^ o1) | (s2 ^ o2) | (s3 ^ o3)) == 0u)
      continue;

    const uint32_t c0 = (s0 & 0xFF000000u) == 0u ? 0u : s0;
    const uint32_t c1 = (s1 & 0xFF000000u) == 0u ? 0u : s1;
    const uint32_t c2 = (s2 & 0xFF000000u) == 0u ? 0u : s2;
    const uint32_t c3 = (s3 & 0xFF000000u) == 0u ? 0u : s3;

    if (((c0 ^ o0) | (c1 ^ o1) | (c2 ^ o2) | (c3 ^ o3)) == 0u)
      continue;

    if (c0 == c1 && c0 == c2 && c0 == c3)
    {
      uint32_t baked = 0u;
      if (c0 != 0u)
      {
        if (c0 == lastRaw)
        {
          baked = lastBaked;
        }
        else
        {
          baked = ConvertPixel(c0, ctx);
          lastRaw = c0;
          lastBaked = baked;
        }
      }
      const uint32_t oArr[4] = {o0, o1, o2, o3};
      for (int k = 0; k < 4; ++k)
      {
        if (c0 == oArr[k])
          continue;
        const int cx = col + k;
        rawRow[cx] = c0;
        if (oArr[k] != 0u)
          --visDelta;
        if (c0 != 0u)
          ++visDelta;
        pmaRow[cx] = baked;
        if (cx < rowMinX)
          rowMinX = cx;
        rowMaxX = cx;
      }
      continue;
    }

    const int changedVis = (c0 != o0 && c0 != 0u) + (c1 != o1 && c1 != 0u) +
                           (c2 != o2 && c2 != 0u) + (c3 != o3 && c3 != 0u);
    if (changedVis >= 2)
    {
      const uint32_t in4[4] = {c0, c1, c2, c3};
      uint32_t out4[4];
      Convert4PixelsNeonT<Mode, Limited>(in4, ctx, out4);

      if (c0 != o0)
      {
        rawRow[col + 0] = c0;
        pmaRow[col + 0] = out4[0];
        if (o0 != 0u)
          --visDelta;
        if (c0 != 0u)
          ++visDelta;
        if (col < rowMinX)
          rowMinX = col;
        rowMaxX = col;
      }
      if (c1 != o1)
      {
        rawRow[col + 1] = c1;
        pmaRow[col + 1] = out4[1];
        if (o1 != 0u)
          --visDelta;
        if (c1 != 0u)
          ++visDelta;
        if (col + 1 < rowMinX)
          rowMinX = col + 1;
        rowMaxX = col + 1;
      }
      if (c2 != o2)
      {
        rawRow[col + 2] = c2;
        pmaRow[col + 2] = out4[2];
        if (o2 != 0u)
          --visDelta;
        if (c2 != 0u)
          ++visDelta;
        if (col + 2 < rowMinX)
          rowMinX = col + 2;
        rowMaxX = col + 2;
      }
      if (c3 != o3)
      {
        rawRow[col + 3] = c3;
        pmaRow[col + 3] = out4[3];
        if (o3 != 0u)
          --visDelta;
        if (c3 != 0u)
          ++visDelta;
        if (col + 3 < rowMinX)
          rowMinX = col + 3;
        rowMaxX = col + 3;
      }
      if (c3 != 0u)
      {
        lastRaw = c3;
        lastBaked = out4[3];
      }
      continue;
    }

    const uint32_t cArr[4] = {c0, c1, c2, c3};
    const uint32_t oArr[4] = {o0, o1, o2, o3};
    for (int k = 0; k < 4; ++k)
    {
      const uint32_t srcPx = cArr[k];
      const uint32_t oldRaw = oArr[k];
      if (srcPx == oldRaw)
        continue;
      const int cx = col + k;
      rawRow[cx] = srcPx;
      if (oldRaw != 0u)
        --visDelta;
      if (srcPx != 0u)
        ++visDelta;
      uint32_t bakedPx = 0u;
      if (srcPx != 0u)
      {
        if (srcPx == lastRaw)
        {
          bakedPx = lastBaked;
        }
        else
        {
          bakedPx = ConvertPixel(srcPx, ctx);
          lastRaw = srcPx;
          lastBaked = bakedPx;
        }
      }
      pmaRow[cx] = bakedPx;
      if (cx < rowMinX)
        rowMinX = cx;
      rowMaxX = cx;
    }
  }
#endif

  for (; col < drawW; ++col)
  {
    const uint32_t rawIn = srcRow[col];
    const uint32_t srcPx = (rawIn & 0xFF000000u) == 0u ? 0u : rawIn;
    const uint32_t oldRaw = rawRow[col];
    if (srcPx == oldRaw)
      continue;

    rawRow[col] = srcPx;
    if (oldRaw != 0u)
      --visDelta;
    if (srcPx != 0u)
      ++visDelta;

    uint32_t bakedPx = 0u;
    if (srcPx != 0u)
    {
      if (srcPx == lastRaw)
      {
        bakedPx = lastBaked;
      }
      else
      {
        bakedPx = ConvertPixel(srcPx, ctx);
        lastRaw = srcPx;
        lastBaked = bakedPx;
      }
    }
    pmaRow[col] = bakedPx;
    if (col < rowMinX)
      rowMinX = col;
    rowMaxX = col;
  }
}

inline void BakeRowDiff(const uint32_t* srcRow,
                        uint32_t* rawRow,
                        uint32_t* pmaRow,
                        int drawW,
                        const PreparedContext& ctx,
                        int& rowMinX,
                        int& rowMaxX,
                        int64_t& visDelta)
{
  const bool lim = ctx.key.limitedRange;
  switch (ctx.key.mode)
  {
    case BakeMode::NONE:
    case BakeMode::PASSTHROUGH_SDR:
      if (lim)
        BakeRowDiffT<BakeMode::PASSTHROUGH_SDR, true>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                      rowMaxX, visDelta);
      else
        BakeRowDiffT<BakeMode::PASSTHROUGH_SDR, false>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                       rowMaxX, visDelta);
      break;
    case BakeMode::HDR_PGS_SDR_OUT:
      if (lim)
        BakeRowDiffT<BakeMode::HDR_PGS_SDR_OUT, true>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                      rowMaxX, visDelta);
      else
        BakeRowDiffT<BakeMode::HDR_PGS_SDR_OUT, false>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                       rowMaxX, visDelta);
      break;
    case BakeMode::HDR_PGS_PQ_OUT:
      if (lim)
        BakeRowDiffT<BakeMode::HDR_PGS_PQ_OUT, true>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                     rowMaxX, visDelta);
      else
        BakeRowDiffT<BakeMode::HDR_PGS_PQ_OUT, false>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                      rowMaxX, visDelta);
      break;
    case BakeMode::SDR_SUBS_PQ_OUT:
      if (lim)
        BakeRowDiffT<BakeMode::SDR_SUBS_PQ_OUT, true>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                      rowMaxX, visDelta);
      else
        BakeRowDiffT<BakeMode::SDR_SUBS_PQ_OUT, false>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                       rowMaxX, visDelta);
      break;
    case BakeMode::SDR_GUI_HDR_OUT:
      if (lim)
        BakeRowDiffT<BakeMode::SDR_GUI_HDR_OUT, true>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                      rowMaxX, visDelta);
      else
        BakeRowDiffT<BakeMode::SDR_GUI_HDR_OUT, false>(srcRow, rawRow, pmaRow, drawW, ctx, rowMinX,
                                                       rowMaxX, visDelta);
      break;
  }
}

inline BakeRectDiffResult BakeRectDiff(const uint32_t* srcBase,
                                       int srcStride,
                                       uint32_t* rawBase,
                                       uint32_t* pmaBase,
                                       int surfW,
                                       int surfH,
                                       int ox,
                                       int oy,
                                       int drawW,
                                       int drawH,
                                       const PreparedContext& ctx,
                                       BakePool* pool = nullptr)
{
  BakeRectDiffResult result;
  result.changedX0 = surfW;
  result.changedY0 = surfH;
  result.changedX1 = -1;
  result.changedY1 = -1;
  result.visDelta = 0;

  if (drawW <= 0 || drawH <= 0)
    return result;

  struct SliceState
  {
    int x0;
    int y0;
    int x1;
    int y1;
    int64_t visDelta;
  };

  struct Job
  {
    const uint32_t* srcBase;
    int srcStride;
    uint32_t* rawBase;
    uint32_t* pmaBase;
    int surfW;
    int surfH;
    int ox;
    int oy;
    int drawW;
    const PreparedContext* ctx;
    std::array<SliceState, 4> slices;
  };

  Job job{};
  job.srcBase = srcBase;
  job.srcStride = srcStride;
  job.rawBase = rawBase;
  job.pmaBase = pmaBase;
  job.surfW = surfW;
  job.surfH = surfH;
  job.ox = ox;
  job.oy = oy;
  job.drawW = drawW;
  job.ctx = &ctx;
  for (auto& s : job.slices)
    s = {surfW, surfH, -1, -1, 0};

  auto task = [](int sliceIdx, int rowStart, int rowEnd, void* ud) {
    auto* j = static_cast<Job*>(ud);
    SliceState& st = j->slices[static_cast<size_t>(sliceIdx)];
    int localX0 = j->surfW;
    int localY0 = j->surfH;
    int localX1 = -1;
    int localY1 = -1;
    int64_t localVis = 0;

    for (int row = rowStart; row < rowEnd; ++row)
    {
      const uint32_t* srcRow =
          j->srcBase + static_cast<size_t>(row) * static_cast<size_t>(j->srcStride);
      const size_t rowOffset =
          static_cast<size_t>(j->oy + row) * static_cast<size_t>(j->surfW) +
          static_cast<size_t>(j->ox);
      uint32_t* rawRow = j->rawBase + rowOffset;
      uint32_t* pmaRow = j->pmaBase + rowOffset;

      int rowMinX = j->drawW;
      int rowMaxX = -1;
      BakeRowDiff(srcRow, rawRow, pmaRow, j->drawW, *j->ctx, rowMinX, rowMaxX, localVis);
      if (rowMaxX >= rowMinX)
      {
        const int gx0 = j->ox + rowMinX;
        const int gx1 = j->ox + rowMaxX;
        const int gy = j->oy + row;
        if (gx0 < localX0)
          localX0 = gx0;
        if (gx1 > localX1)
          localX1 = gx1;
        if (gy < localY0)
          localY0 = gy;
        localY1 = gy;
      }
    }

    st.x0 = localX0;
    st.y0 = localY0;
    st.x1 = localX1;
    st.y1 = localY1;
    st.visDelta = localVis;
  };

  // Only wake worker threads when total pixel area is large enough to amortize wakeup (~32k px).
  const int minRowsPerSlice = std::max(16, 16384 / std::max(1, drawW));
  if (pool && static_cast<int64_t>(drawW) * static_cast<int64_t>(drawH) >= 32768)
    pool->ParallelFor(drawH, minRowsPerSlice, task, &job);
  else
    task(0, 0, drawH, &job);

  for (const auto& st : job.slices)
  {
    result.visDelta += st.visDelta;
    if (st.x1 >= st.x0 && st.y1 >= st.y0)
    {
      if (st.x0 < result.changedX0)
        result.changedX0 = st.x0;
      if (st.x1 > result.changedX1)
        result.changedX1 = st.x1;
      if (st.y0 < result.changedY0)
        result.changedY0 = st.y0;
      if (st.y1 > result.changedY1)
        result.changedY1 = st.y1;
    }
  }
  return result;
}

inline void BakeFullSurface(const std::vector<uint32_t>& raw,
                            std::vector<uint32_t>& outPma,
                            const BakeKey& key,
                            BakePool* pool = nullptr)
{
  const size_t n = raw.size();
  outPma.resize(n);
  if (n == 0)
    return;

  const PreparedContext ctx(key);

  struct FullJob
  {
    const uint32_t* src;
    uint32_t* dst;
    const PreparedContext* ctx;
    size_t total;
    size_t chunkSize;
  };

  constexpr size_t kChunk = 1920u;
  const int numChunks = static_cast<int>((n + kChunk - 1u) / kChunk);
  FullJob job{raw.data(), outPma.data(), &ctx, n, kChunk};

  auto task = [](int /*sliceIdx*/, int chunkStart, int chunkEnd, void* ud) {
    auto* j = static_cast<FullJob*>(ud);
    const size_t iStart = static_cast<size_t>(chunkStart) * j->chunkSize;
    const size_t iEnd = std::min(static_cast<size_t>(chunkEnd) * j->chunkSize, j->total);
    const uint32_t* src = j->src;
    uint32_t* dst = j->dst;
    const PreparedContext& c = *j->ctx;
    uint32_t lastRaw = 0u;
    uint32_t lastOut = 0u;
    size_t i = iStart;

#if BDJ_COLOR_HAS_NEON
    for (; i + 4u <= iEnd; i += 4u)
    {
      const uint32_t p0 = src[i + 0u];
      const uint32_t p1 = src[i + 1u];
      const uint32_t p2 = src[i + 2u];
      const uint32_t p3 = src[i + 3u];
      if ((p0 | p1 | p2 | p3) == 0u)
      {
        dst[i + 0u] = 0u;
        dst[i + 1u] = 0u;
        dst[i + 2u] = 0u;
        dst[i + 3u] = 0u;
        continue;
      }
      if (p0 == p1 && p0 == p2 && p0 == p3)
      {
        uint32_t out = 0u;
        if (p0 != 0u)
        {
          if (p0 == lastRaw)
          {
            out = lastOut;
          }
          else
          {
            out = ConvertPixel(p0, c);
            lastRaw = p0;
            lastOut = out;
          }
        }
        dst[i + 0u] = out;
        dst[i + 1u] = out;
        dst[i + 2u] = out;
        dst[i + 3u] = out;
        continue;
      }
      if (((p0 != 0u) + (p1 != 0u) + (p2 != 0u) + (p3 != 0u)) >= 2)
      {
        const uint32_t in4[4] = {p0, p1, p2, p3};
        uint32_t out4[4];
        Convert4PixelsNeon(in4, c, out4);
        dst[i + 0u] = out4[0];
        dst[i + 1u] = out4[1];
        dst[i + 2u] = out4[2];
        dst[i + 3u] = out4[3];
        if (p3 != 0u)
        {
          lastRaw = p3;
          lastOut = out4[3];
        }
        continue;
      }
      for (size_t k = 0; k < 4u; ++k)
      {
        const uint32_t px = src[i + k];
        if (px == 0u)
        {
          dst[i + k] = 0u;
          continue;
        }
        if (px == lastRaw)
        {
          dst[i + k] = lastOut;
          continue;
        }
        const uint32_t out = ConvertPixel(px, c);
        lastRaw = px;
        lastOut = out;
        dst[i + k] = out;
      }
    }
#endif

    for (; i < iEnd; ++i)
    {
      const uint32_t px = src[i];
      if (px == 0u)
      {
        dst[i] = 0u;
        continue;
      }
      if (px == lastRaw)
      {
        dst[i] = lastOut;
        continue;
      }
      const uint32_t out = ConvertPixel(px, c);
      lastRaw = px;
      lastOut = out;
      dst[i] = out;
    }
  };

  if (pool && n >= 32768u)
    pool->ParallelFor(numChunks, 16, task, &job);
  else
    task(0, 0, numChunks, &job);
}

} // namespace BDJ_COLOR
