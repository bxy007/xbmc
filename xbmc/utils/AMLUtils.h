/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "windowing/Resolution.h"
#include "utils/StreamDetails.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum AML_DEVICE_TYPE
{
  AML_DEVICE_TYPE_UNINIT   = -2,
  AML_DEVICE_TYPE_UNKNOWN  = -1,
  AML_DEVICE_TYPE_M1,
  AML_DEVICE_TYPE_M3,
  AML_DEVICE_TYPE_M6,
  AML_DEVICE_TYPE_M8,   // S802
  AML_DEVICE_TYPE_M8B,  // S805
  AML_DEVICE_TYPE_M8M2  // S812
};

enum AML_DISPLAY_AXIS_PARAM
{
  AML_DISPLAY_AXIS_PARAM_X = 0,
  AML_DISPLAY_AXIS_PARAM_Y,
  AML_DISPLAY_AXIS_PARAM_WIDTH,
  AML_DISPLAY_AXIS_PARAM_HEIGHT
};

enum AML_SUPPORT_H264_4K2K
{
  AML_SUPPORT_H264_4K2K_UNINIT = -1,
  AML_NO_H264_4K2K,
  AML_HAS_H264_4K2K,
  AML_HAS_H264_4K2K_SAME_PROFILE
};

enum DV_MODE : int
{
  DV_MODE_ON = 0,
  DV_MODE_ON_DEMAND,
  DV_MODE_OFF
};

enum DV_TYPE : int
{
  DV_TYPE_DISPLAY_LED = 0,
  DV_TYPE_PLAYER_LED_LLDV,
  DV_TYPE_PLAYER_LED_HDR,
  DV_TYPE_VS10_ONLY,
  DV_TYPE_PLAYER_LED_HDR2
};

enum DV_COLORIMETRY : int
{
  DV_COLORIMETRY_AMLOGIC = 0,
  DV_COLORIMETRY_BT2020NC,
  DV_COLORIMETRY_REMOVE
};

#define AML_GXBB    0x1F
#define AML_GXL     0x21
#define AML_GXM     0x22
#define AML_G12A    0x28
#define AML_G12B    0x29
#define AML_SM1     0x2B
#define AML_SC2     0x32

#define FLAG_FORCE_DOVI_LL      (unsigned int)(0x4000)
#define FLAG_FORCE_RGB_OUTPUT   (unsigned int)(0x8000)
#define FLAG_TOGGLE_FRAME       (unsigned int)(0x80000000)

#define DOLBY_VISION_LL_DISABLE (unsigned int)(0)
#define DOLBY_VISION_LL_YUV422  (unsigned int)(1)
#define DOLBY_VISION_LL_RGB444  (unsigned int)(2)

#define DOLBY_VISION_FOLLOW_SOURCE     (unsigned int)(1)
#define DOLBY_VISION_FORCE_OUTPUT_MODE (unsigned int)(2)

#define DOLBY_VISION_OUTPUT_MODE_IPT        (unsigned int)(0)
#define DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL (unsigned int)(1)
#define DOLBY_VISION_OUTPUT_MODE_HDR10      (unsigned int)(2)
#define DOLBY_VISION_OUTPUT_MODE_SDR10      (unsigned int)(3)
#define DOLBY_VISION_OUTPUT_MODE_BYPASS     (unsigned int)(5)

