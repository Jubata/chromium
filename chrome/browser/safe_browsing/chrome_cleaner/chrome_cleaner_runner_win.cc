// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/chrome_cleaner/chrome_cleaner_runner_win.h"

#include <utility>

#include "base/base_paths.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/feature_list.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/task_scheduler/post_task.h"
#include "chrome/browser/safe_browsing/chrome_cleaner/srt_client_info_win.h"
#include "chrome/browser/safe_browsing/chrome_cleaner/srt_field_trial_win.h"
#include "chrome/installer/util/install_util.h"
#include "components/chrome_cleaner/public/constants/constants.h"
#include "components/chrome_cleaner/public/interfaces/chrome_prompt.mojom.h"
#include "components/version_info/version_info.h"
#include "mojo/edk/embedder/connection_params.h"
#include "mojo/edk/embedder/embedder.h"
#include "mojo/edk/embedder/outgoing_broker_client_invitation.h"
#include "mojo/edk/embedder/platform_channel_pair.h"
#include "mojo/edk/embedder/transport_protocol.h"
#include "mojo/public/cpp/system/message_pipe.h"

using chrome_cleaner::mojom::ChromePrompt;
using chrome_cleaner::mojom::ChromePromptRequest;
using content::BrowserThread;

namespace safe_browsing {

namespace {

// Global delegate used to override the launching of the Cleaner process during
// tests.
ChromeCleanerRunnerTestDelegate* g_test_delegate = nullptr;

}  // namespace

ChromeCleanerRunner::ProcessStatus::ProcessStatus(LaunchStatus launch_status,
                                                  int exit_code)
    : launch_status(launch_status), exit_code(exit_code) {}

// static
void ChromeCleanerRunner::RunChromeCleanerAndReplyWithExitCode(
    const base::FilePath& cleaner_executable_path,
    const SwReporterInvocation& reporter_invocation,
    ChromeMetricsStatus metrics_status,
    ChromePromptImpl::OnPromptUser on_prompt_user,
    base::OnceClosure on_connection_closed,
    ChromeCleanerRunner::ProcessDoneCallback on_process_done,
    scoped_refptr<base::SequencedTaskRunner> task_runner) {
  auto cleaner_runner = base::WrapRefCounted(new ChromeCleanerRunner(
      cleaner_executable_path, reporter_invocation, metrics_status,
      std::move(on_prompt_user), std::move(on_connection_closed),
      std::move(on_process_done), std::move(task_runner)));
  auto launch_and_wait = base::BindOnce(
      &ChromeCleanerRunner::LaunchAndWaitForExitOnBackgroundThread,
      cleaner_runner);
  auto process_done =
      base::BindOnce(&ChromeCleanerRunner::OnProcessDone, cleaner_runner);
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE,
      // LaunchAndWaitForExitOnBackgroundThread creates (MayBlock()) and joins
      // (WithBaseSyncPrimitives()) a process.
      {base::MayBlock(), base::WithBaseSyncPrimitives(),
       base::TaskPriority::BACKGROUND,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      std::move(launch_and_wait), std::move(process_done));
}

ChromeCleanerRunner::ChromeCleanerRunner(
    const base::FilePath& cleaner_executable_path,
    const SwReporterInvocation& reporter_invocation,
    ChromeMetricsStatus metrics_status,
    ChromePromptImpl::OnPromptUser on_prompt_user,
    base::OnceClosure on_connection_closed,
    ProcessDoneCallback on_process_done,
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)),
      cleaner_command_line_(cleaner_executable_path),
      on_prompt_user_(std::move(on_prompt_user)),
      on_connection_closed_(std::move(on_connection_closed)),
      on_process_done_(std::move(on_process_done)) {
  DCHECK(on_prompt_user_);
  DCHECK(on_connection_closed_);
  DCHECK(on_process_done_);
  DCHECK(!cleaner_executable_path.empty());

  // Add the non-IPC switches that should be passed to the Cleaner process.

  // Add switches that pass information about this Chrome installation.
  cleaner_command_line_.AppendSwitchASCII(chrome_cleaner::kChromeVersionSwitch,
                                          version_info::GetVersionNumber());
  cleaner_command_line_.AppendSwitchASCII(chrome_cleaner::kChromeChannelSwitch,
                                          base::IntToString(ChannelAsInt()));
  base::FilePath chrome_exe_path;
  PathService::Get(base::FILE_EXE, &chrome_exe_path);
  cleaner_command_line_.AppendSwitchPath(chrome_cleaner::kChromeExePathSwitch,
                                         chrome_exe_path);
  if (!InstallUtil::IsPerUserInstall())
    cleaner_command_line_.AppendSwitch(
        chrome_cleaner::kChromeSystemInstallSwitch);

  // Start the cleaner process in scanning mode.
  cleaner_command_line_.AppendSwitchASCII(
      chrome_cleaner::kExecutionModeSwitch,
      base::IntToString(
          static_cast<int>(chrome_cleaner::ExecutionMode::kScanning)));

  // If set, forward the engine flag from the reporter. Otherwise, set the
  // engine flag explicitly to 1.
  const std::string& reporter_engine =
      reporter_invocation.command_line.GetSwitchValueASCII(
          chrome_cleaner::kEngineSwitch);
  cleaner_command_line_.AppendSwitchASCII(
      chrome_cleaner::kEngineSwitch,
      reporter_engine.empty() ? "1" : reporter_engine);

  // If metrics is enabled, we can enable crash reporting in the Chrome Cleaner
  // process.
  if (metrics_status == ChromeMetricsStatus::kEnabled) {
    cleaner_command_line_.AppendSwitch(chrome_cleaner::kUmaUserSwitch);
    cleaner_command_line_.AppendSwitch(
        chrome_cleaner::kEnableCrashReportingSwitch);
  }

  const std::string group_name = GetSRTFieldTrialGroupName();
  if (!group_name.empty()) {
    cleaner_command_line_.AppendSwitchASCII(
        chrome_cleaner::kSRTPromptFieldTrialGroupNameSwitch, group_name);
  }
}

