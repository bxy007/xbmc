/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "UDFBlockInput.h"

#include "IFileTypes.h"
#include "URL.h"
#include "filesystem/File.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include <udfread/udfread.h>

namespace
{
// Network ISOs (WebDAV/HTTP/SMB/NFS/FTP/SFTP): 1 MiB pages sharply reduce per-request RTTs.
// Local ISOs (and external HTTP/DAV VFS addons that maintain their own cache): keep 256 KiB pages.
constexpr size_t REMOTE_PAGE_SIZE{1024 * 1024};
constexpr size_t LOCAL_PAGE_SIZE{256 * 1024};
static_assert(REMOTE_PAGE_SIZE % UDF_BLOCK_SIZE == 0 && REMOTE_PAGE_SIZE > 0);
static_assert(LOCAL_PAGE_SIZE % UDF_BLOCK_SIZE == 0 && LOCAL_PAGE_SIZE > 0);

/*!
 \brief Segmented LRU (SLRU) capacities.

 Protected (hot) pages hold ISO head/tail UDF metadata and any page reused across separate reads,
 while probationary (cold) pages absorb large sequential reads (such as BD-J PNG assets) without
 evicting UDF volume/directory/BDMV metadata.
 Total ceiling: 64 MiB on remote ISOs (allocated lazily on demand), 16 MiB on local ISOs.
 */
constexpr size_t MAX_HOT_PAGES{24};
constexpr size_t MAX_COLD_PAGES{40};

// First 4 pages (0..4 MiB on remote ISO) cover UDF anchors, VDS, metadata partition, index.bdmv,
// MovieObject.bdmv, playlists and clipinfo; last 2 pages cover backup UDF anchor/VDS.
constexpr uint64_t HEAD_PROTECT_PAGES{4};
constexpr uint64_t TAIL_PROTECT_PAGES{2};

// On remote ISOs, prefetch the first 4 pages in one contiguous read on initial mount, coalesce
// contiguous sequential misses up to 4 MiB per read, and asynchronously read up to 16 MiB ahead
// on confirmed sequential streams (overlapping network I/O with JVM / PNG decode CPU work).
constexpr size_t HEAD_PREFETCH_PAGES{4};
constexpr size_t MAX_SEQ_FETCH_PAGES{4};
constexpr uint64_t PREFETCH_AHEAD_MAX_PAGES{16};
static_assert(PREFETCH_AHEAD_MAX_PAGES < MAX_COLD_PAGES);

// Distinct sentinel indicating the dedicated streaming handle could not be opened (e.g. a server
// enforcing single-connection limits), instructing Read() to fall back seamlessly to Channel 1.
constexpr int STREAM_OPEN_UNAVAILABLE{-2};

thread_local int g_streamReadDepth{0};

bool IsCrossHostRedirect(const std::string& origUrl, const std::string& effectiveUrl)
{
  if (effectiveUrl.empty())
    return false;
  const CURL orig(origUrl);
  const CURL eff(effectiveUrl);
  return !eff.GetHostName().empty() && eff.GetHostName() != orig.GetHostName();
}
} // namespace

CUDFBlockInput::ScopedStreamRead::ScopedStreamRead()
{
  ++g_streamReadDepth;
}

CUDFBlockInput::ScopedStreamRead::~ScopedStreamRead()
{
  --g_streamReadDepth;
}

bool CUDFBlockInput::IsStreamRead()
{
  return g_streamReadDepth > 0;
}

CUDFBlockInput::UDF_BI::~UDF_BI()
{
  StopPrefetch();
}

void CUDFBlockInput::UDF_BI::StopPrefetch()
{
  prefetchCancel.store(true, std::memory_order_release);
  {
    std::lock_guard cacheGuard(cacheLock);
    prefetchStop = true;
    prefetchTargetEndPage = prefetchCursorPage;
    prefetchCv.notify_all();
  }
  if (prefetchThread.joinable())
  {
    // Promptly abort any in-flight cancellable network I/O before joining so playback stop
    // does not wait for socket/connect timeouts on a stalled remote server.
    if (fp)
      fp->IoControl(XFILE::IOCTRL_CANCEL, nullptr);
    prefetchThread.join();
  }
}

int CUDFBlockInput::Close(udfread_block_input* bi)
{
  auto m_bi = reinterpret_cast<UDF_BI*>(bi);

  // Join background prefetch thread outside all locks before closing underlying file handles
  m_bi->StopPrefetch();

  // Consistent lock ordering: streamLock, then ioLock -> cacheLock
  {
    std::lock_guard streamLock(m_bi->streamLock);
    if (m_bi->streamFp)
    {
      m_bi->streamFp->Close();
      m_bi->streamFp.reset();
      m_bi->streamPos = -1;
    }
  }

  std::lock_guard ioGuard(m_bi->ioLock);
  {
    std::lock_guard cacheGuard(m_bi->cacheLock);
    m_bi->pages.clear();
    m_bi->hotLru.clear();
    m_bi->coldLru.clear();
  }
  if (m_bi->fp)
    m_bi->fp->Close();

  return 0;
}

