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
// The non-L2CAP BLE socket (BleSocket/BleServerSocket): a message-oriented
// virtual socket emulated over two GATT characteristics, used as a Nearby
// Connections data channel when nothing better (WiFi LAN/Hotspot,
// Bluetooth Classic) is available.
//
// This is built on //internal/weave, a from-scratch C++ port of Google's
// "Weave" BLE GATT transport (go/weave-ble-gatt-transport, see
// internal/weave/packet.h) that was sitting in this OSS tree unused by any
// platform backend. The GATT service/characteristic UUIDs below are not an
// invention of this backend -- they are copied from the real client-side
// constants in the Apple backend's
// Mediums/BLE/Sockets/Source/Shared/GNSUtils.m
// (kGNSWeaveToPeripheralCharUUIDString/kGNSWeaveFromPeripheralCharUUIDString)
// and ble_medium.mm (kWeaveServiceUUID = "FEF3"), i.e. this is the actual
// wire protocol real devices (including real Android phones) speak for
// this feature.
//
// Not yet verified against a real phone -- see the Linux platform memory
// notes for the project's hardware-verification workflow.

#ifndef PLATFORM_IMPL_LINUX_BLE_SOCKET_H_
#define PLATFORM_IMPL_LINUX_BLE_SOCKET_H_

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "absl/strings/string_view.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/input_stream.h"
#include "internal/platform/output_stream.h"
#include "internal/platform/uuid.h"
#include "internal/weave/connection.h"
#include "internal/weave/socket_callback.h"
#include "internal/weave/sockets/server_socket.h"

namespace nearby {
namespace linux_platform {

// The GATT service hosting the Weave characteristics -- this is the same
// Copresence service UUID used for BLE advertising/the advertisement-read
// GATT characteristic (see ble.cc, ble_gatt_server_smoke_test.cc).
const Uuid& WeaveServiceUuid();
// Central -> peripheral direction (we write to this when we're the GATT
// client; remote centrals write to it when we're the GATT server).
const Uuid& WeaveToPeripheralCharacteristicUuid();
// Peripheral -> central direction (we notify on this when we're the GATT
// server; we subscribe to it when we're the GATT client).
const Uuid& WeaveFromPeripheralCharacteristicUuid();

// The minimum guaranteed-safe BLE ATT MTU-derived packet size (the default
// 23-byte ATT_MTU minus its 3-byte header), matching the real Apple
// backend's own kGNSMinSupportedPacketSize. BlueZ's D-Bus GATT API doesn't
// give us an easy way to read back the real negotiated per-connection MTU,
// so we always offer this conservative floor; Weave's own handshake picks
// min(client, server) anyway, so this only costs some throughput, not
// correctness.
inline constexpr int kMinSupportedPacketSize = 20;

// weave::Connection adapter for the GATT *client* (central) role: used when
// we connect out to a remote peripheral's GATT server (BleMedium::Connect).
class GattClientWeaveConnection : public weave::Connection {
 public:
  GattClientWeaveConnection() = default;

  // Must be called exactly once, after GATT service/characteristic
  // discovery succeeds and before the weave socket built on top of this
  // connection is told to Connect(). Takes ownership of `gatt_client` so it
  // stays alive for as long as this connection does.
  void SetGattClient(std::unique_ptr<api::ble::GattClient> gatt_client,
                      api::ble::GattCharacteristic write_characteristic,
                      api::ble::GattCharacteristic notify_characteristic);

  void Initialize(weave::ConnectionCallback callback) override;
  int GetMaxPacketSize() const override { return kMinSupportedPacketSize; }
  void Transmit(std::string packet) override;
  void Close() override;

  // Bridged from ClientGattConnectionCallback.
  void OnNotifyReceived(absl::string_view value);
  void OnGattDisconnected();

 private:
  std::unique_ptr<api::ble::GattClient> gatt_client_;
  api::ble::GattCharacteristic write_characteristic_;
  api::ble::GattCharacteristic notify_characteristic_;
  weave::ConnectionCallback callback_;
};

// weave::Connection adapter for the GATT *server* (peripheral) role: used
// for a remote central that has connected to our own advertised GATT
// server.
//
// Limitation: BlueZ's GATT server broadcasts a characteristic notification
// to every currently-subscribed remote device -- there's no API to address
// one specific central -- so only one Weave connection can be active on a
// given BleServerSocket at a time. This mirrors how the rest of this GATT
// server was scoped (see ble_gatt_server.h): correct for the one real
// caller (one active Nearby Connections BLE link at a time), not a general
// multi-peer GATT server.
class GattServerWeaveConnection : public weave::Connection {
 public:
  GattServerWeaveConnection(api::ble::GattServer* server,
                            api::ble::GattCharacteristic notify_characteristic,
                            absl::AnyInvocable<void()> on_closed)
      : server_(server),
        notify_characteristic_(notify_characteristic),
        on_closed_(std::move(on_closed)) {}

