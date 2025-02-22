// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_SCRIPT_TRACKER_H_
#define COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_SCRIPT_TRACKER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "components/autofill_assistant/browser/script.h"
#include "components/autofill_assistant/browser/script_executor.h"
#include "components/autofill_assistant/browser/service.pb.h"

namespace autofill_assistant {
class ScriptExecutorDelegate;

// The script tracker keeps track of which scripts are available, which are
// running, which have run, which are runnable whose preconditions are met.
//
// User of this class is responsible for retrieving and passing scripts to the
// tracker and letting the tracker know about changes to the DOM.
class ScriptTracker : public ScriptExecutor::Listener {
 public:
  // Listens to changes on the ScriptTracker state.
  class Listener {
   public:
    virtual ~Listener() = default;

    // Called when the set of runnable scripts have changed. |runnable_scripts|
    // are the new runnable scripts. Runnable scripts are ordered by priority.
    virtual void OnRunnableScriptsChanged(
        const std::vector<ScriptHandle>& runnable_scripts) = 0;

    // Called when there are no more runnable scripts anymore and there cannot
    // be any without navigating to another page.
    //
    // This is not called if a DOM change could make some scripts runnable.
    //
    // This is not called until some scripts have been reported runnable to
    // OnRunnableScriptsChanged at least once.
    virtual void OnNoRunnableScriptsAnymore() = 0;
  };

  // |delegate| and |listener| should outlive this object and should not be
  // nullptr.
  ScriptTracker(ScriptExecutorDelegate* delegate,
                ScriptTracker::Listener* listener);

  ~ScriptTracker() override;

  // Updates the set of available |scripts|. This interrupts any pending checks,
  // but don't start a new one.'
  void SetScripts(std::vector<std::unique_ptr<Script>> scripts);

  // Run the preconditions on the current set of scripts, and possibly update
  // the set of runnable scripts.
  //
  // Calling CheckScripts() while a check is in progress cancels the previously
  // running check and starts a new one right away.
  void CheckScripts(const base::TimeDelta& max_duration);

  // Runs a script and reports, when the script has ended, whether the run was
  // successful. Fails immediately if a script is already running.
  //
  // Scripts that are already executed won't be considered runnable anymore.
  // Call CheckScripts to refresh the set of runnable script after script
  // execution.
  void ExecuteScript(const std::string& path,
                     ScriptExecutor::RunScriptCallback callback);

  // Clears the set of scripts that could be run.
  //
  // Calling this function will clean the bottom bar.
  void ClearRunnableScripts();

  // Checks whether a script is currently running. There can be at most one
  // script running at a time.
  bool running() const { return executor_ != nullptr; }

  // Returns a dictionary describing the current execution context, which
  // is intended to be serialized as JSON string. The execution context is
  // useful when analyzing feedback forms and for debugging in general.
  base::Value GetDebugContext() const;

 private:
  typedef std::map<Script*, std::unique_ptr<Script>> AvailableScriptMap;

  void OnScriptRun(const std::string& script_path,
                   ScriptExecutor::RunScriptCallback original_callback,
                   ScriptExecutor::Result result);
  void UpdateRunnableScriptsIfNecessary();
  void OnCheckDone();

  // Overrides ScriptExecutor::Listener.
  void OnServerPayloadChanged(const std::string& server_payload) override;

  // Stops running pending checks and cleans up any state used by pending
  // checks. This can safely be called at any time, including when no checks are
  // running.
  void TerminatePendingChecks();

  // Returns true if |runnable_| should be updated.
  bool RunnablesHaveChanged();
  void OnPreconditionCheck(Script* script, bool met_preconditions);
  void ClearAvailableScripts();

  ScriptExecutorDelegate* const delegate_;
  ScriptTracker::Listener* const listener_;

  // Paths and names of scripts known to be runnable (they pass the
  // preconditions).
  //
  // NOTE 1: Set of runnable scripts can survive a SetScripts; as
  // long as the new set of runnable script has the same path, it won't be seen
  // as a change to the set of runnable, even if the pointers have changed.
  // NOTE 2: Set of runnable scripts should be in sync with what is displayed on
  // the bottom bar.
  std::vector<ScriptHandle> runnable_scripts_;

  // True if OnRunnableScriptsChanged was called at least once - necessarily
  // with a non-empty set of scripts the first time, since the tracker starts
  // with an empty set of scripts.
  bool reported_runnable_scripts_;

  // Sets of available scripts. SetScripts resets this and interrupts
  // any pending check.
  AvailableScriptMap available_scripts_;

  // List of scripts that have been executed and their corresponding statuses.
  std::map<std::string, ScriptStatusProto> executed_scripts_;
  std::unique_ptr<BatchElementChecker> batch_element_checker_;

  // Scripts found to be runnable so far, in the current check, represented by
  // |batch_element_checker_|.
  std::vector<Script*> pending_runnable_scripts_;

  // If a script is currently running, this is the script's executor. Otherwise,
  // this is nullptr.
  std::unique_ptr<ScriptExecutor> executor_;

  std::string last_server_payload_;

  base::WeakPtrFactory<ScriptTracker> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(ScriptTracker);
};

}  // namespace autofill_assistant

#endif  // COMPONENTS_AUTOFILL_ASSISTANT_BROWSER_SCRIPT_TRACKER_H_
