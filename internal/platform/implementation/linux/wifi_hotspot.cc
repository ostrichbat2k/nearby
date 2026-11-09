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

#include "internal/platform/implementation/linux/wifi_hotspot.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/logging.h"

namespace nearby {
namespace linux_platform {

namespace {

// NetworkManager D-Bus constants, ground-truthed against this machine's
// installed NetworkManager (busctl introspection of the real running
// service) and /usr/include/libnm/nm-dbus-interface.h -- not guessed.
constexpr const char* kNMService = "org.freedesktop.NetworkManager";
constexpr const char* kNMPath = "/org/freedesktop/NetworkManager";
constexpr const char* kNMInterface = "org.freedesktop.NetworkManager";
constexpr const char* kNMDeviceInterface =
    "org.freedesktop.NetworkManager.Device";
constexpr const char* kNMActiveConnectionInterface =
    "org.freedesktop.NetworkManager.Connection.Active";
constexpr const char* kNMIP4ConfigInterface =
    "org.freedesktop.NetworkManager.IP4Config";
constexpr const char* kNMSettingsConnectionInterface =
    "org.freedesktop.NetworkManager.Settings.Connection";

// NMDeviceType (nm-dbus-interface.h).
constexpr uint32_t kNMDeviceTypeWifi = 2;
// NMActiveConnectionState (nm-dbus-interface.h).
constexpr uint32_t kNMActiveConnectionStateActivating = 1;
constexpr uint32_t kNMActiveConnectionStateActivated = 2;

constexpr auto kNMActivationTimeout = std::chrono::seconds(25);
constexpr auto kConnectTimeout = std::chrono::milliseconds(3000);

using NMSettingsMap = std::map<std::string, std::map<std::string, sdbus::Variant>>;

std::unique_ptr<sdbus::IProxy> CreateNMProxy(const std::string& object_path) {
  return sdbus::createProxy(BluezSystemBus::Get().connection(),
                             sdbus::ServiceName{kNMService},
                             sdbus::ObjectPath{object_path});
}

// Raw-bytes (network order) encoding matching ServiceAddress's convention.
std::string RawIPv4Bytes(const struct in_addr& addr) {
  return std::string(reinterpret_cast<const char*>(&addr.s_addr), 4);
}

std::optional<std::string> HumanReadableIPv4ToRawBytes(
    const std::string& human_readable) {
  struct in_addr addr{};
  if (inet_pton(AF_INET, human_readable.c_str(), &addr) != 1) return std::nullopt;
  return RawIPv4Bytes(addr);
}

std::string GenerateHotspotSsid() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  std::string suffix;
  for (int i = 0; i < 6; ++i) suffix += "0123456789abcdef"[dist(rd)];
  return "quickshare-" + suffix;
}

// WPA2-PSK requires an 8-63 character ASCII passphrase.
std::string GenerateHotspotPassword() {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 61);
  std::string password;
  for (int i = 0; i < 16; ++i) password += kAlphabet[dist(rd)];
  return password;
}

// Finds the D-Bus object path of this machine's Wi-Fi device. Assumes at
// most one matters for our purposes (true of every real machine this has
// been tested on); if there were several, NetworkManager would still
// activate fine on whichever we pick.
std::optional<std::string> FindWifiDevicePath() {
  auto nm_proxy = CreateNMProxy(kNMPath);
  std::vector<sdbus::ObjectPath> devices;
  try {
    nm_proxy->callMethod("GetDevices").onInterface(kNMInterface).storeResultsTo(devices);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "NetworkManager GetDevices failed: " << e.what();
    return std::nullopt;
  }
  for (const sdbus::ObjectPath& path : devices) {
    try {
      auto dev_proxy = CreateNMProxy(path);
      uint32_t device_type = dev_proxy->getProperty("DeviceType")
                                 .onInterface(kNMDeviceInterface)
                                 .get<uint32_t>();
      if (device_type == kNMDeviceTypeWifi) return std::string(path);
    } catch (const sdbus::Error&) {
      continue;
    }
  }
  return std::nullopt;
}

// Blocks (with a bounded timeout) until the active connection at
// `active_path` reaches NM_ACTIVE_CONNECTION_STATE_ACTIVATED, or returns
// false on failure/timeout/deactivation. Subscribes to StateChanged before
// checking the current state, same ordering discipline as the Avahi
// EntryGroup wait in wifi_lan.cc, so a transition that happens between the
// subscribe and the initial check can't be missed.
bool WaitForActivation(const std::string& active_path) {
  auto active_proxy = CreateNMProxy(active_path);
  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  std::optional<uint32_t> final_state;

  active_proxy->uponSignal("StateChanged")
      .onInterface(kNMActiveConnectionInterface)
      .call([&](uint32_t state, uint32_t reason) {
        if (state == kNMActiveConnectionStateActivating) return;
        std::lock_guard<std::mutex> lock(wait_mutex);
        final_state = state;
        wait_cv.notify_all();
      });

  uint32_t current_state = 0;
  try {
    current_state = active_proxy->getProperty("State")
                        .onInterface(kNMActiveConnectionInterface)
                        .get<uint32_t>();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to read active connection state: " << e.what();
    return false;
  }
  if (current_state == kNMActiveConnectionStateActivated) return true;
  if (current_state != kNMActiveConnectionStateActivating) {
    // Already past activating (failed or deactivated) before we even
    // finished subscribing.
    return false;
  }

  std::unique_lock<std::mutex> lock(wait_mutex);
  wait_cv.wait_for(lock, kNMActivationTimeout,
                   [&] { return final_state.has_value(); });
  return final_state.has_value() &&
         *final_state == kNMActiveConnectionStateActivated;
}

// Reads the first IPv4 address NetworkManager has assigned to the
// connection's own interface (for AP/shared mode, this is the gateway
// address NetworkManager's internal DHCP server hands out to joining
// clients, typically 10.42.0.1).
//
// The ActiveConnection reaching ACTIVATED (see WaitForActivation) does not
// guarantee Ip4Config/AddressData is populated yet -- confirmed via a real
// hardware test (2026-11-09) that reading this immediately after activation
// intermittently returns an empty AddressData (observed as "Could not read
// SoftAP's own IPv4 address", which self-aborted the whole SoftAP before a
// joining peer even had a chance). Retries for a couple seconds to close
// that race instead of failing on the first empty read.
std::optional<std::string> ReadOwnIPv4Address(
    const std::string& active_path) {
  constexpr int kMaxAttempts = 20;
  constexpr auto kRetryDelay = std::chrono::milliseconds(100);
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) std::this_thread::sleep_for(kRetryDelay);
    try {
      auto active_proxy = CreateNMProxy(active_path);
      sdbus::ObjectPath ip4config_path =
          active_proxy->getProperty("Ip4Config")
              .onInterface(kNMActiveConnectionInterface)
              .get<sdbus::ObjectPath>();
      if (ip4config_path.empty() || ip4config_path == "/") continue;
      auto ip4_proxy = CreateNMProxy(ip4config_path);
      std::vector<std::map<std::string, sdbus::Variant>> address_data =
          ip4_proxy->getProperty("AddressData")
              .onInterface(kNMIP4ConfigInterface)
              .get<std::vector<std::map<std::string, sdbus::Variant>>>();
      if (address_data.empty()) continue;
      auto it = address_data[0].find("address");
      if (it == address_data[0].end()) continue;
      return it->second.get<std::string>();
    } catch (const sdbus::Error& e) {
      LOG(WARNING) << "Transient failure reading own IPv4 address (attempt "
                   << attempt << "): " << e.what();
    }
  }
  LOG(ERROR) << "Failed to read own IPv4 address after retrying.";
  return std::nullopt;
}

