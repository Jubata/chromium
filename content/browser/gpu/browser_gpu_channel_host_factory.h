// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_GPU_BROWSER_GPU_CHANNEL_HOST_FACTORY_H_
#define CONTENT_BROWSER_GPU_BROWSER_GPU_CHANNEL_HOST_FACTORY_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/single_thread_task_runner.h"
#include "build/build_config.h"
#include "content/common/content_export.h"
#include "gpu/ipc/client/gpu_channel_host.h"
#include "ipc/message_filter.h"

namespace content {
class BrowserGpuMemoryBufferManager;

class CONTENT_EXPORT BrowserGpuChannelHostFactory
    : public gpu::GpuChannelHostFactory,
      public gpu::GpuChannelEstablishFactory {
 public:
  static void Initialize(bool establish_gpu_channel);
  static void Terminate();
  static BrowserGpuChannelHostFactory* instance() { return instance_; }

  // Overridden from gpu::GpuChannelHostFactory:
  scoped_refptr<base::SingleThreadTaskRunner> GetIOThreadTaskRunner() override;
  std::unique_ptr<base::SharedMemory> AllocateSharedMemory(
      size_t size) override;

  gpu::GpuChannelHost* GetGpuChannel();
  int GetGpuChannelId() { return gpu_client_id_; }

  // Closes the channel to the GPU process. This should be called before the IO
  // thread stops.
  void CloseChannel();

  // Overridden from gpu::GpuChannelEstablishFactory:
  // The factory will return a null GpuChannelHost in the callback during
  // shutdown.
  void EstablishGpuChannel(
      const gpu::GpuChannelEstablishedCallback& callback) override;
  scoped_refptr<gpu::GpuChannelHost> EstablishGpuChannelSync(
      bool* connection_error) override;
  gpu::GpuMemoryBufferManager* GetGpuMemoryBufferManager() override;

 private:
  struct CreateRequest;
  class EstablishRequest;

  BrowserGpuChannelHostFactory();
  ~BrowserGpuChannelHostFactory() override;

  void GpuChannelEstablished();
  void RestartTimeout();

  static void InitializeShaderDiskCacheOnIO(int gpu_client_id,
                                            const base::FilePath& cache_dir);

  const int gpu_client_id_;
  const uint64_t gpu_client_tracing_id_;
  scoped_refptr<gpu::GpuChannelHost> gpu_channel_;
  std::unique_ptr<BrowserGpuMemoryBufferManager> gpu_memory_buffer_manager_;
  scoped_refptr<EstablishRequest> pending_request_;
  std::vector<gpu::GpuChannelEstablishedCallback> established_callbacks_;

  base::OneShotTimer timeout_;

  static BrowserGpuChannelHostFactory* instance_;

  DISALLOW_COPY_AND_ASSIGN(BrowserGpuChannelHostFactory);
};

}  // namespace content

#endif  // CONTENT_BROWSER_GPU_BROWSER_GPU_CHANNEL_HOST_FACTORY_H_