int  aml_get_cpufamily_id();
bool aml_display_support_hdr_pq();
bool aml_display_support_hdr_hlg();
bool aml_display_support_hdr10plus();
bool aml_display_support_dv();
struct AMLHdmiAudioCaps
{
  bool valid{false};
  int pcm_ch{0};
  bool pcm_192k{false};
  int truehd_ch{0};
  int ddp_ch{0};
  bool ddp_atmos{false};
  int ac3_ch{0};
  int dtshd_ch{0};
  int dts_ch{0};
};
AMLHdmiAudioCaps aml_get_hdmi_audio_caps();
bool aml_display_support_dv_ll();
bool aml_display_support_dv_std();
bool aml_display_support_3d();
bool aml_display_is_widescreen();
bool aml_display_support_12bit(int force_cs = 0);
bool aml_support_hevc();
bool aml_support_hevc_4k2k();
bool aml_support_hevc_8k4k();
bool aml_support_hevc_10bit();
AML_SUPPORT_H264_4K2K aml_support_h264_4k2k();
bool aml_support_vp9();
bool aml_support_av1();
bool aml_support_dolby_vision();
bool aml_dolby_vision_enabled();
std::string aml_dv_output_mode_to_string(unsigned int mode);
std::string aml_dv_mode_to_string(enum DV_MODE mode);
std::string aml_dv_type_to_string(enum DV_TYPE type);
void aml_dv_set_vs10_mode(unsigned int mode, StreamHdrType hdrType, bool force = false);
void aml_dv_wait_video_off(int timeout);
int aml_blackout_policy(int new_blackout);
void aml_set_mpeg2_keep_progressive(bool enable);
int aml_osd_blank(int fbIndex, int blankMode);
unsigned int aml_dv_on(unsigned int mode, bool force = false);
void aml_dv_off(bool restoreFollows = false);
void aml_set_perf_governors_for_playback();
void aml_restore_governors_after_playback();

unsigned int aml_dv_dolby_vision_mode();
void aml_dv_open(StreamHdrType hdrType, unsigned int bitDepth, AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED, bool swDecoded = false);
void aml_dv_close();
bool aml_dv_decoder_enabled();
bool aml_dv_apply_pending_output();
bool aml_dv_playback_active();
void aml_dv_restore_gui_osd_max();
void aml_dv_set_osd_max(int max);
int aml_dv_osd_max_nits();
bool aml_dv_bdj_overlay_visible();
int  aml_dv_sdr_boost_param();
void aml_dv_set_sdr_source_max_nits(int value);
void aml_apply_hdr10_overrides();
bool aml_is_dv_enable();
bool aml_is_force_422_override_active();
bool aml_dv_player_led_output_active(StreamHdrType hdrType, unsigned int bitDepth);
bool aml_dv_eotf_override_active();
void aml_set_bdj_overlay_active(bool active);
std::string aml_get_plane_state_diag();
void aml_dv_display_trigger();
void aml_dv_display_trigger_tick();
void aml_dv_display_auto_now();

class CAmlHdmiWireGuard
{
public:
  explicit CAmlHdmiWireGuard(const char* tag);
  ~CAmlHdmiWireGuard();
  bool Serialised() const { return m_owns; }
  CAmlHdmiWireGuard(const CAmlHdmiWireGuard&) = delete;
  CAmlHdmiWireGuard& operator=(const CAmlHdmiWireGuard&) = delete;

private:
  const char* m_prevHolder{nullptr};
  int m_prevHolderTid{0};
  bool m_owns{false};
};

class CAmlDvWireStep
{
public:
  explicit CAmlDvWireStep(const char* tag);
  ~CAmlDvWireStep();
  CAmlDvWireStep(const CAmlDvWireStep&) = delete;
  CAmlDvWireStep& operator=(const CAmlDvWireStep&) = delete;

private:
  const char* m_prevTag;
  int64_t m_prevSinceMs;
};
void aml_dv_start();
void aml_dv_set_subtitles(bool visible);
int aml_dv_l5_subs_signal_mode();
void aml_dv_push_l5_flags();
void aml_dv_set_xbmc_osd(bool osd_active);
unsigned int aml_vs10_by_setting(const std::string setting);
enum DV_MODE aml_dv_mode();
enum DV_TYPE aml_dv_type();
bool aml_display_connected();
void aml_dv_enable_fel();
void aml_hevc_nal_skip_policy(const int value);
void aml_set_transfer_pq(StreamHdrType hdrType, unsigned int bitDepth,
                         std::optional<unsigned int> override_mode = std::nullopt);