void DeleteNMConnection(const std::string& settings_path) {
  if (settings_path.empty()) return;
  try {
    CreateNMProxy(settings_path)
        ->callMethod("Delete")
        .onInterface(kNMSettingsConnectionInterface);
  } catch (const sdbus::Error& e) {
    LOG(WARNING) << "Failed to delete NetworkManager connection "
                 << settings_path << ": " << e.what();
  }
}

void DeactivateNMConnection(const std::string& active_path) {
  if (active_path.empty()) return;
  try {
    CreateNMProxy(kNMPath)
        ->callMethod("DeactivateConnection")
        .onInterface(kNMInterface)
        .withArguments(sdbus::ObjectPath{active_path});
  } catch (const sdbus::Error& e) {
    LOG(WARNING) << "DeactivateConnection failed for " << active_path << ": "
                 << e.what();
  }
}

// Mirrors wifi_lan.cc's ConnectWithTimeout (same connect()/poll()/cancel
// pattern); not shared directly since it's a small static function local to
// each translation unit, same as upstream's own per-medium duplication
// elsewhere in this tree.
int ConnectWithTimeout(const struct sockaddr_storage& addr, socklen_t addr_len,
                       std::chrono::milliseconds timeout,
                       CancellationFlag* cancellation_flag) {
  int fd = socket(addr.ss_family, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG(ERROR) << "Failed to create socket: " << strerror(errno);
    return -1;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  int rc = connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), addr_len);
  if (rc == 0) {
    fcntl(fd, F_SETFL, flags);
    return fd;
  }
  if (errno != EINPROGRESS) {
    LOG(ERROR) << "connect() failed: " << strerror(errno);
    close(fd);
    return -1;
  }

  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
      LOG(INFO) << "WifiHotspot connect cancelled.";
      close(fd);
      return -1;
    }
    auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::milliseconds(0)) {
      LOG(ERROR) << "Timed out connecting to WifiHotspot service.";
      close(fd);
      return -1;
    }
    auto slice = std::min<std::chrono::milliseconds>(
        std::chrono::milliseconds(100),
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
    struct pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
    int poll_rc = poll(&pfd, 1, static_cast<int>(slice.count()));
    if (poll_rc < 0) {
      if (errno == EINTR) continue;
      LOG(ERROR) << "poll() failed: " << strerror(errno);
      close(fd);
      return -1;
    }
    if (poll_rc == 0) continue;
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0) {
      LOG(ERROR) << "connect() failed: " << strerror(so_error);
      close(fd);
      return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
  }
}

}  // namespace

