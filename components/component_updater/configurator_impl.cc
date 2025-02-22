// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/component_updater/configurator_impl.h"

#include <stddef.h>

#include <algorithm>

#include "base/stl_util.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/version.h"
#include "build/build_config.h"
#include "components/component_updater/component_updater_switches.h"
#include "components/component_updater/component_updater_url_constants.h"
#include "components/update_client/command_line_config_policy.h"
#include "components/update_client/protocol_handler.h"
#include "components/update_client/utils.h"
#include "components/version_info/version_info.h"

#if defined(OS_WIN)
#include "base/win/win_util.h"
#endif  // OS_WIN

namespace component_updater {

namespace {

// Default time constants.
const int kDelayOneMinute = 60;
const int kDelayOneHour = kDelayOneMinute * 60;

}  // namespace

ConfiguratorImpl::ConfiguratorImpl(
    const update_client::CommandLineConfigPolicy& config_policy,
    bool require_encryption)
    : background_downloads_enabled_(config_policy.BackgroundDownloadsEnabled()),
      deltas_enabled_(config_policy.DeltaUpdatesEnabled()),
      fast_update_(config_policy.FastUpdate()),
      pings_enabled_(config_policy.PingsEnabled()),
      require_encryption_(require_encryption),
      url_source_override_(config_policy.UrlSourceOverride()),
      initial_delay_(config_policy.InitialDelay()) {
  if (config_policy.TestRequest())
    extra_info_["testrequest"] = "1";
}

ConfiguratorImpl::~ConfiguratorImpl() {}

int ConfiguratorImpl::InitialDelay() const {
  if (initial_delay_)
    return initial_delay_;
  return fast_update_ ? 10 : (6 * kDelayOneMinute);
}

int ConfiguratorImpl::NextCheckDelay() const {
  return 5 * kDelayOneHour;
}

int ConfiguratorImpl::OnDemandDelay() const {
  return fast_update_ ? 2 : (30 * kDelayOneMinute);
}

int ConfiguratorImpl::UpdateDelay() const {
  return fast_update_ ? 10 : (15 * kDelayOneMinute);
}

std::vector<GURL> ConfiguratorImpl::UpdateUrl() const {
  if (url_source_override_.is_valid()) {
    return {GURL(url_source_override_)};
  }

  std::vector<GURL> urls{GURL(kUpdaterDefaultUrl), GURL(kUpdaterFallbackUrl)};
  if (require_encryption_)
    update_client::RemoveUnsecureUrls(&urls);

  return urls;
}

std::vector<GURL> ConfiguratorImpl::PingUrl() const {
  return pings_enabled_ ? UpdateUrl() : std::vector<GURL>();
}

const base::Version& ConfiguratorImpl::GetBrowserVersion() const {
  return version_info::GetVersion();
}

std::string ConfiguratorImpl::GetOSLongName() const {
  return version_info::GetOSType();
}

base::flat_map<std::string, std::string> ConfiguratorImpl::ExtraRequestParams()
    const {
  return extra_info_;
}

std::string ConfiguratorImpl::GetDownloadPreference() const {
  return std::string();
}

bool ConfiguratorImpl::EnabledDeltas() const {
  return deltas_enabled_;
}

bool ConfiguratorImpl::EnabledComponentUpdates() const {
  return true;
}

bool ConfiguratorImpl::EnabledBackgroundDownloader() const {
  return background_downloads_enabled_;
}

bool ConfiguratorImpl::EnabledCupSigning() const {
  return true;
}

std::vector<uint8_t> ConfiguratorImpl::GetRunActionKeyHash() const {
  return std::vector<uint8_t>{0x5f, 0x94, 0xe0, 0x3c, 0x64, 0x30, 0x9f, 0xbc,
                              0xfe, 0x00, 0x9a, 0x27, 0x3e, 0x52, 0xbf, 0xa5,
                              0x84, 0xb9, 0xb3, 0x75, 0x07, 0x29, 0xde, 0xfa,
                              0x32, 0x76, 0xd9, 0x93, 0xb5, 0xa3, 0xce, 0x02};
}

// The default implementation for most embedders returns an empty string.
// Desktop embedders, such as the Windows component updater can provide a
// meaningful implementation for this function.
std::string ConfiguratorImpl::GetAppGuid() const {
  return {};
}

std::unique_ptr<update_client::ProtocolHandlerFactory>
ConfiguratorImpl::GetProtocolHandlerFactory() const {
  return std::make_unique<update_client::ProtocolHandlerFactoryXml>();
}

}  // namespace component_updater