uint32_t CUDFBlockInput::Size(udfread_block_input* bi)
{
  auto m_bi = reinterpret_cast<UDF_BI*>(bi);

  if (m_bi->fileSize > 0)
    return static_cast<uint32_t>(m_bi->fileSize / UDF_BLOCK_SIZE);

  return static_cast<uint32_t>(m_bi->fp->GetLength() / UDF_BLOCK_SIZE);
}

bool CUDFBlockInput::IsPinnedPage(const UDF_BI& bi, uint64_t page)
{
  return page < HEAD_PROTECT_PAGES ||
         (bi.totalPages > TAIL_PROTECT_PAGES && page >= bi.totalPages - TAIL_PROTECT_PAGES);
}

size_t CUDFBlockInput::ExpectedPageBytes(const UDF_BI& bi, uint64_t page)
{
  if (bi.fileSize <= 0)
    return bi.pageSize;
  const int64_t pageOffset = static_cast<int64_t>(page) * static_cast<int64_t>(bi.pageSize);
  if (pageOffset >= bi.fileSize)
    return 0;
  const uint64_t remaining = static_cast<uint64_t>(bi.fileSize - pageOffset);
  return static_cast<size_t>(std::min<uint64_t>(bi.pageSize, remaining));
}

void CUDFBlockInput::TrimColdLocked(UDF_BI& bi)
{
  while (bi.coldLru.size() > MAX_COLD_PAGES)
  {
    bi.pages.erase(bi.coldLru.back());
    bi.coldLru.pop_back();
  }
}

void CUDFBlockInput::DemoteHotToColdLocked(UDF_BI& bi)
{
  if (bi.hotLru.empty())
    return;

  // Prefer demoting the least-recently-used non-pinned page so ISO head/tail metadata stay hot.
  auto victimIt = bi.hotLru.end();
  for (auto rit = bi.hotLru.rbegin(); rit != bi.hotLru.rend(); ++rit)
  {
    if (!IsPinnedPage(bi, *rit))
    {
      victimIt = std::prev(rit.base());
      break;
    }
  }
  if (victimIt == bi.hotLru.end())
    victimIt = std::prev(bi.hotLru.end());

  const uint64_t victimPage = *victimIt;
  bi.coldLru.splice(bi.coldLru.begin(), bi.hotLru, victimIt);
  if (auto pit = bi.pages.find(victimPage); pit != bi.pages.end())
  {
    pit->second.hot = false;
    pit->second.lruIt = bi.coldLru.begin();
  }
  TrimColdLocked(bi);
}

void CUDFBlockInput::InsertPageLocked(
    UDF_BI& bi, uint64_t page, std::vector<uint8_t>&& data, size_t valid, bool demanded)
{
  if (bi.pages.find(page) != bi.pages.end())
    return;

  CUDFBlockInputPage entry;
  entry.data = std::move(data);
  entry.valid = valid;
  entry.accessed = demanded;
  entry.hot = IsPinnedPage(bi, page);

  if (entry.hot)
  {
    while (bi.hotLru.size() >= MAX_HOT_PAGES)
      DemoteHotToColdLocked(bi);
    bi.hotLru.push_front(page);
    entry.lruIt = bi.hotLru.begin();
  }
  else
  {
    bi.coldLru.push_front(page);
    entry.lruIt = bi.coldLru.begin();
  }

  bi.pages.emplace(page, std::move(entry));
  TrimColdLocked(bi);
}

