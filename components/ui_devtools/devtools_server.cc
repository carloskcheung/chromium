// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/ui_devtools/devtools_server.h"

#include <memory>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/format_macros.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/user_metrics.h"
#include "base/metrics/user_metrics_action.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "net/base/net_errors.h"
#include "net/log/net_log.h"
#include "services/network/public/cpp/server/http_server_request_info.h"
#include "services/network/public/mojom/tcp_socket.mojom.h"

namespace ui_devtools {

namespace {
const char kChromeDeveloperToolsPrefix[] =
    "chrome-devtools://devtools/bundled/devtools_app.html?ws=";

bool IsDevToolsEnabled(const char* enable_devtools_flag) {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      enable_devtools_flag);
}

}  // namespace

UiDevToolsServer* UiDevToolsServer::devtools_server_ = nullptr;

const net::NetworkTrafficAnnotationTag UiDevToolsServer::kUIDevtoolsServerTag =
    net::DefineNetworkTrafficAnnotation("ui_devtools_server", R"(
      semantics {
        sender: "UI Devtools Server"
        description:
          "Backend for UI DevTools, to inspect Aura/Views UI."
        trigger:
          "Run with '--enable-ui-devtools' switch."
        data: "Debugging data, including any data on the open pages."
        destination: OTHER
        destination_other: "The data can be sent to any destination."
      }
      policy {
        cookies_allowed: NO
        setting:
          "This request cannot be disabled in settings. However it will never "
          "be made if user does not run with '--enable-ui-devtools' switch."
        policy_exception_justification:
          "Not implemented, only used in Devtools and is behind a switch."
      })");

const net::NetworkTrafficAnnotationTag UiDevToolsServer::kVizDevtoolsServerTag =
    net::DefineNetworkTrafficAnnotation("viz_devtools_server", R"(
      semantics {
        sender: "Viz Devtools Server"
        description:
          "Backend for Viz DevTools, to inspect FrameSink hierarchies."
        trigger:
          "Run with '--enable-viz-devtools' switch."
        data: "Debugging data, including any data on the active frame sinks."
        destination: OTHER
        destination_other: "The data can be sent to any destination."
      }
      policy {
        cookies_allowed: NO
        setting:
          "This request cannot be disabled in settings. However it will never "
          "be made if user does not run with '--enable-viz-devtools' switch."
        policy_exception_justification:
          "Not implemented, only used in Devtools and is behind a switch."
      })");

UiDevToolsServer::UiDevToolsServer(int port,
                                   net::NetworkTrafficAnnotationTag tag)
    : port_(port), tag_(tag), weak_ptr_factory_(this) {
  DCHECK(!devtools_server_);
  devtools_server_ = this;
}

UiDevToolsServer::~UiDevToolsServer() {
  devtools_server_ = nullptr;
}

// static
std::unique_ptr<UiDevToolsServer> UiDevToolsServer::CreateForViews(
    network::mojom::NetworkContext* network_context,
    const char* enable_devtools_flag,
    int default_port) {
  std::unique_ptr<UiDevToolsServer> server;
  if (IsDevToolsEnabled(enable_devtools_flag) && !devtools_server_) {
    // TODO(mhashmi): Change port if more than one inspectable clients
    int port = GetUiDevToolsPort(enable_devtools_flag, default_port);
    server = base::WrapUnique(new UiDevToolsServer(port, kUIDevtoolsServerTag));
    network::mojom::TCPServerSocketPtr server_socket;
    CreateTCPServerSocket(mojo::MakeRequest(&server_socket), network_context,
                          port, kUIDevtoolsServerTag,
                          base::BindOnce(&UiDevToolsServer::MakeServer,
                                         server->weak_ptr_factory_.GetWeakPtr(),
                                         std::move(server_socket)));
  }
  return server;
}

// static
std::unique_ptr<UiDevToolsServer> UiDevToolsServer::CreateForViz(
    network::mojom::TCPServerSocketPtr server_socket,
    int port) {
  auto server =
      base::WrapUnique(new UiDevToolsServer(port, kVizDevtoolsServerTag));
  server->MakeServer(std::move(server_socket), net::OK, base::nullopt);
  return server;
}

