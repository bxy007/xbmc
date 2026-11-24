/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FileItem.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/SettingsManager.h"

#include <gtest/gtest.h>

using ::testing::Test;
using ::testing::WithParamInterface;
using ::testing::ValuesIn;

struct TestFileData
{
  const char *file;
  bool use_folder;
  const char *base;
};

class AdvancedSettingsResetBase : public Test
{
public:
  AdvancedSettingsResetBase();
};

AdvancedSettingsResetBase::AdvancedSettingsResetBase()
{
  // Force all advanced settings to be reset to defaults
  const auto settings = CServiceBroker::GetSettingsComponent();
  CSettingsManager* settingsMgr = settings->GetSettings()->GetSettingsManager();
  settings->GetAdvancedSettings()->Uninitialize(*settingsMgr);
  settings->GetAdvancedSettings()->Initialize(*settingsMgr);
}

class TestFileItemSpecifiedArtJpg : public AdvancedSettingsResetBase,
                                    public WithParamInterface<TestFileData>
{
};


TEST_P(TestFileItemSpecifiedArtJpg, GetLocalArt)
{
  CFileItem item;
  item.SetPath(GetParam().file);
  std::string path = CURL(item.GetLocalArt("art.jpg", GetParam().use_folder)).Get();
  std::string compare = CURL(GetParam().base).Get();
  EXPECT_EQ(compare, path);
}

const TestFileData MovieFiles[] = {{ "c:\\dir\\filename.avi", false, "c:\\dir\\filename-art.jpg" },
                                   { "c:\\dir\\filename.avi", true,  "c:\\dir\\art.jpg" },
                                   { "/dir/filename.avi", false, "/dir/filename-art.jpg" },
                                   { "/dir/filename.avi", true,  "/dir/art.jpg" },
                                   { "smb://somepath/file.avi", false, "smb://somepath/file-art.jpg" },
                                   { "smb://somepath/file.avi", true, "smb://somepath/art.jpg" },
                                   { "stack:///path/to/movie-cd1.avi , /path/to/movie-cd2.avi", false,  "/path/to/movie-art.jpg" },
                                   { "stack:///path/to/movie-cd1.avi , /path/to/movie-cd2.avi", true,  "/path/to/art.jpg" },
                                   { "stack:///path/to/movie_name/cd1/some_file1.avi , /path/to/movie_name/cd2/some_file2.avi", true,  "/path/to/movie_name/art.jpg" },
                                   { "/home/user/TV Shows/Dexter/S1/1x01.avi", false, "/home/user/TV Shows/Dexter/S1/1x01-art.jpg" },
                                   { "/home/user/TV Shows/Dexter/S1/1x01.avi", true, "/home/user/TV Shows/Dexter/S1/art.jpg" },
                                   { "zip://g%3a%5cmultimedia%5cmovies%5cSphere%2ezip/Sphere.avi", false, "g:\\multimedia\\movies\\Sphere-art.jpg" },
                                   { "zip://g%3a%5cmultimedia%5cmovies%5cSphere%2ezip/Sphere.avi", true, "g:\\multimedia\\movies\\art.jpg" },
                                   { "/home/user/movies/movie_name/video_ts/VIDEO_TS.IFO", false, "/home/user/movies/movie_name/art.jpg" },
                                   { "/home/user/movies/movie_name/video_ts/VIDEO_TS.IFO", true, "/home/user/movies/movie_name/art.jpg" },
                                   { "/home/user/movies/movie_name/BDMV/index.bdmv", false, "/home/user/movies/movie_name/art.jpg" },
                                   { "/home/user/movies/movie_name/BDMV/index.bdmv", true, "/home/user/movies/movie_name/art.jpg" }};

INSTANTIATE_TEST_SUITE_P(MovieFiles, TestFileItemSpecifiedArtJpg, ValuesIn(MovieFiles));

class TestFileItemFallbackArt : public AdvancedSettingsResetBase,
                                public WithParamInterface<TestFileData>
{
};

TEST_P(TestFileItemFallbackArt, GetLocalArt)
{
  CFileItem item;
  item.SetPath(GetParam().file);
  std::string path = CURL(item.GetLocalArt("", GetParam().use_folder)).Get();
  std::string compare = CURL(GetParam().base).Get();
  EXPECT_EQ(compare, path);
}

const TestFileData NoArtFiles[] = {{ "c:\\dir\\filename.avi", false, "c:\\dir\\filename.tbn" },
                                   { "/dir/filename.avi", false, "/dir/filename.tbn" },
                                   { "smb://somepath/file.avi", false, "smb://somepath/file.tbn" },
                                   { "/home/user/TV Shows/Dexter/S1/1x01.avi", false, "/home/user/TV Shows/Dexter/S1/1x01.tbn" },
                                   { "zip://g%3a%5cmultimedia%5cmovies%5cSphere%2ezip/Sphere.avi", false, "g:\\multimedia\\movies\\Sphere.tbn" }};

