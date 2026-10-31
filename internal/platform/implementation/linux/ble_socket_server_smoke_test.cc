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
// Manual smoke test (not a unit test -- it talks to the real system BlueZ
// daemon) for the Weave-based BleSocket *server* role (BleMedium::
// OpenServerSocket() + BleServerSocket::Accept()).
//
// Starts the Weave GATT server + Copresence advertisement and waits for a
// real external BLE central (e.g. a generic BLE scanner/debug app on a
// phone) to connect, manually write the Weave connection-request bytes to
// the ToPeripheralChar characteristic, and subscribe to FromPeripheralChar
// for the reply.
//
// A same-machine loopback (a second BleMedium on this process scanning for
// and connecting to this adapter's own advertisement) was tried first and
// found unreliable on this hardware: the "client" side reported a
// successful handshake while BleServerSocket::OnCharacteristicWrite() never
// actually ran (confirmed via tracing) -- self-connecting to one's own
// advertised address doesn't appear to route through the real GATT server
// dispatch the way a connection from a genuinely separate device does. So
// this test only exercises the real external-central path.

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "internal/platform/byte_array.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/linux/ble.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/input_stream.h"
#include "internal/platform/output_stream.h"
#include "internal/platform/uuid.h"

int main() {
  nearby::linux_platform::BluetoothAdapter adapter;
  printf("Adapter enabled: %d\n", adapter.IsEnabled());
  printf("Adapter address: %s\n", adapter.GetMacAddress().ToString().c_str());

  nearby::Uuid copresence_service_uuid =
      *nearby::Uuid::FromString("0000fef3-0000-1000-8000-00805f9b34fb");

  nearby::linux_platform::BleMedium server_medium;
  std::unique_ptr<nearby::api::ble::BleServerSocket> server_socket =
      server_medium.OpenServerSocket("quickshare-linux-server-smoke-test");
  if (server_socket == nullptr) {
    printf("OpenServerSocket returned nullptr\n");
    return 1;
  }
  printf("OpenServerSocket succeeded (Weave GATT server + characteristics "
        "registered)\n");

  nearby::api::ble::BleAdvertisementData advertising_data;
  advertising_data.is_extended_advertisement = false;
  advertising_data.service_data[copresence_service_uuid] =
      nearby::ByteArray(std::string("\x11\x01hi", 4));
  nearby::api::ble::AdvertiseParameters params;
  params.tx_power_level = nearby::api::ble::TxPowerLevel::kHigh;
  params.is_connectable = true;
  bool advertising = server_medium.StartAdvertising(advertising_data, params);
  printf("StartAdvertising: %d\n", advertising);

  bool accepted = false;
  std::thread accept_thread([&server_socket, &accepted]() {
    std::unique_ptr<nearby::api::ble::BleSocket> socket =
        server_socket->Accept();
    if (socket == nullptr) {
      printf("Accept() returned nullptr (server socket closed before any "
            "connection completed)\n");
      return;
    }
    accepted = true;
    printf("Accept() succeeded! Weave handshake completed with remote "
          "peripheral id=%llu\n",
          static_cast<unsigned long long>(socket->GetRemotePeripheralId()));

    std::string reply = "hello back from linux gatt server";
    nearby::Exception write_result = socket->GetOutputStream().Write(reply);
    printf("Reply write(%zu bytes): exception=%d (watch for a notification "
          "in your BLE app)\n",
          reply.size(), write_result.value);

    // Read() has no built-in timeout and the remote end (a generic BLE
    // debug app) has no reason to spontaneously send anything, so bound
    // the wait the same way the client-role smoke test does: run it on a
    // background thread and enforce the deadline by closing the socket
    // (unblocking it with EOF) from this thread; join before returning so
    // there's no race with the socket's destruction.
    printf("Waiting up to 5s for any further data...\n");
    nearby::api::ble::BleSocket* socket_ptr = socket.get();
    std::thread reader([socket_ptr]() {
      nearby::ExceptionOr<nearby::ByteArray> read_result =
          socket_ptr->GetInputStream().Read(512);
      if (read_result.ok()) {
        printf("Read %zu bytes: \"%s\"\n", read_result.result().size(),
               std::string(read_result.result()).c_str());
      } else {
        printf("Read failed/EOF: exception=%d\n",
               static_cast<int>(read_result.exception()));
      }
    });
    std::this_thread::sleep_for(std::chrono::seconds(5));
    socket->Close();
    reader.join();
    printf("Closed.\n");
  });

  // A same-machine loopback was tried here previously and found unreliable
  // on this hardware: a loopback "central" can report a successful Weave
  // handshake while BleServerSocket::OnCharacteristicWrite() never actually
  // runs (confirmed via tracing) -- i.e. self-connecting to one's own
  // advertised address doesn't route through the real GATT server dispatch
  // the way connecting from a genuinely separate device does. So this test
  // only waits for a real external BLE central now.
  printf("Waiting up to 180s for an external BLE central (e.g. nRF Connect "
        "on a phone) to connect to this machine (address %s) and write the "
        "Weave connection-request bytes to characteristic "
        "00000100-0004-1000-8000-001a11000101 under service "
        "0000fef3-0000-1000-8000-00805f9b34fb, then subscribe to "
        "00000100-0004-1000-8000-001a11000102 for the reply...\n",
        adapter.GetMacAddress().ToString().c_str());
  for (int i = 0; i < 180 && !accepted; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  server_medium.StopAdvertising();
  server_socket->Close();
  accept_thread.join();
  printf("Done.\n");
  return 0;
}