ExceptionOr<ByteArray> WifiHotspotInputStream::Read(std::int64_t size) {
  int fd = fd_->fd();
  if (fd < 0) return {Exception::kIo};
  std::vector<char> buffer(size);
  ssize_t n = recv(fd, buffer.data(), buffer.size(), 0);
  if (n < 0) {
    LOG(ERROR) << "WifiHotspot socket recv() failed: " << strerror(errno);
    return {Exception::kIo};
  }
  return ExceptionOr<ByteArray>(ByteArray(buffer.data(), n));
}

Exception WifiHotspotInputStream::Close() { return {Exception::kSuccess}; }

Exception WifiHotspotOutputStream::Write(absl::string_view data) {
  int fd = fd_->fd();
  if (fd < 0) return {Exception::kIo};
  size_t written = 0;
  while (written < data.size()) {
    ssize_t n = send(fd, data.data() + written, data.size() - written,
                     MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      LOG(ERROR) << "WifiHotspot socket send() failed: " << strerror(errno);
      return {Exception::kIo};
    }
    written += n;
  }
  return {Exception::kSuccess};
}

Exception WifiHotspotOutputStream::Flush() { return {Exception::kSuccess}; }
Exception WifiHotspotOutputStream::Close() { return {Exception::kSuccess}; }

WifiHotspotSocket::WifiHotspotSocket(int fd)
    : fd_(std::make_shared<SharedSocketFd>(fd)),
      input_stream_(fd_),
      output_stream_(fd_) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

WifiHotspotSocket::~WifiHotspotSocket() { Close(); }

Exception WifiHotspotSocket::Close() {
  fd_->Close();
  return {Exception::kSuccess};
}

