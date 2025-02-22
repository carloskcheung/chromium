// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/chromeos_paths.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/system/sys_info.h"

namespace chromeos {

namespace {

const base::FilePath::CharType kDefaultAppOrderFileName[] =
#if defined(GOOGLE_CHROME_BUILD)
    FILE_PATH_LITERAL("/usr/share/google-chrome/default_app_order.json");
#else
    FILE_PATH_LITERAL("/usr/share/chromium/default_app_order.json");
#endif  // defined(GOOGLE_CHROME_BUILD)

const base::FilePath::CharType kDefaultUserPolicyKeysDir[] =
    FILE_PATH_LITERAL("/run/user_policy");

const base::FilePath::CharType kOwnerKeyFileName[] =
    FILE_PATH_LITERAL("/var/lib/whitelist/owner.key");

const base::FilePath::CharType kInstallAttributesFileName[] =
    FILE_PATH_LITERAL("/run/lockbox/install_attributes.pb");

const base::FilePath::CharType kMachineHardwareInfoFileName[] =
    FILE_PATH_LITERAL("/tmp/machine-info");

const base::FilePath::CharType kVpdFileName[] = FILE_PATH_LITERAL(
    "/mnt/stateful_partition/unencrypted/cache/vpd/filtered.txt");

const base::FilePath::CharType kUptimeFileName[] =
    FILE_PATH_LITERAL("/proc/uptime");

const base::FilePath::CharType kUpdateRebootNeededUptimeFile[] =
    FILE_PATH_LITERAL("/run/chrome/update_reboot_needed_uptime");

const base::FilePath::CharType kDeviceLocalAccountExtensionDir[] =
    FILE_PATH_LITERAL("/var/cache/device_local_account_extensions");

const base::FilePath::CharType kDeviceLocalAccountExternalDataDir[] =
    FILE_PATH_LITERAL("/var/cache/device_local_account_external_policy_data");

const base::FilePath::CharType kDeviceLocalAccountComponentPolicy[] =
    FILE_PATH_LITERAL("/var/cache/device_local_account_component_policy");

const base::FilePath::CharType kDeviceDisplayProfileDirectory[] =
    FILE_PATH_LITERAL("/var/cache/display_profiles");

const base::FilePath::CharType kDeviceExtensionLocalCache[] =
    FILE_PATH_LITERAL("/var/cache/external_cache");

const base::FilePath::CharType kSigninProfileComponentPolicy[] =
    FILE_PATH_LITERAL("/var/cache/signin_profile_component_policy");

const base::FilePath::CharType kPreinstalledComponents[] =
    FILE_PATH_LITERAL("/mnt/stateful_partition/unencrypted/");

bool PathProvider(int key, base::FilePath* result) {
  switch (key) {
    case FILE_DEFAULT_APP_ORDER:
      *result = base::FilePath(kDefaultAppOrderFileName);
      break;
    case DIR_USER_POLICY_KEYS:
      *result = base::FilePath(kDefaultUserPolicyKeysDir);
      break;
    case FILE_OWNER_KEY:
      *result = base::FilePath(kOwnerKeyFileName);
      break;
    case FILE_INSTALL_ATTRIBUTES:
      *result = base::FilePath(kInstallAttributesFileName);
      break;
    case FILE_MACHINE_INFO:
      *result = base::FilePath(kMachineHardwareInfoFileName);
      break;
    case FILE_VPD:
      *result = base::FilePath(kVpdFileName);
      break;
    case FILE_UPTIME:
      *result = base::FilePath(kUptimeFileName);
      break;
    case FILE_UPDATE_REBOOT_NEEDED_UPTIME:
      *result = base::FilePath(kUpdateRebootNeededUptimeFile);
      break;
    case DIR_DEVICE_LOCAL_ACCOUNT_EXTENSIONS:
      *result = base::FilePath(kDeviceLocalAccountExtensionDir);
      break;
    case DIR_DEVICE_LOCAL_ACCOUNT_EXTERNAL_DATA:
      *result = base::FilePath(kDeviceLocalAccountExternalDataDir);
      break;
    case DIR_DEVICE_LOCAL_ACCOUNT_COMPONENT_POLICY:
      *result = base::FilePath(kDeviceLocalAccountComponentPolicy);
      break;
    case DIR_DEVICE_DISPLAY_PROFILES:
      *result = base::FilePath(kDeviceDisplayProfileDirectory);
      break;
    case DIR_DEVICE_EXTENSION_LOCAL_CACHE:
      *result = base::FilePath(kDeviceExtensionLocalCache);
      break;
    case DIR_SIGNIN_PROFILE_COMPONENT_POLICY:
      *result = base::FilePath(kSigninProfileComponentPolicy);
      break;
    case DIR_PREINSTALLED_COMPONENTS:
      *result = base::FilePath(kPreinstalledComponents);
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace

void RegisterPathProvider() {
  base::PathService::RegisterProvider(PathProvider, PATH_START, PATH_END);
}

void RegisterStubPathOverrides(const base::FilePath& stubs_dir) {
  CHECK(!base::SysInfo::IsRunningOnChromeOS());
  // Override these paths on the desktop, so that enrollment and cloud policy
  // work and can be tested.
  base::FilePath parent = base::MakeAbsoluteFilePath(stubs_dir);
  base::PathService::Override(DIR_USER_POLICY_KEYS,
                              parent.AppendASCII("stub_user_policy"));
  const bool is_absolute = true;
  const bool create = false;
  base::PathService::OverrideAndCreateIfNeeded(
      FILE_OWNER_KEY, parent.AppendASCII("stub_owner.key"), is_absolute,
      create);
  base::PathService::OverrideAndCreateIfNeeded(
      FILE_INSTALL_ATTRIBUTES, parent.AppendASCII("stub_install_attributes.pb"),
      is_absolute, create);
  base::PathService::OverrideAndCreateIfNeeded(
      FILE_MACHINE_INFO, parent.AppendASCII("stub_machine-info"), is_absolute,
      create);
  base::PathService::OverrideAndCreateIfNeeded(
      FILE_VPD, parent.AppendASCII("stub_vpd"), is_absolute, create);
  base::PathService::Override(
      DIR_DEVICE_LOCAL_ACCOUNT_EXTENSIONS,
      parent.AppendASCII("stub_device_local_account_extensions"));
  base::PathService::Override(
      DIR_DEVICE_LOCAL_ACCOUNT_EXTERNAL_DATA,
      parent.AppendASCII("stub_device_local_account_external_data"));
  base::PathService::Override(
      DIR_DEVICE_LOCAL_ACCOUNT_COMPONENT_POLICY,
      parent.AppendASCII("stub_device_local_account_component_policy"));
  base::PathService::Override(
      DIR_DEVICE_EXTENSION_LOCAL_CACHE,
      parent.AppendASCII("stub_device_local_extension_cache"));
  base::PathService::Override(
      DIR_SIGNIN_PROFILE_COMPONENT_POLICY,
      parent.AppendASCII("stub_signin_profile_component_policy"));
}

}  // namespace chromeos
