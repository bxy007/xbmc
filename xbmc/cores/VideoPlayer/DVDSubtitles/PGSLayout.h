// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

struct PGSTextBand
{
  int x0, y0, x1, y1;
};
// Horizontal PGS captions already specify where their text is located.
inline std::vector<PGSTextBand> PGSFindLines(const uint8_t* img, int w, int h)
{
  std::vector<PGSTextBand> found;
  int last = -100;
  for (int y = 0; y < h; y++)
  {
    int count = 0;
    for (int x = 0; x < w; x++)
      count += img[y * w + x] < 230;
    if (count < 2)
      continue;
    if (y - last > 4)
      found.push_back({w, y, 0, y + 1});
    else
      found.back().y1 = y + 1;
    last = y;
  }
  std::vector<PGSTextBand> result;
  for (auto b : found)
  {
    if (b.y1 - b.y0 < 8)
      continue;
    for (int y = b.y0; y < b.y1; y++)
      for (int x = 0; x < w; x++)
        if (img[y * w + x] < 230)
        {
          b.x0 = std::min(b.x0, x);
          b.x1 = std::max(b.x1, x + 1);
        }
    if (b.x0 < b.x1)
      result.push_back(b);
  }
  return result;
}
