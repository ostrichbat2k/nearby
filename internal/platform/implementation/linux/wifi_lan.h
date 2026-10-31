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
// WifiLan medium: advertise/discover over mDNS via Avahi's D-Bus API (see
// avahi.h), and exchange data over plain POSIX TCP sockets. This is the
// medium Nearby Connections uses for bandwidth-upgrade data channels once a
// connection has been bootstrapped over BLE.

#ifndef PLATFORM_IMPL_LINUX_WIFI_LAN_H_
#define PLATFORM_IMPL_LINUX_WIFI_LAN_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/container/flat_hash_map.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/implementation/upgrade_address_info.h"
#include "internal/platform/implementation/wifi_lan.h"
#include "internal/platform/input_stream.h"
#include "internal/platform/nsd_service_info.h"
#include "internal/platform/output_stream.h"
#include "internal/platform/service_address.h"

namespace nearby {
namespace linux_platform {

// Thread-safe holder for a socket fd shared between a WifiLanSocket and its
// input/output streams, so that Close() -- possibly called from a different
// thread to cancel a blocked Read()/Write(), e.g. via CancellationFlag --
// can shut the connection down without two threads independently deciding
// whether/when to call close() on the same fd.
class SharedSocketFd {
 public:
  explicit SharedSocketFd(int fd) : fd_(fd) {}

  // Current fd, or -1 if already closed. Safe to call concurrently with
  // Close(); the caller should still treat a blocking syscall on the
  // returned fd as liable to fail/unblock if Close() runs concurrently.
  int fd() {
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_;
  }

  // Shuts down (to unblock any thread currently blocked in recv()/send() on
  // this fd) and closes the fd. Idempotent; safe to call more than once or
  // concurrently with itself.
  void Close();

 private:
  std::mutex mutex_;
  int fd_;
};

// Blocking InputStream/OutputStream directly over a connected TCP socket fd.
class WifiLanInputStream : public InputStream {
 public:
  explicit WifiLanInputStream(std::shared_ptr<SharedSocketFd> fd)
      : fd_(std::move(fd)) {}
  ~WifiLanInputStream() override = default;

  ExceptionOr<ByteArray> Read(std::int64_t size) override;
  Exception Close() override;

 private:
  std::shared_ptr<SharedSocketFd> fd_;
};

class WifiLanOutputStream : public OutputStream {
 public:
  explicit WifiLanOutputStream(std::shared_ptr<SharedSocketFd> fd)
      : fd_(std::move(fd)) {}
  ~WifiLanOutputStream() override = default;

  Exception Write(absl::string_view data) override;
  Exception Flush() override;
  Exception Close() override;

 private:
  std::shared_ptr<SharedSocketFd> fd_;
};

// Concrete WifiLanSocket: a connected TCP socket. `fd_` is shared with the
// input/output streams (rather than owned exclusively) so that Close() can
// shut the socket down out from under a concurrently-blocked Read()/Write().
class WifiLanSocket : public api::WifiLanSocket {
 public:
  // Takes ownership of an already-connected socket fd.
  explicit WifiLanSocket(int fd);
  ~WifiLanSocket() override;

  WifiLanSocket(const WifiLanSocket&) = delete;
  WifiLanSocket& operator=(const WifiLanSocket&) = delete;

  InputStream& GetInputStream() override { return input_stream_; }
  OutputStream& GetOutputStream() override { return output_stream_; }
  Exception Close() override;

 private:
  std::shared_ptr<SharedSocketFd> fd_;
  WifiLanInputStream input_stream_;
  WifiLanOutputStream output_stream_;
};

// Concrete WifiLanServerSocket: a listening TCP socket.
class WifiLanServerSocket : public api::WifiLanServerSocket {
 public:
  // Binds and listens on `port` (0 = let the OS pick an ephemeral port).
  // Returns nullptr on failure.
  static std::unique_ptr<WifiLanServerSocket> Listen(int port);

  ~WifiLanServerSocket() override;

  WifiLanServerSocket(const WifiLanServerSocket&) = delete;
  WifiLanServerSocket& operator=(const WifiLanServerSocket&) = delete;

  std::string GetIPAddress() const override;
  int GetPort() const override { return port_; }
  std::unique_ptr<api::WifiLanSocket> Accept() override;
  Exception Close() override;

 private:
  explicit WifiLanServerSocket(int fd, int port) : fd_(fd), port_(port) {}

  std::mutex mutex_;
  int fd_;
  int port_;
};

// Concrete WifiLanMedium: mDNS advertise/discover via Avahi, TCP data
// channel. Supports multiple concurrently-advertised service types and
// multiple concurrent discoveries (one Avahi EntryGroup/ServiceBrowser per
// service type), and multiple concurrently-listening ports, matching the
// api::WifiLanMedium contract.
class WifiLanMedium : public api::WifiLanMedium {
 public:
  WifiLanMedium() = default;
  ~WifiLanMedium() override;

  WifiLanMedium(const WifiLanMedium&) = delete;
  WifiLanMedium& operator=(const WifiLanMedium&) = delete;

  bool IsNetworkConnected() const override;

  bool StartAdvertising(const NsdServiceInfo& nsd_service_info) override;
  bool StopAdvertising(const NsdServiceInfo& nsd_service_info) override;

  bool StartDiscovery(const std::string& service_type,
                      DiscoveredServiceCallback callback) override;
  bool StopDiscovery(const std::string& service_type) override;

  std::unique_ptr<api::WifiLanSocket> ConnectToService(
      const NsdServiceInfo& remote_service_info,
      CancellationFlag* cancellation_flag) override;
  std::unique_ptr<api::WifiLanSocket> ConnectToService(
      const ServiceAddress& service_address,
      CancellationFlag* cancellation_flag) override;

  std::unique_ptr<api::WifiLanServerSocket> ListenForService(
      int port) override;

  absl::optional<std::pair<std::int32_t, std::int32_t>> GetDynamicPortRange()
      override {
    // No fixed range is enforced on Linux; let the OS pick an ephemeral port.
    return absl::nullopt;
  }

  api::UpgradeAddressInfo GetUpgradeAddressCandidates(
      const api::WifiLanServerSocket& server_socket) override;

 private:
  struct AdvertisingEntry {
    std::string object_path;
    std::unique_ptr<sdbus::IProxy> entry_group_proxy;
  };

  struct DiscoveryEntry {
    std::string object_path;
    std::unique_ptr<sdbus::IProxy> browser_proxy;
    DiscoveredServiceCallback callback;
  };

  std::mutex mutex_;
  absl::flat_hash_map<std::string, AdvertisingEntry> advertising_;  // by service type
  absl::flat_hash_map<std::string, DiscoveryEntry> discovering_;    // by service type
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_WIFI_LAN_H_
