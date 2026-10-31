/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 100

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D m_samp0;
uniform sampler2D m_samp1;
varying vec4 m_cord0;
varying vec4 m_cord1;
uniform lowp vec4 m_unicol;
uniform float m_sdrPeak;
uniform float m_sdrSaturation;

highp float rand(highp vec2 co)
{
  // Simple stable hash for dithering.
  return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
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
  x = mix(luma, x, m_sdrSaturation);
  x = max(x, vec3(0.0));

  float gain = max(m_sdrPeak, 0.0);
  vec3 boosted = x * gain;

  if (gain > 1.0)
    x = boosted / (vec3(1.0) + x * (gain - 1.0));
  else
    x = boosted;

  x = pow(max(x, vec3(1e-10)), vec3(0.45));
  float dither = (rand(gl_FragCoord.xy) - 0.5) * mix(1.0 / 1024.0, 1.0 / 255.0, step(0.5, m_guiDither8));
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

  // REC.709 to linear (approximation)
  if (m_guiSrgbDecode > 0.5)
    x = guiSrgbToLinear(x);
  else
    x = pow(max(x, vec3(1e-10)), vec3(1.0 / 0.45));

  // REC.709 to BT.2020
  x = matx * x;
  x = max(x, vec3(0.0));

  // Optional saturation adjustment for SDR GUI when rendering into HDR PQ output.
  // Apply in linear BT.2020 and clamp to avoid out-of-gamut hue shifts.
  vec3 luma = vec3(dot(x, vec3(0.2627, 0.6780, 0.0593)));
  x = mix(luma, x, m_sdrSaturation);
  x = max(x, vec3(0.0));

  // Scale SDR peak (m_sdrPeak is nits/100)
  float peakNits = 100.0 * m_sdrPeak;

  // Linear (nits) normalized to 10,000 nits, then PQ encode
  x = pow(max(x * (peakNits / 10000.0), vec3(1e-10)), vec3(ST2084_m1));
  x = (ST2084_c1 + ST2084_c2 * x) / (1.0 + ST2084_c3 * x);
  x = pow(x, vec3(ST2084_m2));

  // Dither PQ output to reduce visible banding/stepping in gradients.
  float dither = (rand(gl_FragCoord.xy) - 0.5) * mix(1.0 / 1024.0, 1.0 / 255.0, step(0.5, m_guiDither8));
  x = clamp(x + vec3(dither), vec3(0.0), vec3(1.0));

  return x;
}

void main()
{
  gl_FragColor = m_unicol;
  gl_FragColor.a *= texture2D(m_samp0, m_cord0.xy).r;
  gl_FragColor.a *= texture2D(m_samp1, m_cord1.xy).r;

#if defined(KODI_TRANSFER_PQ)
  if (m_guiTransferBypass < 0.5)
    gl_FragColor.rgb = convertGuiForPqOutput(gl_FragColor.rgb);
#elif defined(KODI_TRANSFER_HDR)
  if (m_guiTransferBypass < 0.5)
    gl_FragColor.rgb = adjustGuiForHdrOutput(gl_FragColor.rgb);
#elif defined(KODI_GUI_SDR_SATURATION)
  // Keep SDR transfer values for Core2; a custom saturation needs no PQ pass.
  if (gl_FragColor.a > 0.0 && abs(m_sdrSaturation - 1.0) > 0.0001)
  {
    vec3 linear = pow(max(gl_FragColor.rgb, vec3(0.0)), vec3(1.0 / 0.45));
    vec3 luma = vec3(dot(linear, vec3(0.2126, 0.7152, 0.0722)));
    gl_FragColor.rgb = pow(clamp(mix(luma, linear, m_sdrSaturation), vec3(0.0), vec3(1.0)),
                  vec3(0.45));
  }
#endif

#if defined(KODI_LIMITED_RANGE)
#if defined(KODI_TRANSFER_PQ) || defined(KODI_TRANSFER_HDR)
  if (m_guiTransferBypass < 0.5)
#endif
  {
    gl_FragColor.rgb *= (235.0 - 16.0) / 255.0;
    gl_FragColor.rgb += 16.0 / 255.0;
  }
#endif
}