// static
void UiDevToolsServer::CreateTCPServerSocket(
    network::mojom::TCPServerSocketRequest server_socket_request,
    network::mojom::NetworkContext* network_context,
    int port,
    net::NetworkTrafficAnnotationTag tag,
    network::mojom::NetworkContext::CreateTCPServerSocketCallback callback) {
  // Create the socket using the address 0.0.0.0 to listen on all interfaces.
  net::IPAddress address(0, 0, 0, 0);
  constexpr int kBacklog = 1;
  network_context->CreateTCPServerSocket(
      net::IPEndPoint(address, port), kBacklog,
      net::MutableNetworkTrafficAnnotationTag(tag),
      std::move(server_socket_request), std::move(callback));
}

// static
std::vector<UiDevToolsServer::NameUrlPair>
UiDevToolsServer::GetClientNamesAndUrls() {
  std::vector<NameUrlPair> pairs;
  if (!devtools_server_)
    return pairs;

  for (ClientsList::size_type i = 0; i != devtools_server_->clients_.size();
       i++) {
    pairs.push_back(std::pair<std::string, std::string>(
        devtools_server_->clients_[i]->name(),
        base::StringPrintf("%s127.0.0.1:%d/%" PRIuS,
                           kChromeDeveloperToolsPrefix,
                           devtools_server_->port(), i)));
  }
  return pairs;
}

// static
int UiDevToolsServer::GetUiDevToolsPort(const char* enable_devtools_flag,
                                        int default_port) {
  DCHECK(IsDevToolsEnabled(enable_devtools_flag));
  std::string switch_value =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          enable_devtools_flag);
  int port;
  if (!base::StringToInt(switch_value, &port))
    return default_port;
  return port;
}

void UiDevToolsServer::AttachClient(std::unique_ptr<UiDevToolsClient> client) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(devtools_server_sequence_);
  clients_.push_back(std::move(client));
}

void UiDevToolsServer::SendOverWebSocket(int connection_id,
                                         const String& message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(devtools_server_sequence_);
  server_->SendOverWebSocket(connection_id, message, tag_);
}

void UiDevToolsServer::MakeServer(
    network::mojom::TCPServerSocketPtr server_socket,
    int result,
    const base::Optional<net::IPEndPoint>& local_addr) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(devtools_server_sequence_);
  if (result == net::OK) {
    server_ = std::make_unique<network::server::HttpServer>(
        std::move(server_socket), this);
  }
}

// HttpServer::Delegate Implementation
void UiDevToolsServer::OnConnect(int connection_id) {
  base::RecordAction(base::UserMetricsAction("UI_DevTools_Connect"));
}

void UiDevToolsServer::OnHttpRequest(
    int connection_id,
    const network::server::HttpServerRequestInfo& info) {
  NOTIMPLEMENTED();
}

void UiDevToolsServer::OnWebSocketRequest(
    int connection_id,
    const network::server::HttpServerRequestInfo& info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(devtools_server_sequence_);
  size_t target_id = 0;
  if (info.path.empty() ||
      !base::StringToSizeT(info.path.substr(1), &target_id) ||
      target_id > clients_.size())
    return;

  UiDevToolsClient* client = clients_[target_id].get();
  // Only one user can inspect the client at a time
  if (client->connected())
    return;
  client->set_connection_id(connection_id);
  connections_[connection_id] = client;
  server_->AcceptWebSocket(connection_id, info, tag_);
}

void UiDevToolsServer::OnWebSocketMessage(int connection_id,
                                          const std::string& data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(devtools_server_sequence_);
  auto it = connections_.find(connection_id);
  DCHECK(it != connections_.end());
  UiDevToolsClient* client = it->second;
  client->Dispatch(data);
}

void UiDevToolsServer::OnClose(int connection_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(devtools_server_sequence_);
  auto it = connections_.find(connection_id);
  if (it == connections_.end())
    return;
  UiDevToolsClient* client = it->second;
  client->Disconnect();
  connections_.erase(it);
}

}  // namespace ui_devtools
