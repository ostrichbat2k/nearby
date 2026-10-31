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

#include "internal/platform/implementation/linux/ble_socket.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/logging.h"
#include "internal/platform/pipe.h"
#include "internal/platform/uuid.h"
#include "internal/weave/connection.h"

namespace nearby {
namespace linux_platform {

const Uuid& WeaveServiceUuid() {
  static const Uuid* uuid =
      new Uuid(*Uuid::FromString("0000fef3-0000-1000-8000-00805f9b34fb"));
  return *uuid;
}

const Uuid& WeaveToPeripheralCharacteristicUuid() {
  static const Uuid* uuid =
      new Uuid(*Uuid::FromString("00000100-0004-1000-8000-001a11000101"));
  return *uuid;
}

const Uuid& WeaveFromPeripheralCharacteristicUuid() {
  static const Uuid* uuid =
      new Uuid(*Uuid::FromString("00000100-0004-1000-8000-001a11000102"));
  return *uuid;
}

// ---------------------------------------------------------------------------
// GattClientWeaveConnection
// ---------------------------------------------------------------------------

void GattClientWeaveConnection::SetGattClient(
    std::unique_ptr<api::ble::GattClient> gatt_client,
    api::ble::GattCharacteristic write_characteristic,
    api::ble::GattCharacteristic notify_characteristic) {
  gatt_client_ = std::move(gatt_client);
  write_characteristic_ = write_characteristic;
  notify_characteristic_ = notify_characteristic;
}

void GattClientWeaveConnection::Initialize(weave::ConnectionCallback callback) {
  callback_ = std::move(callback);
}

void GattClientWeaveConnection::Transmit(std::string packet) {
  bool ok = gatt_client_->WriteCharacteristic(
      write_characteristic_, packet,
      api::ble::GattClient::WriteType::kWithResponse);
  if (callback_.on_transmit_cb) {
    callback_.on_transmit_cb(
        ok ? absl::OkStatus()
           : absl::InternalError("Weave GATT characteristic write failed"));
  }
}

void GattClientWeaveConnection::Close() {
  if (gatt_client_ != nullptr) {
    gatt_client_->Disconnect();
  }
}

void GattClientWeaveConnection::OnNotifyReceived(absl::string_view value) {
  if (callback_.on_remote_transmit_cb) {
    callback_.on_remote_transmit_cb(std::string(value));
  }
}

void GattClientWeaveConnection::OnGattDisconnected() {
  if (callback_.on_disconnected_cb) {
    callback_.on_disconnected_cb();
  }
}

// ---------------------------------------------------------------------------
// GattServerWeaveConnection
// ---------------------------------------------------------------------------

void GattServerWeaveConnection::Initialize(weave::ConnectionCallback callback) {
  callback_ = std::move(callback);
}

void GattServerWeaveConnection::Transmit(std::string packet) {
  absl::Status status = server_->NotifyCharacteristicChanged(
      notify_characteristic_, /*confirm=*/false, ByteArray(packet));
  if (!status.ok()) {
    LOG(WARNING) << "Weave GATT notify failed: " << status;
  }
  if (callback_.on_transmit_cb) {
    callback_.on_transmit_cb(status);
  }
}

void GattServerWeaveConnection::Close() {
  if (on_closed_) {
    on_closed_();
  }
}

void GattServerWeaveConnection::OnRemoteTransmit(absl::string_view data) {
  if (callback_.on_remote_transmit_cb) {
    callback_.on_remote_transmit_cb(std::string(data));
  }
}

void GattServerWeaveConnection::OnGattDisconnected() {
  if (callback_.on_disconnected_cb) {
    callback_.on_disconnected_cb();
  }
}

// ---------------------------------------------------------------------------
// BleSocket
// ---------------------------------------------------------------------------

BleSocket::BleSocket(std::unique_ptr<weave::Connection> connection,
                     std::unique_ptr<weave::BaseSocket> weave_socket,
                     api::ble::BlePeripheral::UniqueId remote_peripheral_id,
                     std::unique_ptr<InputStream> pipe_input)
    : connection_(std::move(connection)),
      weave_socket_(std::move(weave_socket)),
      remote_peripheral_id_(remote_peripheral_id),
      pipe_input_(std::move(pipe_input)),
      output_stream_(*weave_socket_) {}

BleSocket::~BleSocket() { Close(); }

Exception BleSocket::Close() {
  // Deliberately not calling weave_socket_->Disconnect() here: it only
  // *schedules* (on weave's own executor thread) writing a graceful error
  // control packet through `connection_`, and returns immediately. Tearing
  // down `connection_` right after -- as a naive Close() would -- races
  // that scheduled write: confirmed on real hardware, where the GATT
  // characteristic write for that final packet failed with "unknown
  // characteristic" because connection_->Close() (for the client role,
  // GattClientWeaveConnection::Close() -> GattClient::Disconnect()) had
  // already cleared the characteristic map by the time it ran. The
  // underlying transport going away is itself a perfectly normal way for
  // the remote to observe disconnection, so just tear it down directly and
  // skip the packet.
  connection_->Close();
  pipe_input_->Close();
  return {Exception::kSuccess};
}

Exception BleSocket::WeaveOutputStream::Write(absl::string_view data) {
  ExceptionOr<absl::Status> result =
      socket_.Write(ByteArray(std::string(data))).Get();
  if (!result.ok()) {
    return {result.exception()};
  }
  if (!result.result().ok()) {
    LOG(WARNING) << "Weave socket write failed: " << result.result();
    return {Exception::kIo};
  }
  return {Exception::kSuccess};
}

// ---------------------------------------------------------------------------
// BleServerSocket
// ---------------------------------------------------------------------------

BleServerSocket::~BleServerSocket() { Close(); }

void BleServerSocket::Initialize(
    std::unique_ptr<api::ble::GattServer> gatt_server,
    api::ble::GattCharacteristic write_characteristic,
    api::ble::GattCharacteristic notify_characteristic) {
  std::lock_guard<std::mutex> lock(mutex_);
  gatt_server_ = std::move(gatt_server);
  notify_characteristic_ = notify_characteristic;
}

void BleServerSocket::OnCharacteristicWrite(
    api::ble::BlePeripheral::UniqueId remote_id,
    const api::ble::GattCharacteristic& characteristic,
    absl::string_view data) {
  if (characteristic.uuid != WeaveToPeripheralCharacteristicUuid()) return;

  // Held across OnRemoteTransmit() below (not just the bookkeeping above)
  // deliberately: OnConnectionClosed() can run concurrently on the weave
  // socket's own executor thread (e.g. a disconnect racing with this
  // write), and it can destroy `active_connection_`'s pending connection
  // via `pending_ = PendingConnection()`. Serializing against that through
  // this same mutex_ is what keeps `connection_to_notify` from dangling.
  // on_remote_transmit_cb/on_receive_cb/on_error_cb -- the only callbacks
  // OnRemoteTransmit can synchronously trigger -- never touch mutex_
  // themselves, so this can't deadlock against OnWeaveConnected/
  // OnConnectionClosed (both of which run asynchronously, on a different
  // thread, never inline from this call).
  std::lock_guard<std::mutex> lock(mutex_);
  GattServerWeaveConnection* connection_to_notify = nullptr;
  if (closed_ || gatt_server_ == nullptr) return;

  if (active_connection_ == nullptr) {
    // A new incoming Weave connection. Build the Connection adapter and
    // the weave::ServerSocket on top of it; the pipe is created now (not
    // once the handshake completes) so on_receive_cb has somewhere to
    // write starting from the very first data packet.
    auto connection = std::make_unique<GattServerWeaveConnection>(
        gatt_server_.get(), notify_characteristic_,
        [this, remote_id]() { OnConnectionClosed(remote_id); });
    GattServerWeaveConnection* connection_raw = connection.get();

    auto [pipe_input, pipe_output] = CreatePipe();
    std::shared_ptr<OutputStream> shared_pipe_output(std::move(pipe_output));

    auto weave_socket = std::make_unique<weave::ServerSocket>(
        *connection_raw,
        weave::SocketCallback{
            .on_connected_cb = [this, remote_id]() {
              OnWeaveConnected(remote_id);
            },
            .on_disconnected_cb = [this, remote_id]() {
              OnConnectionClosed(remote_id);
            },
            .on_receive_cb = [shared_pipe_output](std::string message) {
              shared_pipe_output->Write(message);
            },
            .on_error_cb = [remote_id](absl::Status status) {
              LOG(WARNING) << "Weave server socket error for peripheral "
                           << remote_id << ": " << status;
            },
        });

    active_connection_ = connection_raw;
    active_remote_id_ = remote_id;
    pending_.connection = std::move(connection);
    pending_.weave_socket = std::move(weave_socket);
    pending_.pipe_input = std::move(pipe_input);
  } else if (active_remote_id_ != remote_id) {
    LOG(WARNING) << "Rejecting GATT write from peripheral " << remote_id
                 << " while a Weave connection to peripheral "
                 << active_remote_id_ << " is already active.";
    return;
  }
  connection_to_notify = active_connection_;
  connection_to_notify->OnRemoteTransmit(data);
}

void BleServerSocket::OnWeaveConnected(
    api::ble::BlePeripheral::UniqueId remote_id) {
  std::unique_ptr<api::ble::BleSocket> socket;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_remote_id_ != remote_id || pending_.connection == nullptr) {
      return;
    }
    socket = std::make_unique<BleSocket>(
        std::move(pending_.connection), std::move(pending_.weave_socket),
        remote_id, std::move(pending_.pipe_input));
    ready_sockets_.push_back(std::move(socket));
  }
  accept_cv_.notify_one();
}

void BleServerSocket::OnConnectionClosed(
    api::ble::BlePeripheral::UniqueId remote_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_remote_id_ == remote_id) {
    active_connection_ = nullptr;
    active_remote_id_ = 0;
    pending_ = PendingConnection();
  }
}

std::unique_ptr<api::ble::BleSocket> BleServerSocket::Accept() {
  std::unique_lock<std::mutex> lock(mutex_);
  accept_cv_.wait(lock,
                  [this] { return closed_ || !ready_sockets_.empty(); });
  if (ready_sockets_.empty()) return nullptr;
  std::unique_ptr<api::ble::BleSocket> socket =
      std::move(ready_sockets_.front());
  ready_sockets_.pop_front();
  return socket;
}

Exception BleServerSocket::Close() {
  std::unique_ptr<api::ble::GattServer> gatt_server;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return {Exception::kSuccess};
    closed_ = true;
    gatt_server = std::move(gatt_server_);
  }
  accept_cv_.notify_all();
  if (gatt_server != nullptr) {
    gatt_server->Stop();
  }
  return {Exception::kSuccess};
}

}  // namespace linux_platform
}  // namespace nearby