INSTANTIATE_TEST_SUITE_P(NoArt, TestFileItemFallbackArt, ValuesIn(NoArtFiles));

class TestFileItemBasePath : public AdvancedSettingsResetBase,
                             public WithParamInterface<TestFileData>
{
};

TEST_P(TestFileItemBasePath, GetBaseMoviePath)
{
  CFileItem item;
  item.SetPath(GetParam().file);
  std::string path = CURL(item.GetBaseMoviePath(GetParam().use_folder)).Get();
  std::string compare = CURL(GetParam().base).Get();
  EXPECT_EQ(compare, path);
}

const TestFileData BaseMovies[] = {{ "c:\\dir\\filename.avi", false, "c:\\dir\\filename.avi" },
                                   { "c:\\dir\\filename.avi", true,  "c:\\dir\\" },
                                   { "/dir/filename.avi", false, "/dir/filename.avi" },
                                   { "/dir/filename.avi", true,  "/dir/" },
                                   { "smb://somepath/file.avi", false, "smb://somepath/file.avi" },
                                   { "smb://somepath/file.avi", true, "smb://somepath/" },
                                   { "stack:///path/to/movie_name/cd1/some_file1.avi , /path/to/movie_name/cd2/some_file2.avi", false, "stack:///path/to/movie_name/cd1/some_file1.avi , /path/to/movie_name/cd2/some_file2.avi" },
                                   { "stack:///path/to/movie_name/cd1/some_file1.avi , /path/to/movie_name/cd2/some_file2.avi", true,  "/path/to/movie_name/" },
                                   { "/home/user/TV Shows/Dexter/S1/1x01.avi", false, "/home/user/TV Shows/Dexter/S1/1x01.avi" },
                                   { "/home/user/TV Shows/Dexter/S1/1x01.avi", true, "/home/user/TV Shows/Dexter/S1/" },
                                   { "zip://g%3a%5cmultimedia%5cmovies%5cSphere%2ezip/Sphere.avi", true, "g:\\multimedia\\movies\\" },
                                   { "/home/user/movies/movie_name/video_ts/VIDEO_TS.IFO", false, "/home/user/movies/movie_name/" },
                                   { "/home/user/movies/movie_name/video_ts/VIDEO_TS.IFO", true, "/home/user/movies/movie_name/" },
                                   { "/home/user/movies/movie_name/BDMV/index.bdmv", false, "/home/user/movies/movie_name/" },
                                   { "/home/user/movies/movie_name/BDMV/index.bdmv", true, "/home/user/movies/movie_name/" }};

INSTANTIATE_TEST_SUITE_P(BaseNameMovies, TestFileItemBasePath, ValuesIn(BaseMovies));

TEST_F(AdvancedSettingsResetBase, ArchiveFileFolderDeferredToOnClick)
{
  const char* archivePaths[] = {
      "/media/movies/sample.zip", "/media/movies/sample.rar", "/media/movies/sample.cbz",
      "/media/movies/sample.cbr", "/media/movies/sample.apk", "dav://192.168.50.101/video/test.zip",
  };
  for (const char* path : archivePaths)
  {
    CFileItem item(path, false);
    EXPECT_TRUE(item.IsArchiveFileFolder()) << path;
    EXPECT_FALSE(item.IsFileFolder(EFILEFOLDER_TYPE_ALWAYS)) << path;
    EXPECT_TRUE(item.IsFileFolder(EFILEFOLDER_MASK_ONCLICK)) << path;
    EXPECT_TRUE(item.IsFileFolder(EFILEFOLDER_MASK_ONBROWSE)) << path;
    EXPECT_TRUE(item.IsFileFolder(EFILEFOLDER_MASK_ALL)) << path;
  }

  CFileItem discImage("/media/movies/movie.iso", false);
  EXPECT_FALSE(discImage.IsArchiveFileFolder());
  EXPECT_FALSE(discImage.IsFileFolder(EFILEFOLDER_TYPE_ALWAYS));
  EXPECT_FALSE(discImage.IsFileFolder(EFILEFOLDER_MASK_ONCLICK));
  EXPECT_TRUE(discImage.IsFileFolder(EFILEFOLDER_MASK_ONBROWSE));

  CFileItem smartPlaylist("special://videoplaylists/test.xsp", false);
  EXPECT_FALSE(smartPlaylist.IsArchiveFileFolder());
  EXPECT_TRUE(smartPlaylist.IsFileFolder(EFILEFOLDER_TYPE_ALWAYS));

  CFileItem audioBook("/media/books/book.m4b", false);
  EXPECT_FALSE(audioBook.IsArchiveFileFolder());
  EXPECT_TRUE(audioBook.IsFileFolder(EFILEFOLDER_TYPE_ALWAYS));

  CFileItem movie("/media/movies/movie.mkv", false);
  EXPECT_FALSE(movie.IsArchiveFileFolder());
  EXPECT_FALSE(movie.IsFileFolder(EFILEFOLDER_MASK_ALL));

  // .ts.001 recordings (including URLs with query/protocol options) must not be treated as .001 archives
  CFileItem tsPartLocal("/media/tv/recording.ts.001", false);
  EXPECT_FALSE(tsPartLocal.IsRAR());
  EXPECT_FALSE(tsPartLocal.IsArchiveFileFolder());

  CFileItem tsPartUrl("http://example.com/tv/recording.ts.001?token=abc|User-Agent=test", false);
  EXPECT_FALSE(tsPartUrl.IsRAR());
  EXPECT_FALSE(tsPartUrl.IsArchiveFileFolder());

  CFileItem splitArchiveUrl("http://example.com/movies/archive.001?token=abc|User-Agent=test",
                            false);
  EXPECT_TRUE(splitArchiveUrl.IsRAR());
  EXPECT_TRUE(splitArchiveUrl.IsArchiveFileFolder());
}

