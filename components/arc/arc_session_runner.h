// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_ARC_ARC_SESSION_RUNNER_H_
#define COMPONENTS_ARC_ARC_SESSION_RUNNER_H_

#include <memory>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/threading/thread_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chromeos/dbus/session_manager_client.h"
#include "components/arc/arc_instance_mode.h"
#include "components/arc/arc_session.h"
#include "components/arc/arc_stop_reason.h"

namespace arc {

// These enums are used to define the buckets for an enumerated UMA histogram
// and need to be synced with tools/metrics/histograms/enums.xml. This enum
// class should also be treated as append-only.
enum class ArcContainerLifetimeEvent {
  // Note: "container" here means "instance". Outside Chromium, like UMA
  // dashboard, we use the former term.

  // Chrome asked session_manager to start an ARC instance (of any kind). We
  // record this as a baseline.
  CONTAINER_STARTING = 0,
  // The instance failed to start or exited unexpectedly.
  CONTAINER_FAILED_TO_START = 1,
  // The instance crashed before establishing an IPC connection to Chrome.
  CONTAINER_CRASHED_EARLY = 2,
  // The instance crashed after establishing the connection.
  CONTAINER_CRASHED = 3,
  COUNT
};

// Accept requests to start/stop ARC instance. Also supports automatic
// restarting on unexpected ARC instance crash.
class ArcSessionRunner : public ArcSession::Observer,
                         public chromeos::SessionManagerClient::Observer {
 public:
  // Observer to notify events across multiple ARC session runs.
  class Observer {
   public:
    // Called when ARC instance is stopped. If |restarting| is true, another
    // ARC session is being restarted (practically after certain delay).
    // Note: this is called once per ARC session, including unexpected
    // CRASH on ARC container, and expected SHUTDOWN of ARC triggered by
    // RequestStop(), so may be called multiple times for one RequestStart().
    virtual void OnSessionStopped(ArcStopReason reason, bool restarting) = 0;

    // Called when ARC session is stopped, but is being restarted automatically.
    // Unlike OnSessionStopped() with |restarting| == true, this is called
    // _after_ the container is actually created.
    virtual void OnSessionRestarting() = 0;

   protected:
    virtual ~Observer() = default;
  };

  // This is the factory interface to inject ArcSession instance
  // for testing purpose.
  using ArcSessionFactory = base::Callback<std::unique_ptr<ArcSession>()>;

  explicit ArcSessionRunner(const ArcSessionFactory& factory);
  ~ArcSessionRunner() override;

  // Add/Remove an observer.
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Starts the ARC service, then it will connect the Mojo channel. When the
  // bridge becomes ready, registered Observer's OnSessionReady() is called.
  void RequestStart(ArcInstanceMode request_mode);

  // Stops the ARC service.
  void RequestStop();

  // OnShutdown() should be called when the browser is shutting down. This can
  // only be called on the thread that this class was created on. We assume that
  // when this function is called, MessageLoop is no longer exists.
  void OnShutdown();

  // Returns the current ArcSession instance for testing purpose.
  ArcSession* GetArcSessionForTesting() { return arc_session_.get(); }

  // Normally, automatic restarting happens after a short delay. When testing,
  // however, we'd like it to happen immediately to avoid adding unnecessary
  // delays.
  void SetRestartDelayForTesting(const base::TimeDelta& restart_delay);

 private:
  // Starts to run an ARC instance.
  void StartArcSession();

  // Restarts an ARC instance.
  void RestartArcSession();

  // ArcSession::Observer:
  void OnSessionStopped(ArcStopReason reason, bool was_running) override;

  // chromeos::SessionManagerClient::Observer:
  void EmitLoginPromptVisibleCalled() override;

  THREAD_CHECKER(thread_checker_);

  // Observers for the ARC instance state change events.
  base::ObserverList<Observer> observer_list_;

  // Target ARC instance running mode. If nullopt, it means the ARC instance
  // should stop eventually.
  base::Optional<ArcInstanceMode> target_mode_;

  // Instead of immediately trying to restart the container, give it some time
  // to finish tearing down in case it is still in the process of stopping.
  base::TimeDelta restart_delay_;
  base::OneShotTimer restart_timer_;
  size_t restart_after_crash_count_;  // for UMA recording.

  // Factory to inject a fake ArcSession instance for testing.
  ArcSessionFactory factory_;

  // ArcSession object for currently running ARC instance. This should be
  // nullptr if the state is STOPPED, otherwise non-nullptr.
  std::unique_ptr<ArcSession> arc_session_;

  // WeakPtrFactory to use callbacks.
  base::WeakPtrFactory<ArcSessionRunner> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(ArcSessionRunner);
};

}  // namespace arc

#endif  // COMPONENTS_ARC_ARC_SESSION_RUNNER_H_
