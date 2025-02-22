// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy/browser_dm_token_storage.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string16.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/task_runner_util.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "content/public/browser/browser_thread.h"

namespace policy {

// static
BrowserDMTokenStorage* BrowserDMTokenStorage::storage_for_testing_ = nullptr;

BrowserDMTokenStorage::BrowserDMTokenStorage() : is_initialized_(false) {
  DETACH_FROM_SEQUENCE(sequence_checker_);

  // We don't call InitIfNeeded() here so that the global instance can be
  // created early during startup if needed. The tokens and client ID are read
  // from the system as part of the first retrieve or store operation.
}

BrowserDMTokenStorage::~BrowserDMTokenStorage() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::string BrowserDMTokenStorage::RetrieveClientId() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  InitIfNeeded();
  return client_id_;
}

std::string BrowserDMTokenStorage::RetrieveEnrollmentToken() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  InitIfNeeded();
  return enrollment_token_;
}

void BrowserDMTokenStorage::StoreDMToken(const std::string& dm_token,
                                         StoreCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!store_callback_);
  InitIfNeeded();

  dm_token_ = dm_token;

  store_callback_ = std::move(callback);

  SaveDMToken(dm_token);
}

std::string BrowserDMTokenStorage::RetrieveDMToken() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!store_callback_);

  InitIfNeeded();
  return dm_token_;
}

void BrowserDMTokenStorage::OnDMTokenStored(bool success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(store_callback_);

  if (!store_callback_.is_null())
    std::move(store_callback_).Run(success);
}

bool BrowserDMTokenStorage::ShouldDisplayErrorMessageOnFailure() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  InitIfNeeded();
  return should_display_error_message_on_failure_;
}

void BrowserDMTokenStorage::ScheduleUnusedPolicyDirectoryDeletion() {
  // TODO(crbug.com/883869): Add a UMA metrics to track the deletion progress.
  content::BrowserThread::PostAfterStartupTask(
      FROM_HERE,
      base::CreateTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}),
      base::BindOnce(&BrowserDMTokenStorage::DeletePolicyDirectory,
                     base::Unretained(this)));
}

void BrowserDMTokenStorage::InitIfNeeded() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (is_initialized_)
    return;

  is_initialized_ = true;

  // Only supported in official builds.
  client_id_ = InitClientId();
  DVLOG(1) << "Client ID = " << client_id_;
  if (client_id_.empty())
    return;

  enrollment_token_ = InitEnrollmentToken();
  DVLOG(1) << "Enrollment token = " << enrollment_token_;

  dm_token_ = InitDMToken();
  DVLOG(1) << "DM Token = " << dm_token_;

  should_display_error_message_on_failure_ = InitEnrollmentErrorOption();
}

void BrowserDMTokenStorage::DeletePolicyDirectory() {}

}  // namespace policy
