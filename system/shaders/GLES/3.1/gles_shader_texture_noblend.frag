/*
 *      Copyright (C) 2010-2013 Team XBMC
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

#version 310 es

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D m_samp0;
in vec4 m_cord0;

layout(std140) uniform KodiGuiFragmentBlock
{
  vec4 uGuiParams0;
  vec4 uGuiParams1;
};

out vec4 fragColor;

highp float interleavedGradientNoise(highp vec2 co)
{
  return fract(52.9829189 * fract(0.06711056 * co.x + 0.00583715 * co.y));
}

uniform float m_guiSrgbDecode;
uniform float m_guiDither8;
uniform float m_guiTransferBypass;

vec3 guiSrgbToLinear(vec3 x)
{
  vec3 lo = x / 12.92;
  vec3 hi = pow(max((x + vec3(0.055)) / 1.055, vec3(1e-10)), vec3(2.4));
  return mix(lo, hi, step(vec3(0.04045), x));
}

vec3 adjustGuiForHdrOutput(vec3 x)
{
  x = pow(max(x, vec3(1e-10)), vec3(1.0 / 0.45));

  vec3 luma = vec3(dot(x, vec3(0.2126, 0.7152, 0.0722)));
  x = mix(luma, x, uGuiParams0.w);
  x = max(x, vec3(0.0));

  float gain = max(uGuiParams0.z, 0.0);
  vec3 boosted = x * gain;

  if (gain > 1.0)
    x = boosted / (vec3(1.0) + x * (gain - 1.0));
  else
    x = boosted;

  x = pow(max(x, vec3(1e-10)), vec3(0.45));
  float dither = (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) * mix(1.0 / 1024.0, 1.0 / 255.0, step(0.5, m_guiDither8));
  return clamp(x + vec3(dither), vec3(0.0), vec3(1.0));
}

vec3 convertGuiForPqOutput(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  const mat3 matx = mat3(
      0.627402, 0.069095, 0.016394,
      0.329292, 0.919544, 0.088028,
      0.043306, 0.011360, 0.895578);

  if (m_guiSrgbDecode > 0.5)
    x = guiSrgbToLinear(x);
  else
    x = pow(max(x, vec3(1e-10)), vec3(1.0 / 0.45));
  x = matx * x;
  x = max(x, vec3(0.0));

  vec3 luma = vec3(dot(x, vec3(0.2627, 0.6780, 0.0593)));
  x = mix(luma, x, uGuiParams0.w);
  x = max(x, vec3(0.0));

  float peakNits = 100.0 * uGuiParams0.z;
  x = pow(max(x * (peakNits / 10000.0), vec3(1e-10)), vec3(ST2084_m1));
  x = (ST2084_c1 + ST2084_c2 * x) / (1.0 + ST2084_c3 * x);
  x = pow(x, vec3(ST2084_m2));

  float dither = (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) * mix(1.0 / 1024.0, 1.0 / 255.0, step(0.5, m_guiDither8));
  return clamp(x + vec3(dither), vec3(0.0), vec3(1.0));
}

#if defined(KODI_HDR_PGS_PQ_OUTPUT) || defined(KODI_HDR_PGS_SDR_OUTPUT)
vec3 decodePQ(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  x = clamp(x, vec3(0.0), vec3(1.0));
  vec3 p = pow(max(x, vec3(1e-10)), vec3(1.0 / ST2084_m2));
  vec3 num = max(p - vec3(ST2084_c1), vec3(0.0));
  vec3 den = max(vec3(ST2084_c2) - vec3(ST2084_c3) * p, vec3(1e-6));
  return pow(max(num / den, vec3(1e-10)), vec3(1.0 / ST2084_m1));
}
#endif

#if defined(KODI_HDR_PGS_PQ_OUTPUT)
vec3 encodePQ(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  vec3 p = pow(max(x, vec3(1e-10)), vec3(ST2084_m1));
  vec3 y = (vec3(ST2084_c1) + vec3(ST2084_c2) * p) / (vec3(1.0) + vec3(ST2084_c3) * p);
  y = pow(y, vec3(ST2084_m2));
  return clamp(y, vec3(0.0), vec3(1.0));
}

vec3 convertHdrPgsForPqOutput(vec3 pq)
{
  vec3 linear = decodePQ(pq);
  vec3 luma = vec3(dot(linear, vec3(0.2627, 0.6780, 0.0593)));
  linear = mix(luma, linear, uGuiParams1.y);
  linear = max(linear, vec3(0.0));

  float targetNorm = 0.05 * uGuiParams1.x;
  float maxC = max(max(linear.r, linear.g), linear.b);
  if (maxC > targetNorm)
    linear *= targetNorm / maxC;

  float dither = (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) * mix(1.0 / 1024.0, 1.0 / 255.0, step(0.5, m_guiDither8));
  return clamp(encodePQ(linear) + vec3(dither), vec3(0.0), vec3(1.0));
}
#endif

#if defined(KODI_SDR_IMAGE_SUBS)
vec3 convertSdrImageSubsForPqOutput(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  const mat3 matx = mat3(
      0.627402, 0.069095, 0.016394,
      0.329292, 0.919544, 0.088028,
      0.043306, 0.011360, 0.895578);

  if (m_guiSrgbDecode > 0.5)
    x = guiSrgbToLinear(x);
  else
    x = pow(max(x, vec3(1e-10)), vec3(1.0 / 0.45));
  x = matx * x;
  x = max(x, vec3(0.0));

  vec3 luma = vec3(dot(x, vec3(0.2627, 0.6780, 0.0593)));
  x = mix(luma, x, uGuiParams1.w);
  x = max(x, vec3(0.0));

  float peakNits = 100.0 * pow(max(uGuiParams1.z, 1e-10), 3.3219281);
  if (peakNits < 0.001)
    return vec3(0.0);
  vec3 x_pq = max(x * (peakNits / 10000.0), vec3(1e-10));
  x_pq = pow(x_pq, vec3(ST2084_m1));
  x = (ST2084_c1 + ST2084_c2 * x_pq) / (1.0 + ST2084_c3 * x_pq);
  x = pow(x, vec3(ST2084_m2));

  float dither = (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) * mix(1.0 / 1024.0, 1.0 / 255.0, step(0.5, m_guiDither8));
  return clamp(x + vec3(dither), vec3(0.0), vec3(1.0));
}
#endif

#if defined(KODI_HDR_PGS_SDR_OUTPUT)
vec3 convertHdrPgsForSdrOutput(vec3 pq)
{
  const mat3 bt2020ToBt709 = mat3(
      1.6605, -0.1246, -0.0182,
     -0.5876,  1.1329, -0.1006,
     -0.0728, -0.0083,  1.1187);

  vec3 linear = decodePQ(pq) * 100.0;
  linear = bt2020ToBt709 * linear;
  linear = max(linear, vec3(0.0));
  float maxC = max(max(linear.r, linear.g), linear.b);
  if (maxC > 1.0)
    linear /= maxC;
  vec3 luma = vec3(dot(linear, vec3(0.2126, 0.7152, 0.0722)));
  linear = mix(luma, linear, 1.0);
  linear = pow(max(clamp(linear, vec3(0.0), vec3(1.0)), vec3(1e-10)), vec3(0.45));
  float dither = (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) * mix(1.0 / 1024.0, 1.0 / 255.0, step(0.5, m_guiDither8));
  return clamp(linear + vec3(dither), vec3(0.0), vec3(1.0));
}
#endif

void main()
{
  vec4 rgb = texture(m_samp0, m_cord0.xy);

#if defined(KODI_RAW_PASSTHROUGH)
  if (rgb.a <= 0.001)
    discard;
  fragColor = rgb;
#else
#if defined(KODI_PREMULTIPLIED_ALPHA)
  float pmaAlpha = rgb.a;
  if (pmaAlpha <= 0.001)
    discard;
  rgb.rgb /= pmaAlpha;
#endif

#if defined(KODI_SDR_IMAGE_SUBS) && defined(KODI_TRANSFER_PQ)
  rgb.rgb = convertSdrImageSubsForPqOutput(rgb.rgb);
#elif defined(KODI_TRANSFER_PQ)
  if (m_guiTransferBypass < 0.5)
    rgb.rgb = convertGuiForPqOutput(rgb.rgb);
#elif defined(KODI_TRANSFER_HDR)
  if (m_guiTransferBypass < 0.5)
    rgb.rgb = adjustGuiForHdrOutput(rgb.rgb);
#elif defined(KODI_HDR_PGS_PQ_OUTPUT)
  rgb.rgb = convertHdrPgsForPqOutput(rgb.rgb);
#elif defined(KODI_HDR_PGS_SDR_OUTPUT)
  rgb.rgb = convertHdrPgsForSdrOutput(rgb.rgb);
#endif

#if defined(KODI_LIMITED_RANGE)
#if defined(KODI_TRANSFER_PQ) || defined(KODI_TRANSFER_HDR)
  if (m_guiTransferBypass < 0.5)
#endif
  {
    rgb.rgb *= (235.0 - 16.0) / 255.0;
    rgb.rgb += 16.0 / 255.0;
  }
#endif

#if defined(KODI_PREMULTIPLIED_ALPHA)
  rgb.rgb *= pmaAlpha;
#endif

  fragColor = rgb;
#endif
}