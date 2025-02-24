// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/metrics/oom/out_of_memory_reporter.h"

#include "base/logging.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"

DEFINE_WEB_CONTENTS_USER_DATA_KEY(OutOfMemoryReporter);

OutOfMemoryReporter::~OutOfMemoryReporter() {}

void OutOfMemoryReporter::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void OutOfMemoryReporter::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

OutOfMemoryReporter::OutOfMemoryReporter(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents)
#if defined(OS_ANDROID)
      ,
      scoped_observer_(this) {
  // This adds N async observers for N WebContents, which isn't great but
  // probably won't be a big problem on Android, where many multiple tabs are
  // rarer.
  auto* crash_manager = breakpad::CrashDumpManager::GetInstance();
  DCHECK(crash_manager);
  scoped_observer_.Add(crash_manager);
#else
{
#endif
}

void OutOfMemoryReporter::OnForegroundOOMDetected(const GURL& url,
                                                  ukm::SourceId source_id) {
  for (auto& observer : observers_) {
    observer.OnForegroundOOMDetected(url, source_id);
  }
}

void OutOfMemoryReporter::DidFinishNavigation(
    content::NavigationHandle* handle) {
  // Only care about main frame navigations that commit to another document.
  if (!handle->IsInMainFrame() || !handle->HasCommitted() ||
      handle->IsSameDocument()) {
    return;
  }
  last_committed_source_id_.reset();
  crashed_render_process_id_ = content::ChildProcessHost::kInvalidUniqueID;
  if (handle->IsErrorPage())
    return;
  last_committed_source_id_ = ukm::ConvertToSourceId(
      handle->GetNavigationId(), ukm::SourceIdType::NAVIGATION_ID);
}

void OutOfMemoryReporter::RenderProcessGone(base::TerminationStatus status) {
  if (!last_committed_source_id_.has_value())
    return;
  if (!web_contents()->IsVisible())
    return;

  crashed_render_process_id_ =
      web_contents()->GetMainFrame()->GetProcess()->GetID();

// On Android, we care about OOM protected crashes, which are obtained via
// crash dump analysis. Otherwise we can use the termination status to
// deterine OOM.
#if !defined(OS_ANDROID)
  if (status == base::TERMINATION_STATUS_OOM
#if defined(OS_CHROMEOS)
      || status == base::TERMINATION_STATUS_PROCESS_WAS_KILLED_BY_OOM
#endif
      ) {
    OnForegroundOOMDetected(web_contents()->GetLastCommittedURL(),
                            *last_committed_source_id_);
  }
#endif  // !defined(OS_ANDROID)
}

#if defined(OS_ANDROID)
// This should always be called *after* the associated RenderProcessGone. This
// is because the crash dump is processed asynchronously on the IO thread in
// response to RenderProcessHost::ProcessDied, while RenderProcessGone is called
// synchronously from the call to ProcessDied.
void OutOfMemoryReporter::OnCrashDumpProcessed(
    const breakpad::CrashDumpManager::CrashDumpDetails& details) {
  if (!last_committed_source_id_.has_value())
    return;
  // Make sure the crash happened in the correct RPH.
  if (details.process_host_id != crashed_render_process_id_)
    return;

  if (details.process_type == content::PROCESS_TYPE_RENDERER &&
      details.termination_status == base::TERMINATION_STATUS_OOM_PROTECTED &&
      details.file_size == 0 &&
      details.app_state ==
          base::android::APPLICATION_STATE_HAS_RUNNING_ACTIVITIES) {
    OnForegroundOOMDetected(web_contents()->GetLastCommittedURL(),
                            *last_committed_source_id_);
  }
}
#endif  // defined(OS_ANDROID)
