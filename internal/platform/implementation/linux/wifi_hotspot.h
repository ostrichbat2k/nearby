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
// WifiHotspot medium: hosts (SoftAP) or joins (station) a WPA2 Wi-Fi network
// via NetworkManager's D-Bus API, and exchanges data over a plain POSIX TCP
// socket once associated -- same data-channel shape as WifiLan (see
// wifi_lan.h), just over a network NetworkManager brings up on demand rather
// than one that's already there.
//
// This is a real bandwidth-upgrade medium (unlike WifiLan's upgrade path,
// which is only offered by real Android when the connection was *already*
// over WifiLan from the start): a BLE-bootstrapped connection to a real
// phone only ever offers WIFI_HOTSPOT/WIFI_DIRECT/WIFI_AWARE/BLUETOOTH as
// upgrade candidates (confirmed via a real hardware test -- see the
// "Their supported medium name" log lines logged by
// BasePcpHandler::ComputeIntersectionOfSupportedMediums -- WIFI_LAN was
// never among them), and of those, WifiHotspot is the only one with a
// matching NetworkManager primitive that doesn't require a whole new
// protocol stack (hostapd config generation for a from-scratch AP, or
// wpa_supplicant P2P group negotiation for WifiDirect).
//
// Role: per connections/implementation/mediums/wifi_hotspot/
// wifi_hotspot_bwu_handler.cc, whichever side locally calls
// BwuManager::InitiateBwuForEndpoint() (i.e. calls
// NearbyConnectionsManager::UpgradeBandwidth()) becomes the upgrade
// *initiator*, which for WIFI_HOTSPOT means it's the one that creates the
// SoftAP (StartWifiHotspot) and sends its SSID/password to the peer; the
// other side (the BWU *target*) is the one that joins it (ConnectWifiHotspot
// + ConnectToService). Our own ReceiveSession driver calls UpgradeBandwidth
// right after accepting -- matching real Android's
// IncomingShareSession::TryUpgradeBandwidth -- so for a real phone sending
// to this Linux box, *this box* ends up hosting the SoftAP and the phone
// joins it (consistent with a real-world observation: the phone's own WiFi
// visibly disconnects from its prior network while this is happening, since
// a phone's WiFi radio can only be associated with one AP at a time).
// ConnectWifiHotspot/ConnectToService (the join role) is implemented too,
// for the reverse direction (this box sending to a phone, which would then
// host the SoftAP for us to join).

#ifndef PLATFORM_IMPL_LINUX_WIFI_HOTSPOT_H_
#define PLATFORM_IMPL_LINUX_WIFI_HOTSPOT_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "absl/strings/string_view.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/linux/wifi_lan.h"
#include "internal/platform/implementation/wifi_hotspot.h"
#include "internal/platform/input_stream.h"
#include "internal/platform/output_stream.h"
#include "internal/platform/service_address.h"
#include "internal/platform/wifi_credential.h"

namespace nearby {
namespace linux_platform {

// Blocking InputStream/OutputStream directly over a connected TCP socket fd.
// Shares linux_platform::SharedSocketFd (wifi_lan.h) with WifiLan -- the
// close-race it guards against (a concurrent Close() racing a blocked
// recv()/send()) is identical for any plain-TCP-backed medium, so there's no
// reason to duplicate it.
class WifiHotspotInputStream : public InputStream {
 public:
  explicit WifiHotspotInputStream(std::shared_ptr<SharedSocketFd> fd)
      : fd_(std::move(fd)) {}
  ~WifiHotspotInputStream() override = default;

  ExceptionOr<ByteArray> Read(std::int64_t size) override;
  Exception Close() override;

 private:
  std::shared_ptr<SharedSocketFd> fd_;
};

class WifiHotspotOutputStream : public OutputStream {
 public:
  explicit WifiHotspotOutputStream(std::shared_ptr<SharedSocketFd> fd)
      : fd_(std::move(fd)) {}
  ~WifiHotspotOutputStream() override = default;

  Exception Write(absl::string_view data) override;
  Exception Flush() override;
  Exception Close() override;

 private:
  std::shared_ptr<SharedSocketFd> fd_;
};

class WifiHotspotSocket : public api::WifiHotspotSocket {
 public:
  // Takes ownership of an already-connected socket fd.
  explicit WifiHotspotSocket(int fd);
  ~WifiHotspotSocket() override;

