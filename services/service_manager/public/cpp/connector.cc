// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/service_manager/public/cpp/connector.h"

#include "services/service_manager/public/cpp/identity.h"

namespace service_manager {

Connector::Connector(mojom::ConnectorPtrInfo unbound_state)
    : unbound_state_(std::move(unbound_state)), weak_factory_(this) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

Connector::Connector(mojom::ConnectorPtr connector)
    : connector_(std::move(connector)), weak_factory_(this) {
  connector_.set_connection_error_handler(
      base::Bind(&Connector::OnConnectionError, base::Unretained(this)));
}

Connector::~Connector() = default;

std::unique_ptr<Connector> Connector::Create(mojom::ConnectorRequest* request) {
  mojom::ConnectorPtr proxy;
  *request = mojo::MakeRequest(&proxy);
  return std::make_unique<Connector>(proxy.PassInterface());
}

void Connector::WarmService(const ServiceFilter& filter,
                            WarmServiceCallback callback) {
  if (!BindConnectorIfNecessary())
    return;
  connector_->WarmService(filter, std::move(callback));
}

void Connector::RegisterServiceInstance(
    const Identity& identity,
    mojom::ServicePtr service,
    mojom::PIDReceiverRequest pid_receiver_request,
    RegisterServiceInstanceCallback callback) {
  if (!BindConnectorIfNecessary())
    return;

  DCHECK(identity.instance_group() && !identity.instance_group()->is_zero());
  DCHECK(identity.instance_id());
  DCHECK(identity.globally_unique_id() &&
         !identity.globally_unique_id()->is_zero());
  DCHECK(service.is_bound() && pid_receiver_request.is_pending());
  connector_->RegisterServiceInstance(
      identity, service.PassInterface().PassHandle(),
      std::move(pid_receiver_request), std::move(callback));
}

void Connector::QueryService(const std::string& service_name,
                             mojom::Connector::QueryServiceCallback callback) {
  if (!BindConnectorIfNecessary())
    return;

  connector_->QueryService(service_name, std::move(callback));
}

void Connector::BindInterface(const ServiceFilter& filter,
                              const std::string& interface_name,
                              mojo::ScopedMessagePipeHandle interface_pipe,
                              BindInterfaceCallback callback) {
  auto service_overrides_iter = local_binder_overrides_.find(filter);
  if (service_overrides_iter != local_binder_overrides_.end()) {
    auto override_iter = service_overrides_iter->second.find(interface_name);
    if (override_iter != service_overrides_iter->second.end()) {
      override_iter->second.Run(std::move(interface_pipe));
      return;
    }
  }

  if (!BindConnectorIfNecessary())
    return;

  connector_->BindInterface(filter, interface_name, std::move(interface_pipe),
                            std::move(callback));
}

std::unique_ptr<Connector> Connector::Clone() {
  mojom::ConnectorPtrInfo connector;
  auto request = mojo::MakeRequest(&connector);
  if (BindConnectorIfNecessary())
    connector_->Clone(std::move(request));
  return std::make_unique<Connector>(std::move(connector));
}

bool Connector::IsBound() const {
  return connector_.is_bound();
}

void Connector::FilterInterfaces(const std::string& spec,
                                 const Identity& source_identity,
                                 mojom::InterfaceProviderRequest request,
                                 mojom::InterfaceProviderPtr target) {
  if (!BindConnectorIfNecessary())
    return;
  connector_->FilterInterfaces(spec, source_identity, std::move(request),
                               std::move(target));
}

void Connector::BindConnectorRequest(mojom::ConnectorRequest request) {
  if (!BindConnectorIfNecessary())
    return;
  connector_->Clone(std::move(request));
}

base::WeakPtr<Connector> Connector::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void Connector::OverrideBinderForTesting(
    const service_manager::ServiceFilter& filter,
    const std::string& interface_name,
    const TestApi::Binder& binder) {
  local_binder_overrides_[filter][interface_name] = binder;
}

bool Connector::HasBinderOverrideForTesting(
    const service_manager::ServiceFilter& filter,
    const std::string& interface_name) {
  auto service_overrides = local_binder_overrides_.find(filter);
  if (service_overrides == local_binder_overrides_.end())
    return false;

  return base::ContainsKey(service_overrides->second, interface_name);
}

void Connector::ClearBinderOverrideForTesting(
    const service_manager::ServiceFilter& filter,
    const std::string& interface_name) {
  auto service_overrides = local_binder_overrides_.find(filter);
  if (service_overrides == local_binder_overrides_.end())
    return;

  service_overrides->second.erase(interface_name);
}

void Connector::ClearBinderOverridesForTesting() {
  local_binder_overrides_.clear();
}

void Connector::OnConnectionError() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  connector_.reset();
}

bool Connector::BindConnectorIfNecessary() {
  // Bind the message pipe and SequenceChecker to the current thread the first
  // time it is used to connect.
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connector_.is_bound()) {
    if (!unbound_state_.is_valid()) {
      // It's possible to get here when the link to the service manager has been
      // severed (and so the connector pipe has been closed) but the app has
      // chosen not to quit.
      return false;
    }

    connector_.Bind(std::move(unbound_state_));
    connector_.set_connection_error_handler(
        base::Bind(&Connector::OnConnectionError, base::Unretained(this)));
  }

  return true;
}

}  // namespace service_manager
