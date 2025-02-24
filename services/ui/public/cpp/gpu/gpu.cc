// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/ui/public/cpp/gpu/gpu.h"

#include "base/debug/stack_trace.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "gpu/command_buffer/common/scheduling_priority.h"
#include "mojo/public/cpp/bindings/sync_call_restrictions.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "services/service_manager/public/cpp/connector.h"
#include "services/ui/public/cpp/gpu/client_gpu_memory_buffer_manager.h"
#include "services/ui/public/cpp/gpu/context_provider_command_buffer.h"
#include "services/ui/public/interfaces/constants.mojom.h"
#include "services/ui/public/interfaces/gpu.mojom.h"

namespace ui {

namespace {

mojom::GpuPtr DefaultFactory(service_manager::Connector* connector,
                             const std::string& service_name) {
  mojom::GpuPtr gpu_ptr;
  connector->BindInterface(service_name, &gpu_ptr);
  return gpu_ptr;
}

}  // namespace

// Encapsulates a single request to establish a GPU channel.
class Gpu::EstablishRequest
    : public base::RefCountedThreadSafe<Gpu::EstablishRequest> {
 public:
  EstablishRequest(Gpu* parent,
                   scoped_refptr<base::SingleThreadTaskRunner> main_task_runner)
      : parent_(parent), main_task_runner_(main_task_runner) {}

  int client_id() { return client_id_; }
  mojo::ScopedMessagePipeHandle& channel_handle() { return channel_handle_; }
  gpu::GPUInfo& gpu_info() { return gpu_info_; }
  gpu::GpuFeatureInfo& gpu_feature_info() { return gpu_feature_info_; }

  // Sends EstablishGpuChannel() request using |gpu|. This must be called from
  // the IO thread so that the response is handled on the IO thread.
  void SendRequest(mojom::ThreadSafeGpuPtr* gpu) {
    DCHECK(!main_task_runner_->BelongsToCurrentThread());
    base::AutoLock lock(lock_);

    if (finished_)
      return;

    (*gpu)->EstablishGpuChannel(
        base::Bind(&EstablishRequest::OnEstablishedGpuChannel, this));
  }

  // Sets a WaitableEvent so the main thread can block for a synchronous
  // request. This must be called from main thread.
  void SetWaitableEvent(base::WaitableEvent* establish_event) {
    DCHECK(main_task_runner_->BelongsToCurrentThread());
    base::AutoLock mutex(lock_);

    // If we've already received a response then don't reset |establish_event|.
    // The caller won't block and will immediately process the response.
    if (received_)
      return;

    establish_event_ = establish_event;
    establish_event_->Reset();
  }

  // Cancels the pending request. Any asynchronous calls back into this object
  // will return early and do nothing. This must be called from main thread.
  void Cancel() {
    DCHECK(main_task_runner_->BelongsToCurrentThread());
    base::AutoLock lock(lock_);
    DCHECK(!finished_);
    finished_ = true;
  }

  // This must be called after OnEstablishedGpuChannel() from the main thread.
  void FinishOnMain() {
    // No lock needed, everything will run on |main_task_runner_| now.
    DCHECK(main_task_runner_->BelongsToCurrentThread());
    DCHECK(received_);

    // It's possible to enter FinishedOnMain() twice if EstablishGpuChannel() is
    // called, the request returns and schedules a task on |main_task_runner_|.
    // If EstablishGpuChannelSync() runs before the scheduled task then it will
    // enter FinishedOnMain() immediately and finish. The scheduled task will
    // run later and return early here, doing nothing.
    if (finished_)
      return;

    finished_ = true;

    // |this| might be deleted when running Gpu::OnEstablishedGpuChannel().
    parent_->OnEstablishedGpuChannel();
  }

 protected:
  friend class base::RefCountedThreadSafe<Gpu::EstablishRequest>;

  virtual ~EstablishRequest() = default;

  void OnEstablishedGpuChannel(int client_id,
                               mojo::ScopedMessagePipeHandle channel_handle,
                               const gpu::GPUInfo& gpu_info,
                               const gpu::GpuFeatureInfo& gpu_feature_info) {
    DCHECK(!main_task_runner_->BelongsToCurrentThread());
    base::AutoLock lock(lock_);

    // Do nothing if Cancel() was called.
    if (finished_)
      return;

    DCHECK(!received_);
    received_ = true;

    client_id_ = client_id;
    channel_handle_ = std::move(channel_handle);
    gpu_info_ = gpu_info;
    gpu_feature_info_ = gpu_feature_info;

    if (establish_event_) {
      // Gpu::EstablishGpuChannelSync() was called. Unblock the main thread and
      // let it finish.
      establish_event_->Signal();
    } else {
      main_task_runner_->PostTask(
          FROM_HERE, base::Bind(&EstablishRequest::FinishOnMain, this));
    }
  }

  Gpu* const parent_;
  scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;
  base::WaitableEvent* establish_event_ = nullptr;

  base::Lock lock_;
  bool received_ = false;
  bool finished_ = false;

  int client_id_;
  mojo::ScopedMessagePipeHandle channel_handle_;
  gpu::GPUInfo gpu_info_;
  gpu::GpuFeatureInfo gpu_feature_info_;

  DISALLOW_COPY_AND_ASSIGN(EstablishRequest);
};

Gpu::Gpu(GpuPtrFactory factory,
         scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : main_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      io_task_runner_(std::move(task_runner)),
      gpu_memory_buffer_manager_(
          std::make_unique<ClientGpuMemoryBufferManager>(factory.Run())),
      gpu_(mojom::ThreadSafeGpuPtr::Create(factory.Run().PassInterface(),
                                           io_task_runner_)) {
  DCHECK(main_task_runner_);
  DCHECK(io_task_runner_);
}

Gpu::~Gpu() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (pending_request_) {
    pending_request_->Cancel();
    pending_request_ = nullptr;
  }
  gpu_ = nullptr;
  if (gpu_channel_)
    gpu_channel_->DestroyChannel();
}

// static
std::unique_ptr<Gpu> Gpu::Create(
    service_manager::Connector* connector,
    const std::string& service_name,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  GpuPtrFactory factory =
      base::BindRepeating(&DefaultFactory, connector, service_name);
  auto gpu =
      base::WrapUnique(new Gpu(std::move(factory), std::move(task_runner)));
#if defined(OS_CHROMEOS)
  gpu->InitializeArc(connector, service_name);
#endif  // defined(OS_CHROMEOS)
  return gpu;
}

scoped_refptr<viz::ContextProvider> Gpu::CreateContextProvider(
    scoped_refptr<gpu::GpuChannelHost> gpu_channel) {
  int32_t stream_id = 0;
  gpu::SchedulingPriority stream_priority = gpu::SchedulingPriority::kNormal;

  constexpr bool automatic_flushes = false;
  constexpr bool support_locking = false;

  gpu::gles2::ContextCreationAttribHelper attributes;
  attributes.alpha_size = -1;
  attributes.depth_size = 0;
  attributes.stencil_size = 0;
  attributes.samples = 0;
  attributes.sample_buffers = 0;
  attributes.bind_generates_resource = false;
  attributes.lose_context_when_out_of_memory = true;
  ContextProviderCommandBuffer* shared_context_provider = nullptr;
  return base::MakeRefCounted<ContextProviderCommandBuffer>(
      std::move(gpu_channel), stream_id, stream_priority,
      gpu::kNullSurfaceHandle, GURL("chrome://gpu/MusContextFactory"),
      automatic_flushes, support_locking, gpu::SharedMemoryLimits(), attributes,
      shared_context_provider, command_buffer_metrics::MUS_CLIENT_CONTEXT);
}

#if defined(OS_CHROMEOS)
void Gpu::CreateArcVideoDecodeAccelerator(
    arc::mojom::VideoDecodeAcceleratorRequest vda_request) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  arc_->CreateVideoDecodeAccelerator(std::move(vda_request));
}

