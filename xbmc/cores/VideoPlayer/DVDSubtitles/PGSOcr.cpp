// SPDX-License-Identifier: LGPL-2.1-or-later
// PP-OCRv5 mobile CTC inference on an already decoded PGS bitmap. No network,
// detector, Python, platform OCR API or colour conversion is involved.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__MINGW32__) && !defined(_stdcall)
// The official Windows ORT headers use the MSVC spelling in strict C++ mode.
#define _stdcall __stdcall
#define MP_UNDEF_ORT_STDCALL
#endif
#include <onnxruntime_cxx_api.h>
#ifdef MP_UNDEF_ORT_STDCALL
#undef _stdcall
#undef MP_UNDEF_ORT_STDCALL
#endif
#include "PGSOcr.h"
#include "PGSLayout.h"

extern "C" FILE* pgs_ocr_open_file(const char* path)
{
  return std::fopen(path, "rb");
}

namespace
{
constexpr int max_width = 4096;
struct file_closer
{
  void operator()(FILE* file) const { std::fclose(file); }
};
struct engine
{
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "kodi-pgs-ocr"};
  Ort::Session session{nullptr};
  Ort::RunOptions run_options;
  std::vector<std::string> chars;
  std::string input, output;
  explicit engine(const char* dir)
  {
    std::string path = std::string(dir) + "/ppocrv5-mobile-rec-int8.onnx";
    std::unique_ptr<FILE, file_closer> file(pgs_ocr_open_file(path.c_str()));
    if (!file || fseek(file.get(), 0, SEEK_END))
      throw std::runtime_error("OCR model unavailable");
    long size = ftell(file.get());
    if (size <= 0 || size > 32 * 1024 * 1024 || fseek(file.get(), 0, SEEK_SET))
      throw std::runtime_error("Invalid OCR model size");
    std::vector<uint8_t> model(size);
    if (fread(model.data(), 1, size, file.get()) != static_cast<size_t>(size))
      throw std::runtime_error("Short OCR model read");
    env.DisableTelemetryEvents();
    Ort::SessionOptions opts;
    // Two threads within one inference, sharing a single model/session. Keep
    // inter-op sequential so the two levels cannot multiply the CPU budget.
    opts.SetIntraOpNumThreads(2);
    opts.SetInterOpNumThreads(1);
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
    opts.AddConfigEntry("session.inter_op.allow_spinning", "0");
    // PGS line widths change constantly. Do not retain every temporary
    // tensor allocation until the full track sampling is complete.
    opts.DisableCpuMemArena();
    opts.DisableMemPattern();
    session = Ort::Session(env, model.data(), model.size(), opts);
    Ort::AllocatorWithDefaultOptions alloc;
    auto dict = session.GetModelMetadata().LookupCustomMetadataMapAllocated("character", alloc);
    if (!dict)
      throw std::runtime_error("OCR character dictionary missing");
    chars.emplace_back(); // CTC blank
    std::string all(dict.get());
    size_t begin = 0;
    while (begin < all.size())
    {
      size_t end = all.find('\n', begin);
      if (end == std::string::npos)
        end = all.size();
      chars.push_back(all.substr(begin, end - begin));
      begin = end + 1;
    }
    chars.emplace_back(" ");
    if (session.GetInputCount() != 1 || session.GetOutputCount() != 1)
      throw std::runtime_error("Unexpected OCR model interface");
    input = session.GetInputNameAllocated(0, alloc).get();
    output = session.GetOutputNameAllocated(0, alloc).get();
  }
};
struct glyph
{
  int id;
  float score;
};
int language(const std::string& s)
{
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  unsigned cp = p[0];
  if (p[0] >= 0xe0 && p[0] < 0xf0 && s.size() == 3)
    cp = ((p[0] & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
  else if (p[0] >= 0xf0 && p[0] < 0xf8 && s.size() == 4)
    cp = ((p[0] & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
  if ((cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0x20000 && cp <= 0x2fa1f))
    return 0;
  if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
    return 1;
  return -1;
}
} // namespace

extern "C" void* pgs_ocr_create(const char* path)
{
  if (!path)
    return nullptr;
  try
  {
    return new engine(path);
  }
  catch (...)
  {
    return nullptr;
  }
}
extern "C" void pgs_ocr_destroy(void* p)
{
  delete static_cast<engine*>(p);
}
extern "C" void pgs_ocr_cancel(void* p)
{
  if (p)
  {
    try
    {
      static_cast<engine*>(p)->run_options.SetTerminate();
    }
    catch (...)
    {
    }
  }
}
extern "C" void pgs_ocr_free_text(char* p)
{
  delete[] p;
}

extern "C" char* pgs_ocr_recognise(void* ptr,
                                   const uint8_t* img,
                                   int w,
                                   int h,
                                   int* confidence,
                                   bool (*cancel)(void*),
                                   void* ctx,
                                   pgs_ocr_word_cb callback,
                                   void* word_ctx,
                                   pgs_ocr_line_cb lineCallback,
                                   void* lineContext)
{
  *confidence = 0;
  if (!ptr || !img || w <= 0 || h <= 0 || w > 2080 || h > 544)
    return nullptr;
  try
  {
    auto& e = *static_cast<engine*>(ptr);
    e.run_options.UnsetTerminate();
    auto bands = PGSFindLines(img, w, h);
    if (bands.size() > 16)
      return nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    std::string text;
    double sum = 0;
    int count = 0;
    int lineIndex = 0;
    for (auto b : bands)
    {
      if ((cancel && cancel(ctx)) || std::chrono::steady_clock::now() > deadline)
        return nullptr;
      const int currentLine = lineIndex++;
      int cw = b.x1 - b.x0 + 12, ch = b.y1 - b.y0 + 6;
      int resized = (32 * cw + ch - 1) / ch;
      if (resized > max_width)
        continue; // do not squash extreme layouts
      int width = std::max(320, resized);
      std::vector<float> tensor(3 * 48 * width, 0.0f);
      auto pixel = [&](int x, int y) -> double
      {
        if (x < 6 || x >= cw - 6 || y < 3 || y >= ch - 3)
          return 255;
        return img[(b.y0 + y - 3) * w + b.x0 + x - 6];
      };
      // Keep the recognizer's 48-high tensor, but scale each text line to
      // 32 high and centre it on white. Right padding stays normalized zero.
      for (int c = 0; c < 3; ++c)
        for (int y = 0; y < 48; ++y)
          std::fill_n(tensor.data() + c * 48 * width + y * width, resized, 1.0f);
      for (int y = 0; y < 32; y++)
      {
        double fy = std::max(0.0, (y + 0.5) * ch / 32.0 - 0.5);
        int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, ch - 1);
        fy -= y0;
        for (int x = 0; x < resized; x++)
        {
          double fx = std::max(0.0, (x + 0.5) * cw / resized - 0.5);
          int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, cw - 1);
          fx -= x0;
          float val = std::round((1 - fy) * ((1 - fx) * pixel(x0, y0) + fx * pixel(x1, y0)) +
                                 fy * ((1 - fx) * pixel(x0, y1) + fx * pixel(x1, y1)));
          val = val / 127.5f - 1;
          for (int c = 0; c < 3; c++)
            tensor[c * 48 * width + (y + 8) * width + x] = val;
        }
      }
      int64_t shape[] = {1, 3, 48, width};
      auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      auto in = Ort::Value::CreateTensor<float>(mem, tensor.data(), tensor.size(), shape, 4);
      const char *input = e.input.c_str(), *output = e.output.c_str();
      auto values = e.session.Run(e.run_options, &input, &in, 1, &output, 1);
      if (cancel && cancel(ctx))
        return nullptr;
      auto dims = values[0].GetTensorTypeAndShapeInfo().GetShape();
      if (dims.size() != 3 || dims[0] != 1 || dims[1] <= 0 || dims[1] > 4096 ||
          dims[2] != static_cast<int64_t>(e.chars.size()))
        return nullptr;
      const float* data = values[0].GetTensorData<float>();
      std::vector<glyph> decoded;
      int previous = 0;
      for (int t = 0; t < dims[1]; t++)
      {
        const float* row = data + t * dims[2];
        int id = std::max_element(row, row + dims[2]) - row;
        if (id && id != previous)
          decoded.push_back({id, row[id]});
        previous = id;
      }
      if (lineCallback)
      {
        std::string raw;
        float minimum = 1.0f;
        for (auto g : decoded)
        {
          raw += e.chars[g.id];
          minimum = std::min(minimum, g.score);
        }
        // Preserve physical line index and unfiltered text. Low-confidence
        // Chinese must not disappear and turn a mixed line into "English".
        lineCallback(lineContext, currentLine, raw.c_str(),
                     decoded.empty() ? 0 : static_cast<int>(minimum * 100));
      }
      double mean = 0;
      for (auto g : decoded)
        mean += static_cast<double>(g.score);
      if (decoded.empty() || mean / decoded.size() < 0.70)
        continue;
      for (auto g : decoded)
      {
        // Individual dubious characters must not win a five-glyph vote
        // just because a long English line raised its mean confidence.
        if (static_cast<double>(g.score) < 0.70)
        {
          text += ' ';
          continue;
        }
        text += e.chars[g.id];
        sum += static_cast<double>(g.score);
        count++;
      }
      text += '\n';
      // Keep language-homogeneous confident words for bilingual evidence.
      for (size_t i = 0; callback && i < decoded.size();)
      {
        int lang = language(e.chars[decoded[i].id]);
        if (lang < 0 || static_cast<double>(decoded[i].score) < 0.70)
        {
          i++;
          continue;
        }
        size_t j = i + 1;
        while (j < decoded.size() && language(e.chars[decoded[j].id]) == lang &&
               static_cast<double>(decoded[j].score) >= 0.70)
          j++;
        std::string word;
        for (size_t k = i; k < j; k++)
          word += e.chars[decoded[k].id];
        callback(word_ctx, word.c_str());
        i = j;
      }
    }
    *confidence = count ? static_cast<int>(100 * sum / count) : 0;
    char* result = new char[text.size() + 1];
    std::memcpy(result, text.c_str(), text.size() + 1);
    return result;
  }
  catch (...)
  {
    return nullptr;
  }
}