std::unique_ptr<WifiHotspotServerSocket> WifiHotspotServerSocket::Listen(
    int port, std::string advertise_ip_raw) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG(ERROR) << "Failed to create WifiHotspot listening socket: "
               << strerror(errno);
    return nullptr;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    LOG(ERROR) << "Failed to bind WifiHotspot server socket on port " << port
               << ": " << strerror(errno);
    close(fd);
    return nullptr;
  }
  socklen_t addr_len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) !=
      0) {
    LOG(ERROR) << "getsockname() failed: " << strerror(errno);
    close(fd);
    return nullptr;
  }
  int bound_port = ntohs(addr.sin_port);
  if (listen(fd, /*backlog=*/4) != 0) {
    LOG(ERROR) << "Failed to listen on WifiHotspot server socket: "
               << strerror(errno);
    close(fd);
    return nullptr;
  }
  LOG(INFO) << "WifiHotspot server socket listening on port " << bound_port;
  return std::unique_ptr<WifiHotspotServerSocket>(new WifiHotspotServerSocket(
      fd, bound_port, std::move(advertise_ip_raw)));
}

WifiHotspotServerSocket::~WifiHotspotServerSocket() { Close(); }

std::unique_ptr<api::WifiHotspotSocket> WifiHotspotServerSocket::Accept() {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
  }
  if (fd < 0) return nullptr;
  int client_fd = accept(fd, nullptr, nullptr);
  if (client_fd < 0) {
    if (errno != EBADF && errno != EINVAL) {
      LOG(ERROR) << "WifiHotspot accept() failed: " << strerror(errno);
    }
    return nullptr;
  }
  return std::make_unique<WifiHotspotSocket>(client_fd);
}

Exception WifiHotspotServerSocket::Close() {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
    fd_ = -1;
  }
  if (fd >= 0) close(fd);
  return {Exception::kSuccess};
}

void WifiHotspotServerSocket::PopulateHotspotCredentials(
    HotspotCredentials& hotspot_credentials) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (advertise_ip_raw_.empty()) return;
  ServiceAddress address{
      .address = {advertise_ip_raw_.begin(), advertise_ip_raw_.end()},
      .port = static_cast<uint16_t>(port_)};
  hotspot_credentials.SetAddressCandidates({address});
}

WifiHotspotMedium::~WifiHotspotMedium() {
  // Best-effort teardown; fire-and-forget since the medium is being
  // destroyed anyway, same discipline as WifiLanMedium's destructor.
  if (!ap_active_path_.empty()) {
    DeactivateNMConnection(ap_active_path_);
    DeleteNMConnection(ap_settings_path_);
  }
  if (!joined_active_path_.empty()) {
    DeactivateNMConnection(joined_active_path_);
    DeleteNMConnection(joined_settings_path_);
  }
}

bool WifiHotspotMedium::IsInterfaceValid() const {
  return FindWifiDevicePath().has_value();
}

std::unique_ptr<api::WifiHotspotSocket> WifiHotspotMedium::ConnectToService(
    const ServiceAddress& service_address,
    CancellationFlag* cancellation_flag) {
  if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
    LOG(INFO) << "WifiHotspot connect cancelled before starting.";
    return nullptr;
  }
  if (service_address.address.size() != 4) {
    LOG(ERROR) << "WifiHotspot ConnectToService requires an IPv4 address.";
    return nullptr;
  }
  struct sockaddr_storage addr{};
  auto* sin = reinterpret_cast<struct sockaddr_in*>(&addr);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(service_address.port);
  std::memcpy(&sin->sin_addr, service_address.address.data(), 4);

  int fd = ConnectWithTimeout(addr, sizeof(*sin), kConnectTimeout,
                              cancellation_flag);
  if (fd < 0) return nullptr;
  LOG(INFO) << "Connected to WifiHotspot service at port "
            << service_address.port;
  return std::make_unique<WifiHotspotSocket>(fd);
}

std::unique_ptr<api::WifiHotspotServerSocket>
WifiHotspotMedium::ListenForService(int port) {
  std::string advertise_ip_raw;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    advertise_ip_raw = ap_gateway_ip_raw_;
  }
  if (advertise_ip_raw.empty()) {
    LOG(ERROR) << "ListenForService called without an active SoftAP.";
    return nullptr;
  }
  return WifiHotspotServerSocket::Listen(port, advertise_ip_raw);
}