CUDFBlockInputPage* CUDFBlockInput::LookupCachedPageLocked(UDF_BI& bi, uint64_t page)
{
  const auto it = bi.pages.find(page);
  if (it == bi.pages.end())
    return nullptr;

  const bool wasPrefetched = !it->second.accessed;
  const bool revisitedFromOtherPage = bi.hasLastReadPage && bi.lastReadPage != page;

  if (it->second.hot)
  {
    bi.hotLru.splice(bi.hotLru.begin(), bi.hotLru, it->second.lruIt);
    it->second.lruIt = bi.hotLru.begin();
    it->second.accessed = true;
  }
  else if (wasPrefetched)
  {
    // First consumption of a prefetched page: keep in coldLru until genuinely revisited
    it->second.accessed = true;
    bi.coldLru.splice(bi.coldLru.begin(), bi.coldLru, it->second.lruIt);
    it->second.lruIt = bi.coldLru.begin();
  }
  else if (revisitedFromOtherPage || IsPinnedPage(bi, page))
  {
    // Genuinely revisited after accessing other pages: promote to protected (hot) segment
    bi.hotLru.splice(bi.hotLru.begin(), bi.coldLru, it->second.lruIt);
    it->second.hot = true;
    it->second.lruIt = bi.hotLru.begin();
    while (bi.hotLru.size() > MAX_HOT_PAGES)
      DemoteHotToColdLocked(bi);
  }
  else
  {
    // Sequential continuation within the same page (e.g. 32 KiB chunks): keep in coldLru
    bi.coldLru.splice(bi.coldLru.begin(), bi.coldLru, it->second.lruIt);
    it->second.lruIt = bi.coldLru.begin();
  }

  // Sliding-window read-ahead: when the caller consumes a prefetched page inside the active
  // sequential window, advance the target ahead and wake the background worker in 4-page batches.
  if (wasPrefetched && bi.prefetchThreadStarted && !bi.prefetchStop && bi.totalPages > 0 &&
      page >= bi.prefetchStartPage && page < bi.prefetchTargetEndPage)
  {
    const uint64_t newTarget = std::min(bi.totalPages, page + 1 + PREFETCH_AHEAD_MAX_PAGES);
    if (newTarget > bi.prefetchTargetEndPage)
    {
      bi.prefetchTargetEndPage = newTarget;
      const uint64_t pending = (bi.prefetchTargetEndPage > bi.prefetchCursorPage)
                                   ? (bi.prefetchTargetEndPage - bi.prefetchCursorPage)
                                   : 0;
      const bool hitsEof = (bi.prefetchTargetEndPage >= bi.totalPages && pending > 0);
      if (bi.demandWaiting == 0 && (pending >= MAX_SEQ_FETCH_PAGES || hitsEof))
        bi.prefetchCv.notify_one();
    }
  }

  bi.hasLastReadPage = true;
  bi.lastReadPage = page;
  return &it->second;
}

void CUDFBlockInput::SchedulePrefetchLocked(UDF_BI& bi, uint64_t startPage)
{
  if (!bi.enableBackgroundPrefetch || bi.totalPages == 0 || bi.prefetchStop ||
      startPage >= bi.totalPages)
    return;

  bi.prefetchStartPage = startPage;
  bi.prefetchCursorPage = startPage;
  bi.prefetchTargetEndPage = std::min(bi.totalPages, startPage + PREFETCH_AHEAD_MAX_PAGES);

  if (!bi.prefetchThreadStarted)
  {
    try
    {
      bi.prefetchThread = std::thread(PrefetchLoop, &bi);
      bi.prefetchThreadStarted = true;
    }
    catch (...)
    {
      bi.prefetchThreadStarted = false;
      bi.prefetchTargetEndPage = bi.prefetchCursorPage;
      return;
    }
  }

  if (bi.demandWaiting == 0)
    bi.prefetchCv.notify_one();
}