void aml_set_osd_pq_bypass(StreamHdrType hdrType);
bool aml_gui_pq_is_final_stage();
void aml_set_linux_osd_sdr8(StreamHdrType hdrType, unsigned int bitDepth = 0,
                            std::optional<unsigned int> override_mode = std::nullopt);
StreamHdrType aml_get_output_hdr_type(StreamHdrType sourceType);
bool aml_has_frac_rate_policy();
void aml_video_mute(bool mute);
void aml_set_audio_passthrough(bool passthrough);
void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode);
bool aml_mode_to_resolution(const char *mode, RESOLUTION_INFO &res);
bool aml_get_native_resolution(RESOLUTION_INFO &res);
bool aml_set_native_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name, const int stereo_mode, bool force_mode_switch);
bool aml_probe_resolutions(std::vector<RESOLUTION_INFO> &resolutions);
bool aml_set_display_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name, bool force_mode_switch);
void aml_handle_scale(const RESOLUTION_INFO &res);
void aml_handle_display_stereo_mode(const int stereo_mode);
void aml_enable_freeScale(const RESOLUTION_INFO &res);
void aml_disable_freeScale();
void aml_set_framebuffer_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name);
void aml_set_framebuffer_resolution(unsigned int width, unsigned int height, std::string framebuffer_name);
bool aml_read_reg(const std::string &reg, uint32_t &reg_val);
bool aml_has_capability_ignore_alpha();
bool aml_set_reg_ignore_alpha();
bool aml_unset_reg_ignore_alpha();
std::string aml_video_fps_info();
std::string aml_video_fps_drop();
bool aml_get_vsync_edge(int64_t& lastVsyncTs);
bool aml_get_time_until_vsync_phase_us(int afterVsyncUs, int& timeUntilPhaseUs);
unsigned int aml_dv_video_processor_mode();
void aml_dv_set_disc_session(bool active);

bool aml_dv_vs10_converting();
bool aml_dv_refresh_vs10_converting(StreamHdrType hdrType, unsigned int bitDepth);
bool aml_dv_vsvdb_v1_enabled();
void aml_dv_write_vsvdb_policy();
void set_vsvdb_payload_ver(enum DV_TYPE dv_type, int max_lum_nits_value, int source_max_pq);
void CalculateVSVDBPayload();
void CalculateVSVDBPayload_2(enum DV_TYPE dv_type);

void aml_set_audio_ddr_urgent(bool enable);
void aml_reset_audio_from_vs10_change();
void aml_reset_audio_from_player_open();
void aml_reset_audio_from_player_pause();
void aml_reset_audio_from_window_home();
void aml_reset_audio_from_play_from_beginning();
void aml_reset_audio_from_play_from_resume();
void aml_reset_from_subtitle_change();

void aml_get_dv_cap();
/*!
 * \brief Read base-EDID chromaticity (bytes 0x19-0x22) from amhdmitx rawedid
 *        and quantize into xbmc_dv_cap Rx/Ry/Gx/Gy/Bx/By VSVDB codes.
 * \return true when primaries were filled and are within VSVDB bit ranges.
 */
bool aml_fill_vsvdb_primaries_from_edid();
/*!
 * \brief Prefer DV VSVDB primaries from dv_cap; if unavailable, use base EDID.
 */
bool aml_ensure_display_vsvdb_primaries();
struct xbmc_dv_cap
{
  static inline int dv_ver_i = 0;
  static inline int dv_len_i = 0;
  static inline int dv_max_v1_i = 0;
  static inline int dv_max_v2_i = 0;
  static inline int dv_rx_i = 0;
  static inline int dv_ry_i = 0;
  static inline int dv_gx_i = 0;
  static inline int dv_gy_i = 0;
  static inline int dv_bx_i = 0;
  static inline int dv_by_i = 0;
  static inline std::string dv_vsvdb_s = "";
};
