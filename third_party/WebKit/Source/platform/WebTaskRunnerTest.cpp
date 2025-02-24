// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/WebTaskRunner.h"

#include "platform/scheduler/test/fake_web_task_runner.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace blink {
namespace {

void Increment(int* x) {
  ++*x;
}

void GetIsActive(bool* is_active, TaskHandle* handle) {
  *is_active = handle->IsActive();
}

class CancellationTestHelper {
 public:
  CancellationTestHelper() : weak_ptr_factory_(this) {}

  WeakPtr<CancellationTestHelper> CreateWeakPtr() {
    return weak_ptr_factory_.CreateWeakPtr();
  }

  void RevokeWeakPtrs() { weak_ptr_factory_.RevokeAll(); }
  void IncrementCounter() { ++counter_; }
  int Counter() const { return counter_; }

 private:
  int counter_ = 0;
  WeakPtrFactory<CancellationTestHelper> weak_ptr_factory_;
};

}  // namespace

TEST(WebTaskRunnerTest, PostCancellableTaskTest) {
  scoped_refptr<scheduler::FakeWebTaskRunner> task_runner =
      base::AdoptRef(new scheduler::FakeWebTaskRunner);

  // Run without cancellation.
  int count = 0;
  TaskHandle handle = task_runner->PostCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&Increment, WTF::Unretained(&count)));
  EXPECT_EQ(0, count);
  EXPECT_TRUE(handle.IsActive());
  task_runner->RunUntilIdle();
  EXPECT_EQ(1, count);
  EXPECT_FALSE(handle.IsActive());

  count = 0;
  handle = task_runner->PostDelayedCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&Increment, WTF::Unretained(&count)),
      TimeDelta::FromMilliseconds(1));
  EXPECT_EQ(0, count);
  EXPECT_TRUE(handle.IsActive());
  task_runner->RunUntilIdle();
  EXPECT_EQ(1, count);
  EXPECT_FALSE(handle.IsActive());

  // Cancel a task.
  count = 0;
  handle = task_runner->PostCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&Increment, WTF::Unretained(&count)));
  handle.Cancel();
  EXPECT_EQ(0, count);
  EXPECT_FALSE(handle.IsActive());
  task_runner->RunUntilIdle();
  EXPECT_EQ(0, count);

  // The task should be cancelled when the handle is dropped.
  {
    count = 0;
    TaskHandle handle2 = task_runner->PostCancellableTask(
        BLINK_FROM_HERE, WTF::Bind(&Increment, WTF::Unretained(&count)));
    EXPECT_TRUE(handle2.IsActive());
  }
  EXPECT_EQ(0, count);
  task_runner->RunUntilIdle();
  EXPECT_EQ(0, count);

  // The task should be cancelled when another TaskHandle is assigned on it.
  count = 0;
  handle = task_runner->PostCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&Increment, WTF::Unretained(&count)));
  handle = task_runner->PostCancellableTask(BLINK_FROM_HERE, WTF::Bind([] {}));
  EXPECT_EQ(0, count);
  task_runner->RunUntilIdle();
  EXPECT_EQ(0, count);

  // Self assign should be nop.
  count = 0;
  handle = task_runner->PostCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&Increment, WTF::Unretained(&count)));
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
  handle = std::move(handle);
#pragma GCC diagnostic pop
#else
  handle = std::move(handle);
#endif  // defined(__clang__)
  EXPECT_EQ(0, count);
  task_runner->RunUntilIdle();
  EXPECT_EQ(1, count);

  // handle->isActive() should switch to false before the task starts running.
  bool is_active = false;
  handle = task_runner->PostCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&GetIsActive, WTF::Unretained(&is_active),
                                 WTF::Unretained(&handle)));
  EXPECT_TRUE(handle.IsActive());
  task_runner->RunUntilIdle();
  EXPECT_FALSE(is_active);
  EXPECT_FALSE(handle.IsActive());
}

TEST(WebTaskRunnerTest, CancellationCheckerTest) {
  scoped_refptr<scheduler::FakeWebTaskRunner> task_runner =
      base::AdoptRef(new scheduler::FakeWebTaskRunner);

  int count = 0;
  TaskHandle handle = task_runner->PostCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&Increment, WTF::Unretained(&count)));
  EXPECT_EQ(0, count);

  // TaskHandle::isActive should detect the deletion of posted task.
  auto queue = task_runner->TakePendingTasksForTesting();
  ASSERT_EQ(1u, queue.size());
  EXPECT_FALSE(queue[0].first.IsCancelled());
  EXPECT_TRUE(handle.IsActive());
  queue.clear();
  EXPECT_FALSE(handle.IsActive());
  EXPECT_EQ(0, count);

  count = 0;
  CancellationTestHelper helper;
  handle = task_runner->PostCancellableTask(
      BLINK_FROM_HERE, WTF::Bind(&CancellationTestHelper::IncrementCounter,
                                 helper.CreateWeakPtr()));
  EXPECT_EQ(0, helper.Counter());

  // The cancellation of the posted task should be propagated to TaskHandle.
  queue = task_runner->TakePendingTasksForTesting();
  ASSERT_EQ(1u, queue.size());
  EXPECT_FALSE(queue[0].first.IsCancelled());
  EXPECT_TRUE(handle.IsActive());
  helper.RevokeWeakPtrs();
  EXPECT_TRUE(queue[0].first.IsCancelled());
  EXPECT_FALSE(handle.IsActive());
}

}  // namespace blink