ssize_t CUDFBlockInput::FetchRawIoLocked(UDF_BI& bi,
                                         uint64_t page,
                                         size_t fetchPages,
                                         std::vector<uint8_t>& raw,
                                         const std::atomic<bool>* cancelFlag)
{
  if (cancelFlag && cancelFlag->load(std::memory_order_acquire))
    return -1;

  const int64_t offset = static_cast<int64_t>(page) * static_cast<int64_t>(bi.pageSize);
  if (bi.fileSize > 0 && offset >= bi.fileSize)
    return -1;

  size_t totalWant = fetchPages * bi.pageSize;
  if (bi.fileSize > 0)
  {
    const uint64_t remaining = static_cast<uint64_t>(bi.fileSize - offset);
    if (remaining < totalWant)
      totalWant = static_cast<size_t>(remaining);
  }
  if (totalWant == 0)
    return -1;

  raw.resize(totalWant);
  ssize_t bytesRead = -1;

  if (bi.supportsReadRange.load(std::memory_order_relaxed))
  {
    XFILE::SReadRange req{offset, raw.data(), totalWant, -1, cancelFlag};
    if (bi.fp->IoControl(XFILE::IOCTRL_READ_RANGE, &req) == 0 && req.bytesRead > 0)
    {
      bytesRead = req.bytesRead;
      bi.fpPos = -1;
      // Keep cached redirected URL in sync in case ReadRange re-resolved an expired link
      const std::string efurl = bi.fp->GetProperty(XFILE::FILE_PROPERTY_EFFECTIVE_URL);
      if (IsCrossHostRedirect(bi.filePath, efurl))
      {
        std::lock_guard cacheGuard(bi.cacheLock);
        bi.effectiveUrl = efurl;
      }
    }
    else if (cancelFlag && cancelFlag->load(std::memory_order_acquire))
    {
      bi.fpPos = -1;
      return -1;
    }
    else
    {
      // Fallback to Seek+Read; only permanently disable ReadRange if fallback succeeds
      bi.fpPos = -1;
      if (bi.fp->Seek(offset, SEEK_SET) == offset)
      {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire))
          return -1;
        bytesRead = bi.fp->Read(raw.data(), totalWant);
        if (bytesRead > 0)
        {
          bi.supportsReadRange.store(false, std::memory_order_relaxed);
          bi.fpPos = offset + bytesRead;
        }
      }
    }
  }
  else
  {
    if (bi.fpPos != offset)
    {
      if (bi.fp->Seek(offset, SEEK_SET) != offset)
      {
        bi.fpPos = -1;
        return -1;
      }
      bi.fpPos = offset;
    }
    if (cancelFlag)
    {
      size_t done = 0;
      while (done < totalWant)
      {
        if (cancelFlag->load(std::memory_order_acquire))
        {
          bi.fpPos = -1;
          return -1;
        }
        const size_t step = std::min(bi.pageSize, totalWant - done);
        const ssize_t r = bi.fp->Read(raw.data() + done, step);
        if (r < 0)
        {
          bi.fpPos = -1;
          return done > 0 ? static_cast<ssize_t>(done) : -1;
        }
        if (r == 0)
          break;
        done += static_cast<size_t>(r);
        bi.fpPos += r;
        if (static_cast<size_t>(r) < step)
          break;
      }
      bytesRead = static_cast<ssize_t>(done);
    }
    else
    {
      bytesRead = bi.fp->Read(raw.data(), totalWant);
      if (bytesRead > 0)
        bi.fpPos += bytesRead;
      else
        bi.fpPos = -1;
    }
  }

  return bytesRead;
}

bool CUDFBlockInput::FetchForegroundLocked(UDF_BI& bi,
                                           uint64_t page,
                                           size_t minPagesNeeded,
                                           size_t from,
                                           size_t want,
                                           uint8_t* out,
                                           size_t& copied)
{
  copied = 0;

  const int64_t offset = static_cast<int64_t>(page) * static_cast<int64_t>(bi.pageSize);
  if (bi.fileSize > 0 && offset >= bi.fileSize)
    return false;

  const bool isInitialHead = (page == 0 && bi.initialOpenAtZero);
  bi.initialOpenAtZero = false;

  size_t desiredPages =
      bi.enableRemoteOptimizations
          ? std::max<size_t>(1, std::min<size_t>(MAX_SEQ_FETCH_PAGES, minPagesNeeded))
          : 1;
  bool sequentialMiss = false;
  size_t newSeqRun = 1;

  {
    std::lock_guard cacheGuard(bi.cacheLock);

    if (isInitialHead && bi.enableRemoteOptimizations &&
        bi.fileSize >= static_cast<int64_t>(HEAD_PREFETCH_PAGES * bi.pageSize))
    {
      // Read 0..4 MiB in one shot right off the initial offset-0 handle on mount
      desiredPages = HEAD_PREFETCH_PAGES;
      bi.seqRunPages = 1;
    }
    else if (bi.enableRemoteOptimizations)
    {
      sequentialMiss = (bi.hasLastFetch && page == bi.lastFetchedEndPage) ||
                       (bi.hasLastReadPage && page == bi.lastReadPage + 1);
      if (sequentialMiss)
      {
        bi.seqRunPages = std::min<size_t>(MAX_SEQ_FETCH_PAGES,
                                          bi.seqRunPages == 0 ? 2 : bi.seqRunPages * 2);
        desiredPages = std::max(desiredPages, bi.seqRunPages);
      }
      else
      {
        bi.seqRunPages = 1;
        // Non-sequential jump: cancel pending speculative read-ahead in the old region
        bi.prefetchStartPage = bi.prefetchCursorPage;
        bi.prefetchTargetEndPage = bi.prefetchCursorPage;
      }
    }
    newSeqRun = bi.seqRunPages;

    size_t fetchPages = 1;
    while (fetchPages < desiredPages)
    {
      if (bi.totalPages > 0 && page + fetchPages >= bi.totalPages)
        break;
      if (bi.pages.find(page + fetchPages) != bi.pages.end())
        break;
      ++fetchPages;
    }
    desiredPages = fetchPages;
  }

  std::vector<uint8_t> raw;
  const ssize_t bytesRead = FetchRawIoLocked(bi, page, desiredPages, raw);
  if (bytesRead <= 0)
    return false;

  const size_t got = static_cast<size_t>(bytesRead);

  std::lock_guard cacheGuard(bi.cacheLock);
  size_t pagesFilled = 0;

  if (desiredPages == 1)
  {
    const size_t expectedLen = ExpectedPageBytes(bi, page);
    if (expectedLen > 0 && got >= expectedLen)
    {
      raw.resize(expectedLen);
      InsertPageLocked(bi, page, std::move(raw), expectedLen, true);
      pagesFilled = 1;
    }
  }
  else
  {
    for (size_t i = 0; i < desiredPages; ++i)
    {
      const size_t chunkStart = i * bi.pageSize;
      if (chunkStart >= got)
        break;
      const size_t chunkLen = std::min(bi.pageSize, got - chunkStart);
      const uint64_t curPage = page + i;
      const size_t expectedLen = ExpectedPageBytes(bi, curPage);
      if (expectedLen == 0 || chunkLen < expectedLen)
        break;
      std::vector<uint8_t> pageData(expectedLen);
      std::memcpy(pageData.data(), raw.data() + chunkStart, expectedLen);
      InsertPageLocked(bi, curPage, std::move(pageData), expectedLen, i == 0);
      ++pagesFilled;
    }
  }

  if (pagesFilled > 0)
  {
    bi.hasLastFetch = true;
    bi.lastFetchedEndPage = page + pagesFilled;
  }
  else
  {
    bi.hasLastFetch = false;
    bi.seqRunPages = 1;
  }
  bi.hasLastReadPage = true;
  bi.lastReadPage = page;

  if (pagesFilled == desiredPages && sequentialMiss && newSeqRun >= 2 &&
      bi.enableRemoteOptimizations)
    SchedulePrefetchLocked(bi, page + pagesFilled);

  if (pagesFilled > 0)
  {
    const auto it = bi.pages.find(page);
    if (it == bi.pages.end())
      return false;

    if (from < it->second.valid)
    {
      copied = std::min(want, it->second.valid - from);
      std::memcpy(out, it->second.data.data() + from, copied);
    }
    return true;
  }

  // Truncated/short read on the demanded page itself: never cache a partial non-EOF page.
  // Serve complete sectors from raw if available, or report an error when the image extends
  // past `from` and fewer than one block was retrieved.
  if (from < got)
  {
    copied = std::min(want, got - from);
    if (bi.fileSize > 0 && copied < UDF_BLOCK_SIZE)
    {
      copied = 0;
      return false;
    }
    std::memcpy(out, raw.data() + from, copied);
    return true;
  }
  return bi.fileSize <= 0;
}

