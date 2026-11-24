/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FileItem.h"
#include "filesystem/Directory.h"
#include "filesystem/FileDirectoryFactory.h"
#include "filesystem/IDirectory.h"
#include "filesystem/SpecialProtocol.h"
#include "test/TestUtils.h"
#include "utils/URIUtils.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{
void AppendLE16(std::string& out, uint16_t v)
{
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void AppendLE32(std::string& out, uint32_t v)
{
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void WriteStoredZip(const std::string& path,
                    const std::vector<std::pair<std::string, std::string>>& entries)
{
  std::string data;
  std::string centralDir;
  for (const auto& entry : entries)
  {
    const uint32_t localOffset = static_cast<uint32_t>(data.size());
    const uint16_t nameLen = static_cast<uint16_t>(entry.first.size());
    const uint32_t bodyLen = static_cast<uint32_t>(entry.second.size());

    // Local file header (STORED, method=0)
    AppendLE32(data, 0x04034b50);
    AppendLE16(data, 20);
    AppendLE16(data, 0);
    AppendLE16(data, 0);
    AppendLE16(data, 0);
    AppendLE16(data, 0);
    AppendLE32(data, 0);
    AppendLE32(data, bodyLen);
    AppendLE32(data, bodyLen);
    AppendLE16(data, nameLen);
    AppendLE16(data, 0);
    data.append(entry.first);
    data.append(entry.second);

    // Central directory header
    AppendLE32(centralDir, 0x02014b50);
    AppendLE16(centralDir, 20);
    AppendLE16(centralDir, 20);
    AppendLE16(centralDir, 0);
    AppendLE16(centralDir, 0);
    AppendLE16(centralDir, 0);
    AppendLE16(centralDir, 0);
    AppendLE32(centralDir, 0);
    AppendLE32(centralDir, bodyLen);
    AppendLE32(centralDir, bodyLen);
    AppendLE16(centralDir, nameLen);
    AppendLE16(centralDir, 0);
    AppendLE16(centralDir, 0);
    AppendLE16(centralDir, 0);
    AppendLE16(centralDir, 0);
    AppendLE32(centralDir, 0);
    AppendLE32(centralDir, localOffset);
    centralDir.append(entry.first);
  }

  const uint32_t cdOffset = static_cast<uint32_t>(data.size());
  const uint32_t cdSize = static_cast<uint32_t>(centralDir.size());
  data.append(centralDir);

  // End of central directory record
  AppendLE32(data, 0x06054b50);
  AppendLE16(data, 0);
  AppendLE16(data, 0);
  AppendLE16(data, static_cast<uint16_t>(entries.size()));
  AppendLE16(data, static_cast<uint16_t>(entries.size()));
  AppendLE32(data, cdSize);
  AppendLE32(data, cdOffset);
  AppendLE16(data, 0);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}
} // namespace

TEST(TestDirectory, General)
{
  std::string tmppath1, tmppath2, tmppath3;
  CFileItemList items;
  CFileItemPtr itemptr;
  tmppath1 = CSpecialProtocol::TranslatePath("special://temp/");
  tmppath1 = URIUtils::AddFileToFolder(tmppath1, "TestDirectory");
  tmppath2 = tmppath1;
  tmppath2 = URIUtils::AddFileToFolder(tmppath2, "subdir");
  EXPECT_TRUE(XFILE::CDirectory::Create(tmppath1));
  EXPECT_TRUE(XFILE::CDirectory::Exists(tmppath1));
  EXPECT_FALSE(XFILE::CDirectory::Exists(tmppath2));
  EXPECT_TRUE(XFILE::CDirectory::Create(tmppath2));
  EXPECT_TRUE(XFILE::CDirectory::Exists(tmppath2));
  EXPECT_TRUE(XFILE::CDirectory::GetDirectory(tmppath1, items, "", XFILE::DIR_FLAG_DEFAULTS));
  XFILE::CDirectory::FilterFileDirectories(items, "");
  tmppath3 = tmppath2;
  URIUtils::AddSlashAtEnd(tmppath3);
  itemptr = items[0];
  EXPECT_STREQ(tmppath3.c_str(), itemptr->GetPath().c_str());
  EXPECT_TRUE(XFILE::CDirectory::Remove(tmppath2));
  EXPECT_FALSE(XFILE::CDirectory::Exists(tmppath2));
  EXPECT_TRUE(XFILE::CDirectory::Exists(tmppath1));
  EXPECT_TRUE(XFILE::CDirectory::Remove(tmppath1));
  EXPECT_FALSE(XFILE::CDirectory::Exists(tmppath1));
}

TEST(TestDirectory, CreateRecursive)
{
  auto path1 = URIUtils::AddFileToFolder(
    CSpecialProtocol::TranslatePath("special://temp/"),
    "level1");
  auto path2 = URIUtils::AddFileToFolder(path1,
    "level2",
    "level3");

  EXPECT_TRUE(XFILE::CDirectory::Create(path2));
  EXPECT_TRUE(XFILE::CDirectory::RemoveRecursive(path1));
}

TEST(TestDirectory, ArchivesNotAutoExpandedOnListing)
{
  const std::string zipPath = XBMC_REF_FILE_PATH("xbmc/filesystem/test/reffile.txt.zip");
  const std::string dirPath = URIUtils::GetDirectory(zipPath);

  CFileItemList items;
  ASSERT_TRUE(XFILE::CDirectory::GetDirectory(dirPath, items, "", XFILE::DIR_FLAG_DEFAULTS));

  CFileItemPtr zipItem;
  for (int i = 0; i < items.Size(); ++i)
  {
    if (items[i]->GetPath() == zipPath)
    {
      zipItem = items[i];
      break;
    }
  }

  ASSERT_NE(zipItem, nullptr);
  EXPECT_FALSE(zipItem->m_bIsFolder);
  EXPECT_TRUE(zipItem->IsArchiveFileFolder());
  EXPECT_FALSE(zipItem->IsFileFolder(EFILEFOLDER_TYPE_ALWAYS));
  EXPECT_TRUE(zipItem->IsFileFolder(EFILEFOLDER_MASK_ONCLICK));

  CFileItem resolved(*zipItem);
  std::unique_ptr<XFILE::IFileDirectory> dir(
      XFILE::CFileDirectoryFactory::Create(resolved.GetURL(), &resolved, ""));
  EXPECT_TRUE(dir != nullptr || resolved.GetPath() != zipPath);

  const std::string tempDir = URIUtils::AddFileToFolder(
      CSpecialProtocol::TranslatePath("special://temp/"), "TestArchiveDefer");
  XFILE::CDirectory::RemoveRecursive(tempDir);
  ASSERT_TRUE(XFILE::CDirectory::Create(tempDir));

  const std::string singleZip = URIUtils::AddFileToFolder(tempDir, "single.zip");
  const std::string multiZip = URIUtils::AddFileToFolder(tempDir, "multi.zip");
  const std::string emptyZip = URIUtils::AddFileToFolder(tempDir, "empty.zip");
  WriteStoredZip(singleZip, {{"clip.mkv", ""}});
  WriteStoredZip(multiZip, {{"clip1.mkv", ""}, {"clip2.mkv", ""}});
  WriteStoredZip(emptyZip, {});

  CFileItemList listed;
  ASSERT_TRUE(XFILE::CDirectory::GetDirectory(tempDir, listed, "", XFILE::DIR_FLAG_DEFAULTS));
  ASSERT_EQ(listed.Size(), 3);
  for (int i = 0; i < listed.Size(); ++i)
  {
    EXPECT_FALSE(listed[i]->m_bIsFolder) << listed[i]->GetPath();
    EXPECT_TRUE(listed[i]->IsArchiveFileFolder()) << listed[i]->GetPath();
  }

  CFileItem singleItem(singleZip, false);
  std::unique_ptr<XFILE::IFileDirectory> singleDir(
      XFILE::CFileDirectoryFactory::Create(singleItem.GetURL(), &singleItem, ""));
  EXPECT_NE(singleDir, nullptr);
  EXPECT_NE(singleItem.GetPath(), singleZip);
  EXPECT_TRUE(URIUtils::HasSlashAtEnd(singleItem.GetPath()));

  CFileItem multiItem(multiZip, false);
  std::unique_ptr<XFILE::IFileDirectory> multiDir(
      XFILE::CFileDirectoryFactory::Create(multiItem.GetURL(), &multiItem, ""));
  EXPECT_NE(multiDir, nullptr);
  EXPECT_NE(multiItem.GetPath(), multiZip);

  CFileItem emptyItem(emptyZip, false);
  std::unique_ptr<XFILE::IFileDirectory> emptyDir(
      XFILE::CFileDirectoryFactory::Create(emptyItem.GetURL(), &emptyItem, ""));
  EXPECT_EQ(emptyDir, nullptr);
  EXPECT_TRUE(emptyItem.m_bIsFolder);

  EXPECT_TRUE(XFILE::CDirectory::RemoveRecursive(tempDir));
}
