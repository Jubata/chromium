// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/ui/public/cpp/gpu/gpu.h"

#include "base/callback_helpers.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/scoped_task_environment.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/config/gpu_info.h"
#include "mojo/public/cpp/bindings/binding_set.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ui {

namespace {

class TestGpuImpl : public mojom::Gpu {
 public:
  TestGpuImpl() = default;
  ~TestGpuImpl() override = default;

  void SetRequestWillSucceed(bool request_will_succeed) {
    request_will_succeed_ = request_will_succeed;
  }

  void BindRequest(mojom::GpuRequest request) {
    bindings_.AddBinding(this, std::move(request));
  }

  // ui::mojom::Gpu overrides:
  void EstablishGpuChannel(
      const EstablishGpuChannelCallback& callback) override {
    constexpr int client_id = 1;
    mojo::ScopedMessagePipeHandle handle;
    if (request_will_succeed_)
      handle = std::move(mojo::MessagePipe().handle0);
    callback.Run(client_id, std::move(handle), gpu::GPUInfo(),
                 gpu::GpuFeatureInfo());
  }

  void CreateJpegDecodeAccelerator(
      media::mojom::GpuJpegDecodeAcceleratorRequest jda_request) override {}

  void CreateVideoEncodeAcceleratorProvider(
      media::mojom::VideoEncodeAcceleratorProviderRequest request) override {}

  void CreateGpuMemoryBuffer(
      gfx::GpuMemoryBufferId id,
      const gfx::Size& size,
      gfx::BufferFormat format,
      gfx::BufferUsage usage,
      const ui::mojom::Gpu::CreateGpuMemoryBufferCallback& callback) override {}

  void DestroyGpuMemoryBuffer(gfx::GpuMemoryBufferId id,
                              const gpu::SyncToken& sync_token) override {}

 private:
  bool request_will_succeed_ = true;
  mojo::BindingSet<mojom::Gpu> bindings_;

  DISALLOW_COPY_AND_ASSIGN(TestGpuImpl);
};

}  // namespace

class GpuTest : public testing::Test {
 public:
  GpuTest() : io_thread_("GPUIOThread") {
    base::Thread::Options thread_options(base::MessageLoop::TYPE_IO, 0);
    thread_options.priority = base::ThreadPriority::NORMAL;
    CHECK(io_thread_.StartWithOptions(thread_options));
  }
  ~GpuTest() override = default;

  Gpu* gpu() { return gpu_.get(); }

  TestGpuImpl* gpu_impl() { return gpu_impl_.get(); }

  // Runs all tasks posted to IO thread and any tasks posted back to main thread
  // as a result.
  void FlushMainAndIO() {
    base::RunLoop run_loop;
    io_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce([](base::Closure callback) { callback.Run(); },
                       run_loop.QuitClosure()));
    run_loop.Run();
  }

  // Runs all tasks posted to IO thread and blocks main thread.
  void BlockMainFlushIO() {
    base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                              base::WaitableEvent::InitialState::NOT_SIGNALED);
    io_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WaitableEvent* event) {
              base::RunLoop(base::RunLoop::Type::kNestableTasksAllowed)
                  .RunUntilIdle();
              event->Signal();
            },
            base::Unretained(&event)));
    event.Wait();
  }

  void DestroyGpu() { gpu_.reset(); }

  // testing::Test:
  void SetUp() override {
    gpu_impl_ = std::make_unique<TestGpuImpl>();
    auto factory =
        base::BindRepeating(&GpuTest::GetPtr, base::Unretained(this));
    gpu_ =
        base::WrapUnique(new Gpu(std::move(factory), io_thread_.task_runner()));
  }

  void TearDown() override {
    FlushMainAndIO();
    DestroyGpuImplOnIO();
  }

 private:
  mojom::GpuPtr GetPtr() {
    mojom::GpuPtr ptr;
    io_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::Bind(&TestGpuImpl::BindRequest, base::Unretained(gpu_impl_.get()),
                   base::Passed(MakeRequest(&ptr))));
    return ptr;
  }

  void DestroyGpuImplOnIO() {
    base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                              base::WaitableEvent::InitialState::NOT_SIGNALED);
    io_thread_.task_runner()->PostTask(
        FROM_HERE, base::Bind(
                       [](std::unique_ptr<TestGpuImpl> gpu_impl,
                          base::WaitableEvent* event) {
                         gpu_impl.reset();
                         event->Signal();
                       },
                       base::Passed(std::move(gpu_impl_)), &event));
    event.Wait();
  }

  base::test::ScopedTaskEnvironment env_;
  base::Thread io_thread_;
  std::unique_ptr<Gpu> gpu_;
  std::unique_ptr<TestGpuImpl> gpu_impl_;

  DISALLOW_COPY_AND_ASSIGN(GpuTest);
};

// Tests that multiple calls for establishing a gpu channel are all notified
// correctly when the channel is established (or fails to establish).
TEST_F(GpuTest, EstablishRequestsQueued) {
  int counter = 2;
  base::RunLoop run_loop;
  // A callback that decrements the counter, and runs the callback when the
  // counter reaches 0.
  auto callback = base::Bind(
      [](int* counter, const base::Closure& callback,
         scoped_refptr<gpu::GpuChannelHost> channel) {
        EXPECT_TRUE(channel);
        --(*counter);
        if (*counter == 0)
          callback.Run();
      },
      &counter, run_loop.QuitClosure());
  gpu()->EstablishGpuChannel(callback);
  gpu()->EstablishGpuChannel(callback);
  EXPECT_EQ(2, counter);
  run_loop.Run();
  EXPECT_EQ(0, counter);
}