void CUDFBlockInput::PrefetchLoop(UDF_BI* bi)
{
  while (true)
  {
    uint64_t startPage = 0;
    size_t maxPages = 0;

    {
      std::unique_lock cacheGuard(bi->cacheLock);
      bi->prefetchCv.wait(cacheGuard,
                          [&]()
                          {
                            if (bi->prefetchStop)
                              return true;
                            if (bi->demandWaiting > 0)
                              return false;
                            if (bi->prefetchCursorPage >= bi->prefetchTargetEndPage)
                              return false;
                            const uint64_t pending =
                                bi->prefetchTargetEndPage - bi->prefetchCursorPage;
                            const bool hitsEof = (bi->totalPages > 0 &&
                                                  bi->prefetchTargetEndPage >= bi->totalPages);
                            return pending >= MAX_SEQ_FETCH_PAGES || hitsEof;
                          });

      if (bi->prefetchStop)
        break;

      while (bi->prefetchCursorPage < bi->prefetchTargetEndPage &&
             bi->pages.find(bi->prefetchCursorPage) != bi->pages.end())
      {
        ++bi->prefetchCursorPage;
      }

      if (bi->prefetchCursorPage >= bi->prefetchTargetEndPage)
        continue;

      startPage = bi->prefetchCursorPage;
      const uint64_t remain = bi->prefetchTargetEndPage - startPage;
      const size_t wantPages = static_cast<size_t>(std::min<uint64_t>(MAX_SEQ_FETCH_PAGES, remain));
      size_t count = 1;
      while (count < wantPages)
      {
        if (bi->totalPages > 0 && startPage + count >= bi->totalPages)
          break;
        if (bi->pages.find(startPage + count) != bi->pages.end())
          break;
        ++count;
      }
      maxPages = count;
    }

    // Acquire ioLock outside cacheLock (strict lock order: ioLock -> cacheLock)
    std::lock_guard ioGuard(bi->ioLock);

    {
      std::lock_guard cacheGuard(bi->cacheLock);
      if (bi->prefetchStop)
        break;
      if (bi->demandWaiting > 0)
        continue;
      if (startPage < bi->prefetchCursorPage || startPage >= bi->prefetchTargetEndPage)
        continue;
      if (bi->pages.find(startPage) != bi->pages.end())
      {
        ++bi->prefetchCursorPage;
        continue;
      }
    }

    std::vector<uint8_t> raw;
    const ssize_t bytesRead = FetchRawIoLocked(*bi, startPage, maxPages, raw, &bi->prefetchCancel);
    if (bytesRead <= 0)
    {
      std::lock_guard cacheGuard(bi->cacheLock);
      if (bi->prefetchStop || bi->prefetchCancel.load(std::memory_order_acquire))
        break;
      bi->prefetchStartPage = bi->prefetchCursorPage;
      bi->prefetchTargetEndPage = bi->prefetchCursorPage;
      continue;
    }

    std::lock_guard cacheGuard(bi->cacheLock);
    if (bi->prefetchStop || bi->prefetchCancel.load(std::memory_order_acquire))
      break;
    const size_t got = static_cast<size_t>(bytesRead);
    size_t pagesFilled = 0;
    for (size_t i = 0; i < maxPages; ++i)
    {
      const size_t chunkStart = i * bi->pageSize;
      if (chunkStart >= got)
        break;
      const size_t chunkLen = std::min(bi->pageSize, got - chunkStart);
      const uint64_t curPage = startPage + i;
      const size_t expectedLen = ExpectedPageBytes(*bi, curPage);
      if (expectedLen == 0 || chunkLen < expectedLen)
        break;
      std::vector<uint8_t> pageData(expectedLen);
      std::memcpy(pageData.data(), raw.data() + chunkStart, expectedLen);
      InsertPageLocked(*bi, curPage, std::move(pageData), expectedLen, false);
      ++pagesFilled;
    }

    if (startPage == bi->prefetchCursorPage)
      bi->prefetchCursorPage += pagesFilled;
    if (pagesFilled < maxPages)
    {
      bi->prefetchStartPage = bi->prefetchCursorPage;
      bi->prefetchTargetEndPage = bi->prefetchCursorPage;
    }
  }
}

