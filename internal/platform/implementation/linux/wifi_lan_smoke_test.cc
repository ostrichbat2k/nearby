// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Manual smoke test (not a unit test -- it talks to the real system Avahi
// daemon, and to a real external mDNS announcer on the LAN for the
// discovery half) for the Avahi-backed WifiLanMedium. Run with `bazel run`.
// Not wired into any test suite; exercised ad hoc while standing up the
// backend.
//
// Part 1 (discovery) targets "_foldersync._tcp", a real service type a
// companion Android app on this network happens to advertise (confirmed via
// `avahi-browse -r -t _foldersync._tcp` before writing this test) -- this
// exercises ItemNew/ResolveService/NsdServiceInfo population against a
// genuine external device, not a loopback.
//
// Part 2 (advertise + data channel) starts our own mDNS advertisement +
// listening socket, verifies externally via `avahi-browse` that Avahi is
// really broadcasting it with the right port/txt, then validates the actual
// data channel with a real loopback TCP connect+accept+read+write (this is
// not a false-positive self-connect like BLE GATT's self-advertise/self-scan
// case was -- a loopback TCP connection genuinely exercises the same
// accept()/connect()/send()/recv() syscalls a remote peer would hit).

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "internal/platform/byte_array.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/linux/wifi_lan.h"
#include "internal/platform/nsd_service_info.h"
#include "internal/platform/service_address.h"

int main() {
  nearby::linux_platform::WifiLanMedium medium;

  printf("IsNetworkConnected: %d\n", medium.IsNetworkConnected());

  // --- Part 1: discovery against a real external device ---
  printf("\n--- Discovery: _foldersync._tcp. ---\n");
  nearby::api::WifiLanMedium::DiscoveredServiceCallback callback;
  callback.service_discovered_cb = [](const nearby::NsdServiceInfo& info) {
    printf("Discovered: name=%s type=%s port=%d ipv6=%s txt(name)=%s\n",
           info.GetServiceName().c_str(), info.GetServiceType().c_str(),
           info.GetPort(), info.GetIPv6Address().c_str(),
           info.GetTxtRecord("name").c_str());
  };
  callback.service_lost_cb = [](const nearby::NsdServiceInfo& info) {
    printf("Lost: name=%s\n", info.GetServiceName().c_str());
  };

  bool started = medium.StartDiscovery("_foldersync._tcp.", std::move(callback));
  printf("StartDiscovery: %d\n", started);
  std::this_thread::sleep_for(std::chrono::seconds(8));
  printf("StopDiscovery: %d\n", medium.StopDiscovery("_foldersync._tcp."));

  // --- Part 2: advertise + real loopback data channel ---
  printf("\n--- Advertise + loopback socket ---\n");
  auto server_socket = medium.ListenForService(0);
  if (server_socket == nullptr) {
    printf("ListenForService failed\n");
    return 1;
  }
  printf("Listening on port %d\n", server_socket->GetPort());

  nearby::NsdServiceInfo nsd_service_info;
  nsd_service_info.SetServiceName("linux-smoke-test");
  nsd_service_info.SetServiceType("_LINUXSMOKE._tcp.");
  nsd_service_info.SetPort(server_socket->GetPort());
  nsd_service_info.SetTxtRecord("n", "smoketest");
  bool advertising = medium.StartAdvertising(nsd_service_info);
  printf("StartAdvertising: %d\n", advertising);

  if (advertising) {
    printf("Verifying externally via avahi-browse (should show our record "
           "with port %d)...\n",
           server_socket->GetPort());
    std::string cmd =
        "avahi-browse -r -t _LINUXSMOKE._tcp 2>&1 | sed 's/^/  [avahi-browse] /'";
    system(cmd.c_str());
  }

  std::thread server_thread([&server_socket]() {
    auto client = server_socket->Accept();
    if (client == nullptr) {
      printf("[server] Accept returned null\n");
      return;
    }
    printf("[server] Accepted a connection\n");
    auto result = client->GetInputStream().Read(64);
    if (result.ok()) {
      std::string received(result.GetResult().data(),
                           result.GetResult().size());
      printf("[server] Read %zu bytes: %s\n", received.size(),
             received.c_str());
      client->GetOutputStream().Write("pong");
    } else {
      printf("[server] Read failed\n");
    }
    client->Close();
  });

  nearby::ServiceAddress loopback_address;
  loopback_address.address = {127, 0, 0, 1};
  loopback_address.port = static_cast<uint16_t>(server_socket->GetPort());
  nearby::CancellationFlag cancellation_flag;
  auto client_socket =
      medium.ConnectToService(loopback_address, &cancellation_flag);
  if (client_socket == nullptr) {
    printf("[client] ConnectToService failed\n");
  } else {
    printf("[client] Connected\n");
    client_socket->GetOutputStream().Write("ping");
    auto result = client_socket->GetInputStream().Read(64);
    if (result.ok()) {
      std::string received(result.GetResult().data(),
                           result.GetResult().size());
      printf("[client] Read %zu bytes: %s\n", received.size(),
             received.c_str());
    } else {
      printf("[client] Read failed\n");
    }
    client_socket->Close();
  }

  server_thread.join();
  printf("StopAdvertising: %d\n", medium.StopAdvertising(nsd_service_info));
  server_socket->Close();

  return 0;
}
