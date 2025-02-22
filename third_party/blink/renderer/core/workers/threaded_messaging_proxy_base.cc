// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/workers/threaded_messaging_proxy_base.h"

#include "base/synchronization/waitable_event.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/renderer/bindings/core/v8/source_location.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/deprecation.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_client.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/inspector/devtools_agent.h"
#include "third_party/blink/renderer/core/inspector/worker_devtools_params.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/core/loader/worker_fetch_context.h"
#include "third_party/blink/renderer/core/workers/global_scope_creation_params.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"
#include "third_party/blink/renderer/platform/wtf/time.h"

namespace blink {

namespace {

static int g_live_messaging_proxy_count = 0;

}  // namespace

ThreadedMessagingProxyBase::ThreadedMessagingProxyBase(
    ExecutionContext* execution_context)
    : execution_context_(execution_context),
      parent_execution_context_task_runners_(
          ParentExecutionContextTaskRunners::Create(execution_context_.Get())),
      terminate_sync_load_event_(
          base::WaitableEvent::ResetPolicy::MANUAL,
          base::WaitableEvent::InitialState::NOT_SIGNALED),
      keep_alive_(this) {
  DCHECK(IsParentContextThread());
  g_live_messaging_proxy_count++;
}

ThreadedMessagingProxyBase::~ThreadedMessagingProxyBase() {
  g_live_messaging_proxy_count--;
}

int ThreadedMessagingProxyBase::ProxyCount() {
  DCHECK(IsMainThread());
  return g_live_messaging_proxy_count;
}

void ThreadedMessagingProxyBase::Trace(blink::Visitor* visitor) {
  visitor->Trace(execution_context_);
}

void ThreadedMessagingProxyBase::InitializeWorkerThread(
    std::unique_ptr<GlobalScopeCreationParams> global_scope_creation_params,
    const base::Optional<WorkerBackingThreadStartupData>& thread_startup_data) {
  DCHECK(IsParentContextThread());

  KURL script_url = global_scope_creation_params->script_url.Copy();

  scoped_refptr<WebWorkerFetchContext> web_worker_fetch_context;
  if (auto* document = DynamicTo<Document>(execution_context_.Get())) {
    LocalFrame* frame = document->GetFrame();
    web_worker_fetch_context = frame->Client()->CreateWorkerFetchContext();
    // |web_worker_fetch_context| is null in some unit tests.
    if (web_worker_fetch_context) {
      web_worker_fetch_context->SetApplicationCacheHostID(
          GetExecutionContext()->Fetcher()->Context().ApplicationCacheHostID());
      web_worker_fetch_context->SetIsOnSubframe(!frame->IsMainFrame());
    }
  } else if (auto* scope = DynamicTo<WorkerGlobalScope>(*execution_context_)) {
    web_worker_fetch_context =
        static_cast<WorkerFetchContext&>(scope->Fetcher()->Context())
            .GetWebWorkerFetchContext()
            ->CloneForNestedWorker();
  }

  if (web_worker_fetch_context) {
    web_worker_fetch_context->SetTerminateSyncLoadEvent(
        &terminate_sync_load_event_);

    // In some cases |web_worker_fetch_context| has already been provided and is
    // overwritten here, and in other cases |web_worker_fetch_context| has been
    // nullptr and is set here.
    // TODO(hiroshige): Clean up this.
    global_scope_creation_params->web_worker_fetch_context =
        std::move(web_worker_fetch_context);
  }

  worker_thread_ = CreateWorkerThread();

  auto devtools_params = DevToolsAgent::WorkerThreadCreated(
      execution_context_.Get(), worker_thread_.get(), script_url);

  worker_thread_->Start(std::move(global_scope_creation_params),
                        thread_startup_data, std::move(devtools_params),
                        GetParentExecutionContextTaskRunners());

  if (auto* scope = DynamicTo<WorkerGlobalScope>(*execution_context_)) {
    scope->GetThread()->ChildThreadStartedOnWorkerThread(worker_thread_.get());
  }
}

void ThreadedMessagingProxyBase::CountFeature(WebFeature feature) {
  DCHECK(IsParentContextThread());
  UseCounter::Count(execution_context_, feature);
}

void ThreadedMessagingProxyBase::CountDeprecation(WebFeature feature) {
  DCHECK(IsParentContextThread());
  Deprecation::CountDeprecation(execution_context_, feature);
}

void ThreadedMessagingProxyBase::ReportConsoleMessage(
    MessageSource source,
    MessageLevel level,
    const String& message,
    std::unique_ptr<SourceLocation> location) {
  DCHECK(IsParentContextThread());
  if (asked_to_terminate_)
    return;
  execution_context_->AddConsoleMessage(ConsoleMessage::CreateFromWorker(
      level, message, std::move(location), worker_thread_.get()));
}

void ThreadedMessagingProxyBase::ParentObjectDestroyed() {
  DCHECK(IsParentContextThread());
  if (worker_thread_) {
    // Request to terminate the global scope. This will eventually call
    // WorkerThreadTerminated().
    TerminateGlobalScope();
  } else {
    WorkerThreadTerminated();
  }
}

void ThreadedMessagingProxyBase::WorkerThreadTerminated() {
  DCHECK(IsParentContextThread());

  // This method is always the last to be performed, so the proxy is not
  // needed for communication in either side any more. However, the parent
  // Worker/Worklet object may still exist, and it assumes that the proxy
  // exists, too.
  asked_to_terminate_ = true;
  WorkerThread* parent_thread = nullptr;
  if (auto* scope = DynamicTo<WorkerGlobalScope>(*execution_context_))
    parent_thread = scope->GetThread();
  std::unique_ptr<WorkerThread> child_thread = std::move(worker_thread_);
  if (child_thread) {
    DevToolsAgent::WorkerThreadTerminated(execution_context_.Get(),
                                          child_thread.get());
  }

  // If the parent Worker/Worklet object was already destroyed, this will
  // destroy |this|.
  keep_alive_.Clear();

  if (parent_thread && child_thread)
    parent_thread->ChildThreadTerminatedOnWorkerThread(child_thread.get());
}

void ThreadedMessagingProxyBase::TerminateGlobalScope() {
  DCHECK(IsParentContextThread());

  if (asked_to_terminate_)
    return;
  asked_to_terminate_ = true;

  terminate_sync_load_event_.Signal();

  if (!worker_thread_)
    return;
  worker_thread_->Terminate();
  DevToolsAgent::WorkerThreadTerminated(execution_context_.Get(),
                                        worker_thread_.get());
}

ExecutionContext* ThreadedMessagingProxyBase::GetExecutionContext() const {
  DCHECK(IsParentContextThread());
  return execution_context_;
}

ParentExecutionContextTaskRunners*
ThreadedMessagingProxyBase::GetParentExecutionContextTaskRunners() const {
  DCHECK(IsParentContextThread());
  return parent_execution_context_task_runners_;
}

WorkerThread* ThreadedMessagingProxyBase::GetWorkerThread() const {
  DCHECK(IsParentContextThread());
  return worker_thread_.get();
}

bool ThreadedMessagingProxyBase::IsParentContextThread() const {
  return execution_context_->IsContextThread();
}

}  // namespace blink