bool WifiHotspotMedium::StartWifiHotspot(
    HotspotCredentials* hotspot_credentials) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ap_active_path_.empty()) {
    LOG(WARNING) << "WifiHotspot SoftAP already started.";
    return true;
  }
  std::optional<std::string> device_path = FindWifiDevicePath();
  if (!device_path.has_value()) {
    LOG(ERROR) << "No WiFi device found; can't start SoftAP.";
    return false;
  }

  std::string ssid = GenerateHotspotSsid();
  std::string password = GenerateHotspotPassword();

  NMSettingsMap connection_settings = {
      {"connection",
       {{"id", sdbus::Variant(std::string("quickshare-linux-hotspot"))},
        {"type", sdbus::Variant(std::string("802-11-wireless"))},
        {"autoconnect", sdbus::Variant(false)}}},
      {"802-11-wireless",
       {{"ssid", sdbus::Variant(std::vector<uint8_t>(ssid.begin(), ssid.end()))},
        {"mode", sdbus::Variant(std::string("ap"))},
        {"band", sdbus::Variant(std::string("a"))},
        // Channel 36 (5180 MHz, UNII-1): the lowest 5GHz channel that's
        // unconditionally AP-legal indoors under every regulatory domain
        // this code has been tested under (ground-truthed via `iw reg get`
        // + `iw phy phy0 info` on this machine, 2026-11-09) without
        // triggering DFS/radar-detection requirements, unlike every channel
        // from 52-144 (all marked "no IR, radar detection" on this
        // adapter) -- wpa_supplicant's AP mode doesn't do the active radar
        // monitoring DFS requires, so those channels aren't usable for a
        // SoftAP at all. Channels 36/40/44/48 are all non-DFS in this same
        // regulatory domain, so they can legally bond into one 80MHz
        // segment -- see channel-width below.
        {"channel", sdbus::Variant(static_cast<uint32_t>(36))},
        // 80MHz: a real hardware test at the default "auto" (which came up
        // as 20MHz) showed 5GHz was NOT faster than 2.4GHz at the
        // application level (~8-11MB/s vs. ~16MB/s) despite a strong PHY
        // link (iw station dump: -35dBm, HE-MCS 11, 286Mbit/s rx bitrate) --
        // i.e. the radio wasn't the bottleneck at 20MHz width, so there was
        // no 5GHz advantage to find yet. Requesting the full legal 80MHz
        // segment (36+40+44+48) actually gives 5GHz a real bandwidth edge
        // over 2.4GHz's 40MHz cap to test against.
        {"channel-width", sdbus::Variant(static_cast<uint32_t>(80))}}},
      {"802-11-wireless-security",
       {{"key-mgmt", sdbus::Variant(std::string("wpa-psk"))},
        {"psk", sdbus::Variant(password)}}},
      {"ipv4", {{"method", sdbus::Variant(std::string("shared"))}}},
      {"ipv6", {{"method", sdbus::Variant(std::string("ignore"))}}},
  };

  sdbus::ObjectPath settings_path;
  sdbus::ObjectPath active_path;
  try {
    CreateNMProxy(kNMPath)
        ->callMethod("AddAndActivateConnection")
        .onInterface(kNMInterface)
        .withArguments(connection_settings, sdbus::ObjectPath{*device_path},
                       sdbus::ObjectPath{"/"})
        .storeResultsTo(settings_path, active_path);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "AddAndActivateConnection (SoftAP) failed: " << e.what();
    return false;
  }

  if (!WaitForActivation(active_path)) {
    LOG(ERROR) << "WifiHotspot SoftAP failed to activate.";
    DeleteNMConnection(settings_path);
    return false;
  }

  std::optional<std::string> gateway_ip = ReadOwnIPv4Address(active_path);
  if (!gateway_ip.has_value()) {
    LOG(ERROR) << "Could not read SoftAP's own IPv4 address.";
    DeactivateNMConnection(active_path);
    DeleteNMConnection(settings_path);
    return false;
  }
  std::optional<std::string> gateway_ip_raw =
      HumanReadableIPv4ToRawBytes(*gateway_ip);
  if (!gateway_ip_raw.has_value()) {
    LOG(ERROR) << "Failed to parse SoftAP gateway address " << *gateway_ip;
    DeactivateNMConnection(active_path);
    DeleteNMConnection(settings_path);
    return false;
  }

  ap_settings_path_ = settings_path;
  ap_active_path_ = active_path;
  ap_gateway_ip_raw_ = *gateway_ip_raw;

  hotspot_credentials->SetSSID(ssid);
  hotspot_credentials->SetPassword(password);
  // 5180 MHz = channel 36, matching the fixed 5GHz channel StartWifiHotspot
  // configures above.
  hotspot_credentials->SetFrequency(5180);
  LOG(INFO) << "Started WifiHotspot SoftAP: ssid=" << ssid
            << " gateway=" << *gateway_ip;
  return true;
}

