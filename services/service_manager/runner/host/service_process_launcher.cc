// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/service_manager/runner/host/service_process_launcher.h"

#include <utility>

#include "base/base_paths.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/path_service.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/run_loop.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/task/post_task.h"
#include "base/task_runner_util.h"
#include "base/threading/thread.h"
#include "build/build_config.h"
#include "mojo/public/cpp/bindings/interface_ptr_info.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/core.h"
#include "mojo/public/cpp/system/invitation.h"
#include "services/service_manager/public/cpp/standalone_service/switches.h"
#include "services/service_manager/runner/common/client_util.h"
#include "services/service_manager/runner/common/switches.h"
#include "services/service_manager/sandbox/switches.h"

#if defined(OS_LINUX)
#include "sandbox/linux/services/namespace_sandbox.h"
#endif

#if defined(OS_WIN)
#include "base/win/windows_version.h"
#endif

#if defined(OS_MACOSX)
#include "services/service_manager/public/cpp/standalone_service/mach_broker.h"
#endif

namespace service_manager {

// Thread-safe owner of state related to a service process. This facilitates
// safely scheduling the launching and joining of a service process in the
// background.
class ServiceProcessLauncher::ProcessState
    : public base::RefCountedThreadSafe<ProcessState> {
 public:
  ProcessState() { DETACH_FROM_SEQUENCE(sequence_checker_); }

  base::ProcessId LaunchInBackground(
      const Identity& target,
      SandboxType sandbox_type,
      std::unique_ptr<base::CommandLine> child_command_line,
      mojo::PlatformChannel::HandlePassingInfo handle_passing_info,
      mojo::PlatformChannel channel,
      mojo::OutgoingInvitation invitation);

  void StopInBackground();

 private:
  friend class base::RefCountedThreadSafe<ProcessState>;

  ~ProcessState() = default;

  base::Process child_process_;
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ProcessState);
};

ServiceProcessLauncher::ServiceProcessLauncher(
    ServiceProcessLauncherDelegate* delegate,
    const base::FilePath& service_path)
    : delegate_(delegate),
      service_path_(service_path.empty()
                        ? base::CommandLine::ForCurrentProcess()->GetProgram()
                        : service_path),
      background_task_runner_(base::CreateSequencedTaskRunnerWithTraits(
          {base::TaskPriority::USER_VISIBLE, base::WithBaseSyncPrimitives(),
           base::MayBlock()})) {}

ServiceProcessLauncher::~ServiceProcessLauncher() {
  // We don't really need to wait for the process to be joined, as long as it
  // eventually happens. Schedule a task to do it, and move on.
  background_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&ProcessState::StopInBackground, state_));
}

mojom::ServicePtr ServiceProcessLauncher::Start(const Identity& target,
                                                SandboxType sandbox_type,
                                                ProcessReadyCallback callback) {
  DCHECK(!state_);

  const base::CommandLine& parent_command_line =
      *base::CommandLine::ForCurrentProcess();

  std::unique_ptr<base::CommandLine> child_command_line(
      new base::CommandLine(service_path_));

  child_command_line->AppendArguments(parent_command_line, false);
  child_command_line->AppendSwitchASCII(switches::kServiceName, target.name());
#ifndef NDEBUG
  child_command_line->AppendSwitchASCII(
      "g", target.instance_group().value_or(base::Token{}).ToString());
#endif

  if (!IsUnsandboxedSandboxType(sandbox_type)) {
    child_command_line->AppendSwitchASCII(
        switches::kServiceSandboxType,
        StringFromUtilitySandboxType(sandbox_type));
  }

  mojo::PlatformChannel::HandlePassingInfo handle_passing_info;
  mojo::PlatformChannel channel;
  channel.PrepareToPassRemoteEndpoint(&handle_passing_info,
                                      child_command_line.get());
  mojo::OutgoingInvitation invitation;
  mojom::ServicePtr client =
      PassServiceRequestOnCommandLine(&invitation, child_command_line.get());

  if (delegate_) {
    delegate_->AdjustCommandLineArgumentsForTarget(target,
                                                   child_command_line.get());
  }

  state_ = base::WrapRefCounted(new ProcessState);
  base::PostTaskAndReplyWithResult(
      background_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ProcessState::LaunchInBackground, state_, target,
                     sandbox_type, std::move(child_command_line),
                     std::move(handle_passing_info), std::move(channel),
                     std::move(invitation)),
      std::move(callback));

  return client;
}

