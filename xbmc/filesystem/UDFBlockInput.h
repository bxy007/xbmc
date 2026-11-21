/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

#include <udfread/blockinput.h>

namespace XFILE
{
class CFile;
}

/*!
 \brief A part of the image, held so that UDF's reads do not each cost a request to the source.
 */
struct CUDFBlockInputPage
{
  std::vector<uint8_t> data;

  //! Bytes the image holds here, short for the last page of an image that does not end on one
  size_t valid{0};

  //! True when held in the protected (hot/metadata) SLRU segment
  bool hot{false};

  //! True once the page has actually been read by a caller (false while merely prefetched)
  bool accessed{false};

  //! Position in either hotLru or coldLru
  std::list<uint64_t>::iterator lruIt;
};

class CUDFBlockInput
{
public:
  /*!
   \brief Marks reads on the current thread as large sequential media stream reads
   (.m2ts/.ssif/.vob), routing them to the dedicated streaming channel so they neither contend
   with nor evict the UDF metadata / BD-J page cache.
   */
  class ScopedStreamRead
  {
  public:
    ScopedStreamRead();
    ~ScopedStreamRead();
    ScopedStreamRead(const ScopedStreamRead&) = delete;
    ScopedStreamRead& operator=(const ScopedStreamRead&) = delete;
  };

  static bool IsStreamRead();

  CUDFBlockInput() = default;
  ~CUDFBlockInput() = default;

  udfread_block_input* GetBlockInput(const std::string& file);


private:
  struct UDF_BI;

  static int Close(udfread_block_input* bi);
  static uint32_t Size(udfread_block_input* bi);
  static int Read(udfread_block_input* bi, uint32_t lba, void* buf, uint32_t nblocks, int flags);
  static int ReadStream(UDF_BI& bi, uint32_t lba, void* buf, uint32_t blocks);

  static bool IsPinnedPage(const UDF_BI& bi, uint64_t page);
  static size_t ExpectedPageBytes(const UDF_BI& bi, uint64_t page);
  static void TrimColdLocked(UDF_BI& bi);
  static void DemoteHotToColdLocked(UDF_BI& bi);
  static void InsertPageLocked(UDF_BI& bi,
                               uint64_t page,
                               std::vector<uint8_t>&& data,
                               size_t valid,
                               bool demanded);
  static CUDFBlockInputPage* LookupCachedPageLocked(UDF_BI& bi, uint64_t page);
  static void SchedulePrefetchLocked(UDF_BI& bi, uint64_t startPage);
  static ssize_t FetchRawIoLocked(UDF_BI& bi,
                                  uint64_t page,
                                  size_t fetchPages,
                                  std::vector<uint8_t>& raw,
                                  const std::atomic<bool>* cancelFlag = nullptr);
  static bool FetchForegroundLocked(UDF_BI& bi,
                                    uint64_t page,
                                    size_t minPagesNeeded,
                                    size_t from,
                                    size_t want,
                                    uint8_t* out,
                                    size_t& copied);
  static void PrefetchLoop(UDF_BI* bi);

  struct UDF_BI
  {
    ~UDF_BI();
    void StopPrefetch();

    struct udfread_block_input bi{};
    std::string filePath;
    std::string effectiveUrl;
    bool isRemote{false};
    bool enableRemoteOptimizations{false};
    bool enableBackgroundPrefetch{false};
    std::atomic<bool> supportsReadRange{false};
    std::atomic<bool> allowStreamChannel{true};
    int64_t fileSize{0};
    uint64_t totalPages{0};
    size_t pageSize{0};
    size_t pageBlocks{0};

    // Channel 1 I/O handle (serialized by ioLock; never holds cacheLock across I/O)
    std::shared_ptr<XFILE::CFile> fp{nullptr};
    std::mutex ioLock;
    bool initialOpenAtZero{false};
    int64_t fpPos{-1};

    // Channel 1 SLRU cache & sliding-window sequential read-ahead (protected by cacheLock)
    // Strict lock hierarchy: streamLock -> cacheLock, ioLock -> cacheLock
    std::mutex cacheLock;
    std::condition_variable prefetchCv;
    bool hasLastReadPage{false};
    uint64_t lastReadPage{0};
    bool hasLastFetch{false};
    uint64_t lastFetchedEndPage{0};
    size_t seqRunPages{0};

    //! Protected (hot/metadata) pages, most recently used first
    std::list<uint64_t> hotLru;

    //! Probationary (cold/scan) pages, most recently used first
    std::list<uint64_t> coldLru;

    std::map<uint64_t, CUDFBlockInputPage> pages;

    // Background sequential read-ahead state (remote ISOs only, lazily spawned)
    std::atomic<bool> prefetchCancel{false};
    bool prefetchStop{false};
    bool prefetchThreadStarted{false};
    size_t demandWaiting{0};
    uint64_t prefetchStartPage{0};
    uint64_t prefetchCursorPage{0};
    uint64_t prefetchTargetEndPage{0};
    std::thread prefetchThread;

    // Channel 2: Dedicated large media stream handle (isolated from SLRU and Channel 1 I/O)
    std::mutex streamLock;
    std::shared_ptr<XFILE::CFile> streamFp{nullptr};
    int64_t streamPos{-1};
    bool streamEverOpened{false};
  };

  std::unique_ptr<UDF_BI> m_bi{nullptr};
};
