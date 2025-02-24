// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_WEB_PUBLIC_DOWNLOAD_DOWNLOAD_TASK_OBSERVER_H_
#define IOS_WEB_PUBLIC_DOWNLOAD_DOWNLOAD_TASK_OBSERVER_H_

#include "base/macros.h"

namespace web {

class DownloadTask;

// Allows observation of DownloadTask updates. All methods are called on UI
// thread.
class DownloadTaskObserver {
 public:
  // Called when the download task has started, downloaded a chunk of data or
  // the download has been completed. Clients may call DownloadTask::IsDone() to
  // check if the task has completed, call DownloadTask::GetErrorCode() to check
  // if the download has failed, DownloadTask::GetPercentComplete() to check
  // the download progress, and DownloadTask::GetResponseWriter() to obtain the
  // downloaded data.
  virtual void OnDownloadUpdated(const DownloadTask* task) {}

  DownloadTaskObserver() = default;
  virtual ~DownloadTaskObserver() = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(DownloadTaskObserver);
};

}  // namespace web

#endif  // IOS_WEB_PUBLIC_DOWNLOAD_DOWNLOAD_TASK_OBSERVER_H_