  WifiHotspotSocket(const WifiHotspotSocket&) = delete;
  WifiHotspotSocket& operator=(const WifiHotspotSocket&) = delete;

  InputStream& GetInputStream() override { return input_stream_; }
  OutputStream& GetOutputStream() override { return output_stream_; }
  Exception Close() override;

 private:
  std::shared_ptr<SharedSocketFd> fd_;
  WifiHotspotInputStream input_stream_;
  WifiHotspotOutputStream output_stream_;
};

// Concrete WifiHotspotServerSocket: a listening TCP socket, bound to all
// interfaces (the SoftAP's own interface is the only one actually reachable
// by the joining peer, so there's no need to restrict the bind). The
// address reported back to the peer (via PopulateHotspotCredentials) is the
// SoftAP interface's specific address, captured by WifiHotspotMedium when
// the SoftAP came up.
class WifiHotspotServerSocket : public api::WifiHotspotServerSocket {
 public:
  // Binds and listens on `port` (0 = let the OS pick an ephemeral port).
  // `advertise_ip_raw` is the raw 4-byte IPv4 address to report back to the
  // peer in PopulateHotspotCredentials (the SoftAP's own interface address).
  // Returns nullptr on failure.
  static std::unique_ptr<WifiHotspotServerSocket> Listen(
      int port, std::string advertise_ip_raw);

  ~WifiHotspotServerSocket() override;

  WifiHotspotServerSocket(const WifiHotspotServerSocket&) = delete;
  WifiHotspotServerSocket& operator=(const WifiHotspotServerSocket&) = delete;

  int GetPort() const override { return port_; }
  std::unique_ptr<api::WifiHotspotSocket> Accept() override;
  Exception Close() override;
  void PopulateHotspotCredentials(
      HotspotCredentials& hotspot_credentials) override;

 private:
  WifiHotspotServerSocket(int fd, int port, std::string advertise_ip_raw)
      : fd_(fd), port_(port), advertise_ip_raw_(std::move(advertise_ip_raw)) {}

  std::mutex mutex_;
  int fd_;
  int port_;
  std::string advertise_ip_raw_;
};

// Concrete WifiHotspotMedium: hosts or joins a WPA2 Wi-Fi network via
// NetworkManager D-Bus (org.freedesktop.NetworkManager), using the same
// process-wide system-bus connection as BlueZ/Avahi (see bluez.h). Supports
// hosting one SoftAP and/or being joined to one remote hotspot at a time --
// matches the real contract (connections::WifiHotspot never does more than
// one of either role concurrently).
class WifiHotspotMedium : public api::WifiHotspotMedium {
 public:
  WifiHotspotMedium() = default;
  ~WifiHotspotMedium() override;

  WifiHotspotMedium(const WifiHotspotMedium&) = delete;
  WifiHotspotMedium& operator=(const WifiHotspotMedium&) = delete;

  bool IsInterfaceValid() const override;

  std::unique_ptr<api::WifiHotspotSocket> ConnectToService(
      const ServiceAddress& service_address,
      CancellationFlag* cancellation_flag) override;

  std::unique_ptr<api::WifiHotspotServerSocket> ListenForService(
      int port) override;

  bool StartWifiHotspot(HotspotCredentials* hotspot_credentials) override;
  bool StopWifiHotspot() override;

  bool ConnectWifiHotspot(
      const HotspotCredentials& hotspot_credentials) override;
  bool DisconnectWifiHotspot() override;

  std::optional<std::pair<std::int32_t, std::int32_t>> GetDynamicPortRange()
      override {
    // NetworkManager doesn't expose a fixed port range for this; let the OS
    // pick an ephemeral port, same as WifiLan.
    return std::nullopt;
  }

 private:
  std::mutex mutex_;

  // Set while hosting a SoftAP, from StartWifiHotspot() until
  // StopWifiHotspot().
  std::string ap_settings_path_;
  std::string ap_active_path_;
  std::string ap_gateway_ip_raw_;  // Raw 4-byte IPv4 of our own AP interface.

  // Set while joined to a remote hotspot as a station, from
  // ConnectWifiHotspot() until DisconnectWifiHotspot().
  std::string joined_settings_path_;
  std::string joined_active_path_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_WIFI_HOTSPOT_H_