void Gpu::CreateArcVideoEncodeAccelerator(
    arc::mojom::VideoEncodeAcceleratorRequest vea_request) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  arc_->CreateVideoEncodeAccelerator(std::move(vea_request));
}

void Gpu::CreateArcProtectedBufferManager(
    arc::mojom::ProtectedBufferManagerRequest pbm_request) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  arc_->CreateProtectedBufferManager(std::move(pbm_request));
}
#endif  // OS_CHROMEOS

void Gpu::CreateJpegDecodeAccelerator(
    media::mojom::GpuJpegDecodeAcceleratorRequest jda_request) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  (*gpu_)->CreateJpegDecodeAccelerator(std::move(jda_request));
}

void Gpu::CreateVideoEncodeAcceleratorProvider(
    media::mojom::VideoEncodeAcceleratorProviderRequest vea_provider_request) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  (*gpu_)->CreateVideoEncodeAcceleratorProvider(
      std::move(vea_provider_request));
}

void Gpu::EstablishGpuChannel(
    const gpu::GpuChannelEstablishedCallback& callback) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  scoped_refptr<gpu::GpuChannelHost> channel = GetGpuChannel();
  if (channel) {
    callback.Run(std::move(channel));
    return;
  }

  establish_callbacks_.push_back(callback);
  SendEstablishGpuChannelRequest();
}