int CUDFBlockInput::ReadStream(UDF_BI& bi, uint32_t lba, void* buf, uint32_t blocks)
{
  std::lock_guard streamLock(bi.streamLock);

  const int64_t offset = static_cast<int64_t>(lba) * UDF_BLOCK_SIZE;
  const size_t want = static_cast<size_t>(blocks) * UDF_BLOCK_SIZE;

  auto openWithOffset = [&](const std::string& targetUrl, int64_t startOff,
                            unsigned int flags) -> std::shared_ptr<XFILE::CFile>
  {
    auto fp = std::make_shared<XFILE::CFile>();
    bool opened = false;
    if (startOff > 0)
    {
      fp->CURLCreate(targetUrl);
      fp->CURLAddOption(XFILE::CURL_OPTION_PROTOCOL, "startoffset",
                        std::to_string(startOff).c_str());
      opened = fp->CURLOpen(flags);
    }
    else
    {
      opened = fp->Open(targetUrl, flags);
    }
    return opened ? fp : nullptr;
  };

  auto openStreamAt = [&](int64_t startOff, bool allowEffectiveUrl) -> bool
  {
    if (bi.streamFp)
    {
      bi.streamFp->Close();
      bi.streamFp.reset();
    }
    bi.streamPos = -1;

    std::string redirectedUrl;
    if (allowEffectiveUrl)
    {
      std::lock_guard cacheGuard(bi.cacheLock);
      redirectedUrl = bi.effectiveUrl;
    }

    const unsigned int flags = XFILE::READ_NO_CACHE | XFILE::READ_NO_BUFFER;

    if (!redirectedUrl.empty())
    {
      if (auto fp = openWithOffset(redirectedUrl, startOff, flags))
      {
        bi.streamFp = std::move(fp);
        bi.streamPos = bi.streamFp->GetPosition();
        bi.streamEverOpened = true;
        CLog::Log(LOGDEBUG,
                  "CUDFBlockInput::ReadStream - opened dedicated stream via redirected URL at offset {}",
                  bi.streamPos);
        return true;
      }
    }

    // For direct HTTP/WebDAV servers (without cross-host redirects), try opening directly at
    // startoffset first so we avoid a wasted offset-0 transfer + disconnect + reconnect.
    if (bi.supportsReadRange.load(std::memory_order_relaxed) && startOff > 0)
    {
      if (auto fp = openWithOffset(bi.filePath, startOff, flags))
      {
        bi.streamFp = std::move(fp);
        bi.streamPos = bi.streamFp->GetPosition();
        bi.streamEverOpened = true;
        const std::string efurl = bi.streamFp->GetProperty(XFILE::FILE_PROPERTY_EFFECTIVE_URL);
        if (IsCrossHostRedirect(bi.filePath, efurl))
        {
          std::lock_guard cacheGuard(bi.cacheLock);
          bi.effectiveUrl = efurl;
        }
        CLog::Log(LOGDEBUG,
                  "CUDFBlockInput::ReadStream - opened dedicated stream at offset {}",
                  bi.streamPos);
        return true;
      }
    }

    auto fp = std::make_shared<XFILE::CFile>();
    if (!fp->Open(bi.filePath, flags))
      return false;

    bi.streamFp = std::move(fp);
    bi.streamPos = bi.streamFp->GetPosition();
    bi.streamEverOpened = true;

    if (bi.supportsReadRange.load(std::memory_order_relaxed))
    {
      const std::string efurl = bi.streamFp->GetProperty(XFILE::FILE_PROPERTY_EFFECTIVE_URL);
      if (IsCrossHostRedirect(bi.filePath, efurl))
      {
        std::lock_guard cacheGuard(bi.cacheLock);
        bi.effectiveUrl = efurl;
      }
    }
    return true;
  };

  for (int attempt = 0; attempt < 2; ++attempt)
  {
    if (!bi.streamFp)
    {
      if (!openStreamAt(offset, attempt == 0))
      {
        if (!bi.streamEverOpened)
        {
          bi.allowStreamChannel.store(false, std::memory_order_relaxed);
          CLog::Log(LOGINFO,
                    "CUDFBlockInput::ReadStream - dedicated stream channel unavailable, "
                    "falling back to shared channel");
        }
        return STREAM_OPEN_UNAVAILABLE;
      }
    }

    if (bi.streamPos != offset)
    {
      const int64_t pos = bi.streamFp->Seek(offset, SEEK_SET);
      if (pos != offset)
      {
        bi.streamFp->Close();
        bi.streamFp.reset();
        bi.streamPos = -1;
        if (attempt == 0)
          continue;
        return -1;
      }
      bi.streamPos = offset;
    }

    const ssize_t read = bi.streamFp->Read(buf, want);
    if (read < 0)
    {
      bi.streamFp->Close();
      bi.streamFp.reset();
      bi.streamPos = -1;
      if (attempt == 0)
        continue;
      return -1;
    }

    bi.streamPos += read;
    return static_cast<int>(static_cast<size_t>(read) / UDF_BLOCK_SIZE);
  }

  return -1;
}