bool WifiHotspotMedium::StopWifiHotspot() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ap_active_path_.empty()) return true;
  DeactivateNMConnection(ap_active_path_);
  DeleteNMConnection(ap_settings_path_);
  LOG(INFO) << "Stopped WifiHotspot SoftAP.";
  ap_settings_path_.clear();
  ap_active_path_.clear();
  ap_gateway_ip_raw_.clear();
  return true;
}

bool WifiHotspotMedium::ConnectWifiHotspot(
    const HotspotCredentials& hotspot_credentials) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!joined_active_path_.empty()) {
    LOG(WARNING) << "Already joined a WifiHotspot; disconnect first.";
    return false;
  }
  std::optional<std::string> device_path = FindWifiDevicePath();
  if (!device_path.has_value()) {
    LOG(ERROR) << "No WiFi device found; can't join SoftAP.";
    return false;
  }

  std::string ssid = hotspot_credentials.GetSSID();
  NMSettingsMap connection_settings = {
      {"connection",
       {{"id", sdbus::Variant(std::string("quickshare-linux-join-" + ssid))},
        {"type", sdbus::Variant(std::string("802-11-wireless"))},
        {"autoconnect", sdbus::Variant(false)}}},
      {"802-11-wireless",
       {{"ssid", sdbus::Variant(std::vector<uint8_t>(ssid.begin(), ssid.end()))},
        {"mode", sdbus::Variant(std::string("infrastructure"))}}},
      {"802-11-wireless-security",
       {{"key-mgmt", sdbus::Variant(std::string("wpa-psk"))},
        {"psk", sdbus::Variant(hotspot_credentials.GetPassword())}}},
      {"ipv4", {{"method", sdbus::Variant(std::string("auto"))}}},
      {"ipv6", {{"method", sdbus::Variant(std::string("ignore"))}}},
  };

  sdbus::ObjectPath settings_path;
  sdbus::ObjectPath active_path;
  try {
    CreateNMProxy(kNMPath)
        ->callMethod("AddAndActivateConnection")
        .onInterface(kNMInterface)
        .withArguments(connection_settings, sdbus::ObjectPath{*device_path},
                       sdbus::ObjectPath{"/"})
        .storeResultsTo(settings_path, active_path);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "AddAndActivateConnection (join " << ssid
               << ") failed: " << e.what();
    return false;
  }

  if (!WaitForActivation(active_path)) {
    LOG(ERROR) << "Failed to join WifiHotspot " << ssid;
    DeleteNMConnection(settings_path);
    return false;
  }

  joined_settings_path_ = settings_path;
  joined_active_path_ = active_path;
  LOG(INFO) << "Joined WifiHotspot " << ssid;
  return true;
}

bool WifiHotspotMedium::DisconnectWifiHotspot() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (joined_active_path_.empty()) return true;
  DeactivateNMConnection(joined_active_path_);
  DeleteNMConnection(joined_settings_path_);
  LOG(INFO) << "Disconnected from WifiHotspot.";
  joined_settings_path_.clear();
  joined_active_path_.clear();
  return true;
}

}  // namespace linux_platform
}  // namespace nearby
