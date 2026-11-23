/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"

#include <atomic>

class CRendererAML : public CBaseRenderer
{
public:
  CRendererAML();
  virtual ~CRendererAML();

  // Registration
  static CBaseRenderer* Create(CVideoBuffer *buffer);
  static bool Register();

  virtual bool RenderCapture(int index, CRenderCapture* capture) override;
  CRenderCapture* GetRenderCapture() override;
  virtual void AddVideoPicture(const VideoPicture &picture, int index) override;
  virtual void ReleaseBuffer(int idx) override;
  virtual bool Configure(const VideoPicture &picture, float fps, unsigned int orientation) override;
  virtual bool IsConfigured() override { return m_bConfigured; };
  virtual bool ConfigChanged(const VideoPicture &picture) { return false; };
  virtual CRenderInfo GetRenderInfo() override;
  virtual void UnInit() override {};
  virtual void Update() override {};
  virtual void RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;
  virtual bool SupportsMultiPassRendering()override { return false; };
  virtual bool Flush(bool saveBuffers) override;

  // Player functions
  virtual bool IsGuiLayer() override { return false; };

  bool SupportsAsyncVideoLayerRender() const override { return true; }
  int GetAsyncRenderIndex() override { return m_asyncRenderIndex.load(); }
  void BeginAsyncVideoLayerRender(int idx) override;
  void EndAsyncVideoLayerRender(int idx) override;
  void PrepareVideoLayer() override { ManageRenderArea(); }
  void RenderVideoLayerCommit(int idx, const CRect& src, const CRect& dst) override
  {
    CommitVideoLayer(idx, src, dst);
  }
  int PollVideoLayer() override;

  // Feature support
  virtual bool Supports(ESCALINGMETHOD method) const override { return false; };
  virtual bool Supports(ERENDERFEATURE feature) const override;

private:
  void Reset();
  void CommitVideoLayer(int index, CRect src, CRect dst);

  static const int m_numRenderBuffers = NUM_BUFFERS;

  struct BUFFER
  {
    BUFFER() : videoBuffer(nullptr) {};
    CVideoBuffer *videoBuffer;
    int duration;
  } m_buffers[m_numRenderBuffers];

  bool m_bConfigured;
  std::atomic_int m_asyncRenderIndex{-1};
  CVideoBuffer* m_asyncPinnedBuffer = nullptr;
};