TEST_F(AdvancedSettingsResetBase, ArchiveFilesNotStacked)
{
  // Plain archives must not stack into stack://
  CFileItemList zipItems("/media/movies/");
  zipItems.Add(std::make_shared<CFileItem>("/media/movies/Film CD1.zip", false));
  zipItems.Add(std::make_shared<CFileItem>("/media/movies/Film CD2.zip", false));
  zipItems.Stack();

  ASSERT_EQ(zipItems.Size(), 2);
  EXPECT_FALSE(zipItems[0]->IsStack());
  EXPECT_FALSE(zipItems[1]->IsStack());
  EXPECT_EQ(zipItems[0]->GetPath(), "/media/movies/Film CD1.zip");
  EXPECT_EQ(zipItems[1]->GetPath(), "/media/movies/Film CD2.zip");

  // Stacking video parts with an interleaved archive must remove only the stacked video part
  CFileItemList mixedItems("/media/movies/");
  mixedItems.Add(std::make_shared<CFileItem>("/media/movies/Film CD1.mkv", false));
  mixedItems.Add(std::make_shared<CFileItem>("/media/movies/Film CD1.zip", false));
  mixedItems.Add(std::make_shared<CFileItem>("/media/movies/Film CD2.mkv", false));
  mixedItems.Stack();

  ASSERT_EQ(mixedItems.Size(), 2);
  EXPECT_TRUE(mixedItems[0]->IsStack());
  EXPECT_EQ(mixedItems[0]->GetPath(),
            "stack:///media/movies/Film CD1.mkv , /media/movies/Film CD2.mkv");
  EXPECT_FALSE(mixedItems[1]->IsStack());
  EXPECT_EQ(mixedItems[1]->GetPath(), "/media/movies/Film CD1.zip");

  // Independent RARs (Film CD1/CD2) must not be hidden, while .partN.rar sets keep dedup
  CFileItemList rarItems("/media/movies/");
  rarItems.Add(std::make_shared<CFileItem>("/media/movies/Film CD1.rar", false));
  rarItems.Add(std::make_shared<CFileItem>("/media/movies/Film CD2.rar", false));
  rarItems.Add(std::make_shared<CFileItem>("/media/movies/Multi.part1.rar", false));
  rarItems.Add(std::make_shared<CFileItem>("/media/movies/Multi.part2.rar", false));
  rarItems.Stack();

  ASSERT_EQ(rarItems.Size(), 3);
  EXPECT_EQ(rarItems[0]->GetPath(), "/media/movies/Film CD1.rar");
  EXPECT_EQ(rarItems[1]->GetPath(), "/media/movies/Film CD2.rar");
  EXPECT_EQ(rarItems[2]->GetPath(), "/media/movies/Multi.part1.rar");

  // Multi-volume RARs with query parameters and/or protocol options must still deduplicate
  CFileItemList urlRarItems("http://example.com/movies/");
  urlRarItems.Add(std::make_shared<CFileItem>(
      "http://example.com/movies/Multi.part1.rar?token=abc|User-Agent=test", false));
  urlRarItems.Add(std::make_shared<CFileItem>(
      "http://example.com/movies/Multi.part2.rar?token=abc|User-Agent=test", false));
  urlRarItems.Stack();

  ASSERT_EQ(urlRarItems.Size(), 1);
  EXPECT_EQ(urlRarItems[0]->GetPath(),
            "http://example.com/movies/Multi.part1.rar?token=abc|User-Agent=test");
}