base::ProcessId ServiceProcessLauncher::ProcessState::LaunchInBackground(
    const Identity& target,
    SandboxType sandbox_type,
    std::unique_ptr<base::CommandLine> child_command_line,
    mojo::PlatformChannel::HandlePassingInfo handle_passing_info,
    mojo::PlatformChannel channel,
    mojo::OutgoingInvitation invitation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::LaunchOptions options;
#if defined(OS_WIN)
  options.handles_to_inherit = handle_passing_info;
  options.stdin_handle = INVALID_HANDLE_VALUE;
  options.stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  options.stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
  // Always inherit stdout/stderr as a pair.
  if (!options.stdout_handle || !options.stdin_handle)
    options.stdin_handle = options.stdout_handle = nullptr;

  // Pseudo handles are used when stdout and stderr redirect to the console. In
  // that case, they're automatically inherited by child processes. See
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682075.aspx
  // Trying to add them to the list of handles to inherit causes CreateProcess
  // to fail. When this process is launched from Python then a real handle is
  // used. In that case, we do want to add it to the list of handles that is
  // inherited.
  if (options.stdout_handle &&
      GetFileType(options.stdout_handle) != FILE_TYPE_CHAR) {
    options.handles_to_inherit.push_back(options.stdout_handle);
  }
  if (options.stderr_handle &&
      GetFileType(options.stderr_handle) != FILE_TYPE_CHAR &&
      options.stdout_handle != options.stderr_handle) {
    options.handles_to_inherit.push_back(options.stderr_handle);
  }
#elif defined(OS_FUCHSIA)
  // LaunchProcess will share stdin/out/err with the child process by default.
  if (!IsUnsandboxedSandboxType(sandbox_type))
    NOTIMPLEMENTED();
  options.handles_to_transfer = std::move(handle_passing_info);
#elif defined(OS_POSIX)
  handle_passing_info.push_back(std::make_pair(STDIN_FILENO, STDIN_FILENO));
  handle_passing_info.push_back(std::make_pair(STDOUT_FILENO, STDOUT_FILENO));
  handle_passing_info.push_back(std::make_pair(STDERR_FILENO, STDERR_FILENO));
  options.fds_to_remap = handle_passing_info;
#endif
  DVLOG(2) << "Launching child with command line: "
           << child_command_line->GetCommandLineString();
#if defined(OS_LINUX)
  if (!IsUnsandboxedSandboxType(sandbox_type)) {
    child_process_ =
        sandbox::NamespaceSandbox::LaunchProcess(*child_command_line, options);
    if (!child_process_.IsValid())
      LOG(ERROR) << "Starting the process with a sandbox failed.";
  } else
#endif
  {
#if defined(OS_MACOSX)
    MachBroker* mach_broker = MachBroker::GetInstance();
    base::AutoLock locker(mach_broker->GetLock());
#endif
    child_process_ = base::LaunchProcess(*child_command_line, options);
#if defined(OS_MACOSX)
    mach_broker->ExpectPid(child_process_.Handle());
#endif
  }

  channel.RemoteProcessLaunchAttempted();
  if (!child_process_.IsValid()) {
    LOG(ERROR) << "Failed to start child process for service: "
               << target.name();
    return base::kNullProcessId;
  }

#if defined(OS_CHROMEOS)
  // Always log instead of DVLOG because knowing which pid maps to which
  // service is vital for interpreting crashes after-the-fact and Chrome OS
  // devices generally run release builds, even in development.
  VLOG(0)
#else
  DVLOG(0)
#endif
      << "Launched child process pid=" << child_process_.Pid()
      << " id=" << target.ToString();

  mojo::OutgoingInvitation::Send(std::move(invitation), child_process_.Handle(),
                                 channel.TakeLocalEndpoint());

  return child_process_.Pid();
}

void ServiceProcessLauncher::ProcessState::StopInBackground() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!child_process_.IsValid())
    return;

  int rv = -1;
  LOG_IF(ERROR, !child_process_.WaitForExit(&rv))
      << "Failed to wait for child process";
  child_process_.Close();
}

}  // namespace service_manager