// Tests that a new request for establishing a gpu channel from a callback of a
// previous callback is processed correctly.
TEST_F(GpuTest, EstablishRequestOnFailureOnPreviousRequest) {
  // Make the first request fail.
  gpu_impl()->SetRequestWillSucceed(false);

  base::RunLoop run_loop;
  scoped_refptr<gpu::GpuChannelHost> host;
  auto callback = base::Bind(
      [](scoped_refptr<gpu::GpuChannelHost>* out_host,
         const base::Closure& callback,
         scoped_refptr<gpu::GpuChannelHost> host) {
        callback.Run();
        *out_host = std::move(host);
      },
      &host, run_loop.QuitClosure());
  gpu()->EstablishGpuChannel(base::Bind(
      [](ui::Gpu* gpu, TestGpuImpl* gpu_impl,
         const gpu::GpuChannelEstablishedCallback& callback,
         scoped_refptr<gpu::GpuChannelHost> host) {
        EXPECT_FALSE(host);
        // Responding to the first request would issue a second request
        // immediately which should succeed.
        gpu_impl->SetRequestWillSucceed(true);
        gpu->EstablishGpuChannel(callback);
      },
      gpu(), gpu_impl(), callback));

  run_loop.Run();
  EXPECT_TRUE(host);
}

// Tests that if a request for a gpu channel succeeded, then subsequent requests
// are met synchronously.
TEST_F(GpuTest, EstablishRequestResponseSynchronouslyOnSuccess) {
  base::RunLoop run_loop;
  gpu()->EstablishGpuChannel(base::Bind(
      [](const base::Closure& callback,
         scoped_refptr<gpu::GpuChannelHost> host) {
        EXPECT_TRUE(host);
        callback.Run();
      },
      run_loop.QuitClosure()));
  run_loop.Run();

  int counter = 0;
  auto callback = base::Bind(
      [](int* counter, scoped_refptr<gpu::GpuChannelHost> host) {
        EXPECT_TRUE(host);
        ++(*counter);
      },
      &counter);
  gpu()->EstablishGpuChannel(callback);
  EXPECT_EQ(1, counter);
}

// Tests that if EstablishGpuChannel() was called but hasn't finished yet and
// EstablishGpuChannelSync() is called, that EstablishGpuChannelSync() blocks
// the thread until the original call returns.
TEST_F(GpuTest, EstablishRequestAsyncThenSync) {
  int counter = 0;
  gpu()->EstablishGpuChannel(base::Bind(
      [](int* counter, scoped_refptr<gpu::GpuChannelHost> host) {
        EXPECT_TRUE(host);
        ++(*counter);
      },
      base::Unretained(&counter)));

  bool connection_error = false;
  scoped_refptr<gpu::GpuChannelHost> host =
      gpu()->EstablishGpuChannelSync(&connection_error);
  EXPECT_FALSE(connection_error);
  EXPECT_EQ(1, counter);
  EXPECT_TRUE(host);
}

// Tests that if EstablishGpuChannelSync() is called after a request for
// EstablishGpuChannel() has returned that request is used immediately.
TEST_F(GpuTest, EstablishRequestAsyncThenSyncWithResponse) {
  int counter = 0;
  gpu()->EstablishGpuChannel(base::Bind(
      [](int* counter, scoped_refptr<gpu::GpuChannelHost> host) {
        EXPECT_TRUE(host);
        ++(*counter);
      },
      base::Unretained(&counter)));

  BlockMainFlushIO();

  // Gpu will be in a state where a response for EstablishGpuChannel() has
  // been received on the IO thread and a task has been posted to the main
  // thread to process it. The main thread task hasn't run yet so the callback
  // won't have run.
  EXPECT_EQ(0, counter);

  // Run EstablishGpuChannelSync() before the posted task can run. The response
  // to the async request should be used immediately, the pending callback
  // should fire and then EstablishGpuChannelSync() should return.
  bool connection_error = false;
  scoped_refptr<gpu::GpuChannelHost> host =
      gpu()->EstablishGpuChannelSync(&connection_error);
  EXPECT_FALSE(connection_error);
  EXPECT_EQ(1, counter);
  EXPECT_TRUE(host);

  // The original posted task will do nothing when it runs later.
}

// Tests that if Gpu is destroyed with a pending request it doesn't cause any
// issues.
TEST_F(GpuTest, DestroyGpuWithPendingRequest) {
  int counter = 0;
  gpu()->EstablishGpuChannel(
      base::Bind([](int* counter,
                    scoped_refptr<gpu::GpuChannelHost> host) { ++(*counter); },
                 base::Unretained(&counter)));

  // This should cancel the pending request and not crash.
  DestroyGpu();

  // The callback shouldn't be called.
  FlushMainAndIO();
  EXPECT_EQ(0, counter);
}

}  // namespace ui