  void Initialize(weave::ConnectionCallback callback) override;
  int GetMaxPacketSize() const override { return kMinSupportedPacketSize; }
  void Transmit(std::string packet) override;
  // Does not stop the underlying GATT server (that's owned by the
  // BleServerSocket, shared across connections) -- only frees up the
  // "one active connection" slot so a new central can connect.
  void Close() override;

  // Bridged from ServerGattConnectionCallback::on_characteristic_write_cb.
  void OnRemoteTransmit(absl::string_view data);
  void OnGattDisconnected();

 private:
  api::ble::GattServer* server_;
  api::ble::GattCharacteristic notify_characteristic_;
  absl::AnyInvocable<void()> on_closed_;
  weave::ConnectionCallback callback_;
};

// api::ble::BleSocket implementation shared by both roles: wraps a
// weave::BaseSocket (ClientSocket or ServerSocket) plus the Connection
// adapter it was built on, exposing blocking InputStream/OutputStream on
// top of Weave's async Write()-returns-a-Future / on_receive_cb contract.
class BleSocket : public api::ble::BleSocket {
 public:
  BleSocket(std::unique_ptr<weave::Connection> connection,
            std::unique_ptr<weave::BaseSocket> weave_socket,
            api::ble::BlePeripheral::UniqueId remote_peripheral_id,
            std::unique_ptr<InputStream> pipe_input);
  ~BleSocket() override;

  InputStream& GetInputStream() override { return *pipe_input_; }
  OutputStream& GetOutputStream() override { return output_stream_; }
  Exception Close() override;
  api::ble::BlePeripheral::UniqueId GetRemotePeripheralId() override {
    return remote_peripheral_id_;
  }

 private:
  class WeaveOutputStream : public OutputStream {
   public:
    explicit WeaveOutputStream(weave::BaseSocket& socket) : socket_(socket) {}
    Exception Write(absl::string_view data) override;
    Exception Flush() override { return {Exception::kSuccess}; }
    Exception Close() override { return {Exception::kSuccess}; }

   private:
    weave::BaseSocket& socket_;
  };

  std::unique_ptr<weave::Connection> connection_;
  std::unique_ptr<weave::BaseSocket> weave_socket_;
  api::ble::BlePeripheral::UniqueId remote_peripheral_id_;
  std::unique_ptr<InputStream> pipe_input_;
  WeaveOutputStream output_stream_;
};

// api::ble::BleServerSocket implementation: owns the GATT server hosting
// the Weave characteristics and accepts at most one connection at a time
// (see GattServerWeaveConnection's limitation above).
class BleServerSocket : public api::ble::BleServerSocket {
 public:
  BleServerSocket() = default;
  ~BleServerSocket() override;

  // Must be called exactly once, right after construction and before the
  // GATT server's write callback can fire for real (i.e. before the caller
  // creates the Weave characteristics on `gatt_server`).
  void Initialize(std::unique_ptr<api::ble::GattServer> gatt_server,
                  api::ble::GattCharacteristic write_characteristic,
                  api::ble::GattCharacteristic notify_characteristic);

  std::unique_ptr<api::ble::BleSocket> Accept() override;
  Exception Close() override;

  // Bridged from ServerGattConnectionCallback::on_characteristic_write_cb.
  // Safe to call before Initialize() (silently dropped).
  void OnCharacteristicWrite(api::ble::BlePeripheral::UniqueId remote_id,
                            const api::ble::GattCharacteristic& characteristic,
                            absl::string_view data);

 private:
  struct PendingConnection {
    std::unique_ptr<GattServerWeaveConnection> connection;
    std::unique_ptr<weave::ServerSocket> weave_socket;
    std::unique_ptr<InputStream> pipe_input;
  };

  void OnWeaveConnected(api::ble::BlePeripheral::UniqueId remote_id);
  void OnConnectionClosed(api::ble::BlePeripheral::UniqueId remote_id);

  std::unique_ptr<api::ble::GattServer> gatt_server_;
  api::ble::GattCharacteristic notify_characteristic_;

  std::mutex mutex_;
  std::condition_variable accept_cv_;
  bool closed_ = false;
  api::ble::BlePeripheral::UniqueId active_remote_id_ = 0;
  GattServerWeaveConnection* active_connection_ = nullptr;
  PendingConnection pending_;
  std::deque<std::unique_ptr<api::ble::BleSocket>> ready_sockets_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLE_SOCKET_H_
