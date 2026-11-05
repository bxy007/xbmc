// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef KODI_PGS_OCR_H
#define KODI_PGS_OCR_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C"
{
#endif
  FILE* pgs_ocr_open_file(const char* path);
  void* pgs_ocr_create(const char* data_path);
  void pgs_ocr_destroy(void* ocr);
  void pgs_ocr_cancel(void* ocr);
  typedef void (*pgs_ocr_word_cb)(void* ctx, const char* text);
  typedef void (*pgs_ocr_line_cb)(void* ctx, int line, const char* text, int minimumConfidence);
  char* pgs_ocr_recognise(void* ocr,
                          const uint8_t* image,
                          int w,
                          int h,
                          int* confidence,
                          bool (*cancel)(void*),
                          void* ctx,
                          pgs_ocr_word_cb word_cb,
                          void* word_ctx,
                          pgs_ocr_line_cb line_cb = nullptr,
                          void* line_ctx = nullptr);
  void pgs_ocr_free_text(char* text);
#ifdef __cplusplus
}
#endif
#endif