ChromeCleanerRunner::ProcessStatus
ChromeCleanerRunner::LaunchAndWaitForExitOnBackgroundThread() {
  mojo::edk::OutgoingBrokerClientInvitation invitation;
  std::string mojo_pipe_token = mojo::edk::GenerateRandomToken();
  mojo::ScopedMessagePipeHandle mojo_pipe =
      invitation.AttachMessagePipe(mojo_pipe_token);
  cleaner_command_line_.AppendSwitchASCII(
      chrome_cleaner::kChromeMojoPipeTokenSwitch, mojo_pipe_token);

  mojo::edk::PlatformChannelPair channel;
  base::LaunchOptions launch_options;
  channel.PrepareToPassClientHandleToChildProcess(
      &cleaner_command_line_, &launch_options.handles_to_inherit);

  base::Process cleaner_process =
      g_test_delegate
          ? g_test_delegate->LaunchTestProcess(cleaner_command_line_,
                                               launch_options)
          : base::LaunchProcess(cleaner_command_line_, launch_options);

  if (!cleaner_process.IsValid())
    return ProcessStatus(LaunchStatus::kLaunchFailed);

  // ChromePromptImpl tasks will need to run on the IO thread. There is no
  // need to synchronize its creation, since the client end will wait for this
  // initialization to be done before sending requests.
  BrowserThread::GetTaskRunnerForThread(BrowserThread::IO)
      ->PostTask(FROM_HERE,
                 base::BindOnce(&ChromeCleanerRunner::CreateChromePromptImpl,
                                base::RetainedRef(this),
                                chrome_cleaner::mojom::ChromePromptRequest(
                                    std::move(mojo_pipe))));

  invitation.Send(
      cleaner_process.Handle(),
      mojo::edk::ConnectionParams(mojo::edk::TransportProtocol::kLegacy,
                                  channel.PassServerHandle()));

  int exit_code = -1;
  if (!cleaner_process.WaitForExit(&exit_code)) {
    return ProcessStatus(
        LaunchStatus::kLaunchSucceededFailedToWaitForCompletion);
  }

  UMA_HISTOGRAM_SPARSE_SLOWLY(
      "SoftwareReporter.Cleaner.ExitCodeFromConnectedProcess", exit_code);
  return ProcessStatus(LaunchStatus::kSuccess, exit_code);
}

ChromeCleanerRunner::~ChromeCleanerRunner() = default;

void ChromeCleanerRunner::CreateChromePromptImpl(
    ChromePromptRequest chrome_prompt_request) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(!chrome_prompt_impl_);

  // Cannot use base::MakeUnique() since it does not support creating
  // std::unique_ptrs with custom deleters.
  chrome_prompt_impl_.reset(new ChromePromptImpl(
      std::move(chrome_prompt_request),
      base::Bind(&ChromeCleanerRunner::OnConnectionClosed,
                 base::RetainedRef(this)),
      base::Bind(&ChromeCleanerRunner::OnPromptUser, base::RetainedRef(this))));
}

void ChromeCleanerRunner::OnPromptUser(
    std::unique_ptr<std::set<base::FilePath>> files_to_delete,
    ChromePrompt::PromptUserCallback prompt_user_callback) {
  if (on_prompt_user_) {
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(std::move(on_prompt_user_),
                                          base::Passed(&files_to_delete),
                                          base::Passed(&prompt_user_callback)));
  }
}

void ChromeCleanerRunner::OnConnectionClosed() {
  if (on_connection_closed_)
    task_runner_->PostTask(FROM_HERE, std::move(on_connection_closed_));
}

void ChromeCleanerRunner::OnProcessDone(ProcessStatus process_status) {
  if (g_test_delegate) {
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&ChromeCleanerRunnerTestDelegate::OnCleanerProcessDone,
                       base::Unretained(g_test_delegate), process_status));
  }

  if (on_process_done_) {
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(on_process_done_), process_status));
  }
}

void SetChromeCleanerRunnerTestDelegateForTesting(
    ChromeCleanerRunnerTestDelegate* test_delegate) {
  g_test_delegate = test_delegate;
}

}  // namespace safe_browsing
