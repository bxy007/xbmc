/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef KODI_PGS_IDENTIFY_H
#define KODI_PGS_IDENTIFY_H

#include <cstddef>
#include <stdbool.h>
#include <stdint.h>
#include <string>
#include <vector>

enum pgs_sub_script
{
  PGS_SUB_SCRIPT_UNKNOWN,
  PGS_SUB_SCRIPT_SIMPLIFIED,
  PGS_SUB_SCRIPT_TRADITIONAL,
  PGS_SUB_SCRIPT_MIXED,
};

#define PGS_SUB_SCRIPT_CONFIRM_CHARS 5

struct pgs_sub_script_evidence
{
  uint32_t seen[128];
  int num_seen, simplified, traditional, captions;
  int simplified_captions, traditional_captions;
  enum pgs_sub_script last_caption;
  enum pgs_sub_script run_script, confirmed;
  int run_chars;
  uint64_t recent[8];
  unsigned next_recent;
};

struct pgs_sub_script_confirmation
{
  enum pgs_sub_script candidate;
  int observed_captions, streak;
};

// Five consecutive script-specific characters; shared characters do not vote.
bool pgs_sub_script_confirm(struct pgs_sub_script_confirmation* state,
                            const struct pgs_sub_script_evidence* evidence);

struct pgs_subtitle_info
{
  bool active;
  enum pgs_sub_script script;
  bool script_from_title;
  bool descriptive_title;
  bool bilingual; // separate English line or repeated mixed-caption evidence
  bool forced; // container/title or at least one PGS forced object
  bool signs_from_title;
  bool ocr_available;
  bool sampled; // Sampling finished (confirmed or budget); not a full scan
  int samples;
};

// Per-caption language counts from confident OCR tokens.
struct pgs_sub_caption_language
{
  int han_chars, latin_letters, latin_words;
  bool english_second_line;
};
struct pgs_sub_bilingual_evidence
{
  int bilingual;
  bool english_line, chinese;
};
int pgs_sub_word_language(const char* text, struct pgs_sub_caption_language* caption);
// Second physical text line: confident ASCII English, allowing punctuation/digits.
void pgs_sub_line_language(const char* text, int line, int minimumConfidence,
                           struct pgs_sub_caption_language* caption);
// 0: not a candidate; 1: Chinese; 2: unknown language. Shared by registration and selection.
int pgs_sub_candidate_language(const std::string& language, const std::string& title);
bool pgs_sub_matches_preference(const pgs_subtitle_info& info, int preference);
// Exact type first; paired mono/bilingual fallback only after potential exact
// candidates finish. The indices correspond to the player's subtitle list.
int pgs_sub_select_preference(const std::vector<pgs_subtitle_info>& candidates,
                              int preference, int current);
int pgs_sub_priority(const pgs_subtitle_info& info, bool chinese, bool multiline, int preference);
void pgs_sub_bilingual_feed(struct pgs_sub_bilingual_evidence* e,
                            const struct pgs_sub_caption_language* caption,
                            struct pgs_subtitle_info* info);

void pgs_sub_script_feed(struct pgs_sub_script_evidence* e, const char* text, int confidence);
enum pgs_sub_script pgs_sub_script_result(const struct pgs_sub_script_evidence* e);
void pgs_subtitle_info_from_metadata(struct pgs_subtitle_info* info,
                                     const char* lang,
                                     const char* title,
                                     bool forced);
const char* pgs_sub_script_name(enum pgs_sub_script script);

// Same label composition as mpv; localization is supplied by the caller.
std::string pgs_subtitle_info_label(const pgs_subtitle_info& info,
                                    std::string (*translate)(uint32_t));
bool pgs_metadata_describes(const char* lang, const char* title);
bool pgs_packet_has_forced(const uint8_t* data, size_t size);
// A nonempty PCS is data activity even when no usable OCR characters result.
bool pgs_packet_has_objects(const uint8_t* data, size_t size);
#endif
