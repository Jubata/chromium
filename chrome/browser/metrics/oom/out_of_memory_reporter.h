// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_METRICS_OOM_OUT_OF_MEMORY_REPORTER_H_
#define CHROME_BROWSER_METRICS_OOM_OUT_OF_MEMORY_REPORTER_H_

#include "base/macros.h"
#include "base/observer_list.h"
#include "base/scoped_observer.h"
#include "build/build_config.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/common/child_process_host.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "url/gurl.h"

#if defined(OS_ANDROID)
#include "components/crash/content/browser/crash_dump_manager_android.h"
#endif

// This class listens for OOM notifications from WebContentsObserver and
// breakpad::CrashDumpManager::Observer methods. It forwards foreground OOM
// notifications to observers.
class OutOfMemoryReporter
    : public content::WebContentsObserver,
      public content::WebContentsUserData<OutOfMemoryReporter>
#if defined(OS_ANDROID)
    ,
      public breakpad::CrashDumpManager::Observer
#endif
{
 public:
  class Observer {
   public:
    virtual void OnForegroundOOMDetected(const GURL& url,
                                         ukm::SourceId source_id) = 0;
  };
  ~OutOfMemoryReporter() override;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  friend class content::WebContentsUserData<OutOfMemoryReporter>;

  OutOfMemoryReporter(content::WebContents* web_contents);
  void OnForegroundOOMDetected(const GURL& url, ukm::SourceId source_id);

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override;
  void RenderProcessGone(base::TerminationStatus termination_status) override;

// breakpad::CrashDumpManager::Observer:
#if defined(OS_ANDROID)
  void OnCrashDumpProcessed(
      const breakpad::CrashDumpManager::CrashDumpDetails& details) override;

  ScopedObserver<breakpad::CrashDumpManager,
                 breakpad::CrashDumpManager::Observer>
      scoped_observer_;
#endif  // defined(OS_ANDROID)

  base::ObserverList<Observer> observers_;

  base::Optional<ukm::SourceId> last_committed_source_id_;
  int crashed_render_process_id_ = content::ChildProcessHost::kInvalidUniqueID;

  DISALLOW_COPY_AND_ASSIGN(OutOfMemoryReporter);
};

#endif  // CHROME_BROWSER_METRICS_OOM_OUT_OF_MEMORY_REPORTER_H_