int CUDFBlockInput::Read(
    udfread_block_input* bi, uint32_t lba, void* buf, uint32_t blocks, int flags)
{
  auto m_bi = reinterpret_cast<UDF_BI*>(bi);

  if (!m_bi || !m_bi->fp || blocks == 0)
    return -1;

  if (IsStreamRead() && m_bi->allowStreamChannel.load(std::memory_order_relaxed))
  {
    const int rc = ReadStream(*m_bi, lba, buf, blocks);
    if (rc != STREAM_OPEN_UNAVAILABLE)
      return rc;
    // Dedicated 2nd handle could not be opened (e.g. single-connection server): fall back safely
  }

  auto* out = static_cast<uint8_t*>(buf);
  uint64_t block{lba};
  uint32_t left{blocks};

  while (left > 0)
  {
    const uint64_t page{block / m_bi->pageBlocks};
    const auto inPage{static_cast<uint32_t>(block % m_bi->pageBlocks)};
    const uint32_t take{std::min(left, static_cast<uint32_t>(m_bi->pageBlocks - inPage))};
    const size_t from{static_cast<size_t>(inPage) * UDF_BLOCK_SIZE};
    const size_t want{static_cast<size_t>(take) * UDF_BLOCK_SIZE};

    const uint64_t endBlock = block + left - 1;
    const uint64_t endPage = endBlock / m_bi->pageBlocks;
    const size_t minPagesNeeded =
        static_cast<size_t>(std::min<uint64_t>(MAX_SEQ_FETCH_PAGES, endPage - page + 1));

    size_t copied = 0;
    bool hit = false;
    {
      std::lock_guard cacheGuard(m_bi->cacheLock);
      if (CUDFBlockInputPage* entry = LookupCachedPageLocked(*m_bi, page))
      {
        hit = true;
        if (from < entry->valid)
        {
          copied = std::min(want, entry->valid - from);
          std::memcpy(out, entry->data.data() + from, copied);
        }
      }
    }

    if (!hit)
    {
      {
        std::lock_guard cacheGuard(m_bi->cacheLock);
        ++m_bi->demandWaiting;
      }

      struct DemandGuard
      {
        UDF_BI& state;
        ~DemandGuard()
        {
          std::lock_guard cacheGuard(state.cacheLock);
          --state.demandWaiting;
          if (state.demandWaiting == 0 && !state.prefetchStop &&
              state.prefetchCursorPage < state.prefetchTargetEndPage)
            state.prefetchCv.notify_one();
        }
      } demandGuard{*m_bi};

      std::lock_guard ioGuard(m_bi->ioLock);

      // Re-check cache after acquiring ioLock in case background prefetch just populated this page
      {
        std::lock_guard cacheGuard(m_bi->cacheLock);
        if (CUDFBlockInputPage* entry = LookupCachedPageLocked(*m_bi, page))
        {
          hit = true;
          if (from < entry->valid)
          {
            copied = std::min(want, entry->valid - from);
            std::memcpy(out, entry->data.data() + from, copied);
          }
        }
      }

      if (!hit)
      {
        if (!FetchForegroundLocked(*m_bi, page, minPagesNeeded, from, want, out, copied))
          return -1;
      }
    }

    out += copied;
    if (copied < want)
    {
      const uint32_t copiedBlocks = static_cast<uint32_t>(copied / UDF_BLOCK_SIZE);
      block += copiedBlocks;
      left -= copiedBlocks;
      break;
    }

    block += take;
    left -= take;
  }

  return static_cast<int>(block - lba);
}


