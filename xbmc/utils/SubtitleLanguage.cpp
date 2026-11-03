/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SubtitleLanguage.h"

#include <array>
#include <string>

namespace
{
std::string LowerASCII(std::string_view value)
{
  std::string lower(value);
  for (char& c : lower)
  {
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  }
  return lower;
}

bool IsASCIIAlphanumeric(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool HasWord(std::string_view value, std::string_view word)
{
  size_t offset = 0;
  while ((offset = value.find(word, offset)) != std::string_view::npos)
  {
    const size_t end = offset + word.size();
    if ((offset == 0 || !IsASCIIAlphanumeric(value[offset - 1])) &&
        (end == value.size() || !IsASCIIAlphanumeric(value[end])))
      return true;
    offset = end;
  }
  return false;
}

bool HasSubtag(std::string_view language, std::string_view subtag)
{
  size_t offset = language.find_first_of("-_");
  while (offset != std::string_view::npos)
  {
    const size_t next = language.find_first_of("-_", offset + 1);
    if (language.substr(offset + 1, next - offset - 1) == subtag)
      return true;
    offset = next;
  }
  return false;
}
} // namespace

bool CSubtitleLanguage::IsChinese(std::string_view language)
{
  const std::string primary = LowerASCII(language.substr(0, language.find_first_of("-_")));
  return primary == "zh" || primary == "chi" || primary == "zho" || primary == "chs" ||
         primary == "cht";
}

bool CSubtitleLanguage::IsSimplifiedChinese(std::string_view language, std::string_view title)
{
  if (!IsChinese(language))
    return false;

  const std::string code = LowerASCII(language);
  const bool hans = HasSubtag(code, "hans") || code == "chs";
  const bool hant = HasSubtag(code, "hant") || code == "cht";
  if (hans || hant)
    return hans && !hant;

  const std::string label = LowerASCII(title);
  constexpr std::array<std::string_view, 8> simplifiedLabels{
      "简体", "簡體", "简中", "簡中", "简英", "簡英", "中简", "中簡"};
  constexpr std::array<std::string_view, 5> traditionalLabels{
      "繁体", "繁體", "繁中", "繁英", "中繁"};
  bool simplified = HasWord(label, "chs") || HasWord(label, "simplified") ||
                    HasWord(label, "simple");
  bool traditional = HasWord(label, "cht") || HasWord(label, "traditional");
  for (const auto marker : simplifiedLabels)
    simplified |= label.find(marker) != std::string::npos;
  for (const auto marker : traditionalLabels)
    traditional |= label.find(marker) != std::string::npos;
  if (simplified || traditional)
    return simplified && !traditional;

  // A region is weaker evidence than an explicit script or track title.
  return (HasSubtag(code, "cn") || HasSubtag(code, "sg")) &&
         !HasSubtag(code, "tw") && !HasSubtag(code, "hk") && !HasSubtag(code, "mo");
}
