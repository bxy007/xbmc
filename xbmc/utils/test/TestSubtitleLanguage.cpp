/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/SubtitleLanguage.h"

#include <gtest/gtest.h>

TEST(TestSubtitleLanguage, ChineseCodes)
{
  for (const auto code : {"zh", "chi", "zho", "ZH-Hans-CN", "zho-Hant", "zh_CN", "chs", "cht"})
    EXPECT_TRUE(CSubtitleLanguage::IsChinese(code)) << code;
  for (const auto code : {"", "und", "eng", "jpn", "zhuang", "Chinese"})
    EXPECT_FALSE(CSubtitleLanguage::IsChinese(code)) << code;
}

TEST(TestSubtitleLanguage, ExplicitScriptAndRegion)
{
  for (const auto code : {"zh-Hans", "chi-Hans", "ZH_hans_CN", "chs", "zh-CN", "zh-SG"})
    EXPECT_TRUE(CSubtitleLanguage::IsSimplifiedChinese(code, "")) << code;
  for (const auto code : {"zh-Hant", "zho-Hant", "cht", "zh-TW", "zh-HK", "zh-MO",
                          "chi", "zh", "zho", "zh-Hans-Hant"})
    EXPECT_FALSE(CSubtitleLanguage::IsSimplifiedChinese(code, "")) << code;
}

TEST(TestSubtitleLanguage, TrackLabels)
{
  for (const auto title : {"简体中文", "簡體中文", "简中", "简英双语", "中简", "CHS",
                           "Chinese (Simple)", "Chinese (Simplified)", "[chs-eng]", "简体（SDR）"})
    EXPECT_TRUE(CSubtitleLanguage::IsSimplifiedChinese("chi", title)) << title;
  for (const auto title : {"繁體中文", "繁中", "繁英双语", "CHT", "Chinese (Traditional)",
                           "", "中文", "Simplified / Traditional", "简体／繁體", "schs", "Simpleton"})
    EXPECT_FALSE(CSubtitleLanguage::IsSimplifiedChinese("chi", title)) << title;
  EXPECT_FALSE(CSubtitleLanguage::IsSimplifiedChinese("eng", "简体中文"));
  EXPECT_FALSE(CSubtitleLanguage::IsSimplifiedChinese("und", "CHS"));
}

TEST(TestSubtitleLanguage, EvidencePrecedence)
{
  EXPECT_TRUE(CSubtitleLanguage::IsSimplifiedChinese("zh-Hans", "繁體"));
  EXPECT_FALSE(CSubtitleLanguage::IsSimplifiedChinese("zh-Hant", "简体"));
  EXPECT_FALSE(CSubtitleLanguage::IsSimplifiedChinese("zh-CN", "繁體"));
  EXPECT_TRUE(CSubtitleLanguage::IsSimplifiedChinese("zh-TW", "简体"));
}