udfread_block_input* CUDFBlockInput::GetBlockInput(const std::string& file)
{
  auto fp = std::make_shared<XFILE::CFile>();

  if (fp->Open(file, XFILE::READ_NO_CACHE | XFILE::READ_NO_BUFFER))
  {
    m_bi = std::make_unique<UDF_BI>();
    if (m_bi)
    {
      m_bi->fp = fp;
      m_bi->filePath = file;
      m_bi->isRemote = URIUtils::IsRemote(file);

      XFILE::SReadRange probe{};
      const bool supportsRange = (fp->IoControl(XFILE::IOCTRL_READ_RANGE, &probe) == 0);
      m_bi->supportsReadRange.store(supportsRange, std::memory_order_relaxed);

      // If an external VFS addon intercepts HTTP/DAV, it maintains its own large cache/buffer;
      // avoid opening a duplicate stream handle or over-reading on top of it.
      const bool isHttpOrDav = URIUtils::IsHTTP(file, true);
      const bool externalHttpVfs = isHttpOrDav && !supportsRange;

      m_bi->enableRemoteOptimizations = m_bi->isRemote && !externalHttpVfs;
      // Close joins the worker, so only speculative reads on cancellable handles are safe.
      m_bi->enableBackgroundPrefetch =
          m_bi->enableRemoteOptimizations &&
          fp->IoControl(XFILE::IOCTRL_CANCEL_POSSIBLE, nullptr) == 1;
      m_bi->allowStreamChannel.store(!externalHttpVfs, std::memory_order_relaxed);
      m_bi->pageSize = m_bi->enableRemoteOptimizations ? REMOTE_PAGE_SIZE : LOCAL_PAGE_SIZE;
      m_bi->pageBlocks = m_bi->pageSize / UDF_BLOCK_SIZE;
      m_bi->fileSize = fp->GetLength();
      if (m_bi->fileSize > 0)
        m_bi->totalPages =
            (static_cast<uint64_t>(m_bi->fileSize) + m_bi->pageSize - 1) / m_bi->pageSize;

      m_bi->initialOpenAtZero = true;
      m_bi->fpPos = 0;

      if (supportsRange)
      {
        const std::string efurl = fp->GetProperty(XFILE::FILE_PROPERTY_EFFECTIVE_URL);
        if (IsCrossHostRedirect(file, efurl))
          m_bi->effectiveUrl = efurl;
      }

      m_bi->bi.close = CUDFBlockInput::Close;
      m_bi->bi.read = CUDFBlockInput::Read;
      m_bi->bi.size = CUDFBlockInput::Size;

      CLog::Log(LOGDEBUG,
                "CUDFBlockInput::GetBlockInput - remote={} range={} remoteOpt={} streamChan={} "
                "backgroundPrefetch={} pageKB={} maxMB={} size={} hasRedirectUrl={}",
                m_bi->isRemote, supportsRange, m_bi->enableRemoteOptimizations,
                m_bi->allowStreamChannel.load(std::memory_order_relaxed),
                m_bi->enableBackgroundPrefetch, m_bi->pageSize / 1024,
                ((MAX_HOT_PAGES + MAX_COLD_PAGES) * m_bi->pageSize) / (1024 * 1024),
                m_bi->fileSize, !m_bi->effectiveUrl.empty());

      return &m_bi->bi;
    }

    fp->Close();
  }

  return nullptr;
}