scoped_refptr<gpu::GpuChannelHost> Gpu::EstablishGpuChannelSync(
    bool* connection_error) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (connection_error)
    *connection_error = false;

  scoped_refptr<gpu::GpuChannelHost> channel = GetGpuChannel();
  if (channel)
    return channel;

  SendEstablishGpuChannelRequest();
  base::WaitableEvent event(base::WaitableEvent::ResetPolicy::MANUAL,
                            base::WaitableEvent::InitialState::SIGNALED);
  pending_request_->SetWaitableEvent(&event);
  event.Wait();

  // Running FinishOnMain() will create |gpu_channel_| and run any callbacks
  // from calls to EstablishGpuChannel() before we return from here.
  pending_request_->FinishOnMain();

  return gpu_channel_;
}

gpu::GpuMemoryBufferManager* Gpu::GetGpuMemoryBufferManager() {
  return gpu_memory_buffer_manager_.get();
}

void Gpu::LoseChannel() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (gpu_channel_) {
    gpu_channel_->DestroyChannel();
    gpu_channel_ = nullptr;
  }
}

scoped_refptr<gpu::GpuChannelHost> Gpu::GetGpuChannel() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (gpu_channel_ && gpu_channel_->IsLost())
    gpu_channel_ = nullptr;
  return gpu_channel_;
}

void Gpu::SendEstablishGpuChannelRequest() {
  if (pending_request_)
    return;

  pending_request_ =
      base::MakeRefCounted<EstablishRequest>(this, main_task_runner_);
  io_task_runner_->PostTask(
      FROM_HERE, base::Bind(&EstablishRequest::SendRequest, pending_request_,
                            base::RetainedRef(gpu_)));
}

void Gpu::OnEstablishedGpuChannel() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK(pending_request_);
  DCHECK(!gpu_channel_);

  if (pending_request_->client_id() &&
      pending_request_->channel_handle().is_valid()) {
    gpu_channel_ = base::MakeRefCounted<gpu::GpuChannelHost>(
        this, pending_request_->client_id(), pending_request_->gpu_info(),
        pending_request_->gpu_feature_info(),
        std::move(pending_request_->channel_handle()),
        gpu_memory_buffer_manager_.get());
  }
  pending_request_ = nullptr;

  std::vector<gpu::GpuChannelEstablishedCallback> callbacks;
  callbacks.swap(establish_callbacks_);
  for (const auto& callback : callbacks)
    callback.Run(gpu_channel_);
}

scoped_refptr<base::SingleThreadTaskRunner> Gpu::GetIOThreadTaskRunner() {
  return io_task_runner_;
}

std::unique_ptr<base::SharedMemory> Gpu::AllocateSharedMemory(size_t size) {
  mojo::ScopedSharedBufferHandle handle =
      mojo::SharedBufferHandle::Create(size);
  if (!handle.is_valid())
    return nullptr;

  base::SharedMemoryHandle platform_handle;
  size_t shared_memory_size;
  bool readonly;
  MojoResult result = mojo::UnwrapSharedMemoryHandle(
      std::move(handle), &platform_handle, &shared_memory_size, &readonly);
  if (result != MOJO_RESULT_OK)
    return nullptr;
  DCHECK_EQ(shared_memory_size, size);

  return std::make_unique<base::SharedMemory>(platform_handle, readonly);
}

#if defined(OS_CHROMEOS)
void Gpu::InitializeArc(service_manager::Connector* connector,
                        const std::string& service_name) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  connector->BindInterface(service_name, &arc_);
}
#endif  // defined(OS_CHROMEOS)

}  // namespace ui
