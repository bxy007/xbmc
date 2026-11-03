/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string_view>

class CSubtitleLanguage
{
public:
  // Recognize Chinese language codes, including script/region subtags.
  static bool IsChinese(std::string_view language);

  // Use explicit script tags, then title labels, then region hints. Do not
  // guess the writing system of an otherwise unlabelled Chinese track.
  static bool IsSimplifiedChinese(std::string_view language, std::string_view title);
};
