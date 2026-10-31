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

#include "internal/platform/implementation/linux/wifi_lan.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ifaddrs.h>
#include <sdbus-c++/sdbus-c++.h>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/linux/avahi.h"
#include "internal/platform/implementation/upgrade_address_info.h"
#include "internal/platform/logging.h"
#include "internal/platform/nsd_service_info.h"
#include "internal/platform/service_address.h"

namespace nearby {
namespace linux_platform {

namespace {

// From metrics on other backends, P90 LAN connection latency is well under
// this; generous enough for a same-subnet TCP handshake without making a
// genuinely unreachable peer hang the caller for long.
constexpr auto kConnectTimeout = std::chrono::milliseconds(3000);
constexpr auto kEntryGroupCommitTimeout = std::chrono::seconds(5);

// Builds a raw-bytes (network order) ServiceAddress-style IP string from a
// sockaddr, matching NsdServiceInfo::SetIPAddress's convention of storing
// IPv4 addresses as a raw 4-byte string rather than a human-readable one.
std::string RawIPv4Bytes(const struct in_addr& addr) {
  return std::string(reinterpret_cast<const char*>(&addr.s_addr), 4);
}

// Resolves a hostname/IP address + port into a sockaddr via getaddrinfo,
// restricted to numeric host (we always already have a literal address --
// Avahi resolves names for us, and NsdServiceInfo stores literal
// addresses), trying every candidate address family.
bool ResolveNumericAddress(const std::string& host, int port,
                           struct sockaddr_storage& out, socklen_t& out_len) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  struct addrinfo* result = nullptr;
  int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                       &result);
  if (rc != 0 || result == nullptr) {
    LOG(ERROR) << "Failed to resolve address " << host << ": "
               << gai_strerror(rc);
    return false;
  }
  std::memcpy(&out, result->ai_addr, result->ai_addrlen);
  out_len = result->ai_addrlen;
  freeaddrinfo(result);
  return true;
}

// Connects a fresh TCP socket to `addr`, bounded by `timeout`. On success,
// returns the connected fd (caller owns it); on failure or timeout, returns
// -1 and leaves no fd behind. Polls `cancellation_flag` (if non-null) while
// waiting so a caller cancellation doesn't block for the full timeout.
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

  int rc = connect(fd, reinterpret_cast<const struct sockaddr*>(&addr),
                   addr_len);
  if (rc == 0) {
    fcntl(fd, F_SETFL, flags);  // restore blocking mode for later recv/send.
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
      LOG(INFO) << "WifiLan connect cancelled.";
      close(fd);
      return -1;
    }
    auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::milliseconds(0)) {
      LOG(ERROR) << "Timed out connecting to WifiLan service.";
      close(fd);
      return -1;
    }
    // Poll in short slices so cancellation is noticed promptly rather than
    // only at the end of the full timeout.
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
    if (poll_rc == 0) continue;  // slice elapsed, loop to recheck cancellation/deadline.
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0) {
      LOG(ERROR) << "connect() failed: " << strerror(so_error);
      close(fd);
      return -1;
    }
    fcntl(fd, F_SETFL, flags);  // restore blocking mode for later recv/send.
    return fd;
  }
}

}  // namespace

void SharedSocketFd::Close() {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
    fd_ = -1;
  }
  if (fd >= 0) {
    // shutdown() first so a thread currently blocked in recv()/send() on
    // this fd unblocks promptly rather than racing the close() below.
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
}

ExceptionOr<ByteArray> WifiLanInputStream::Read(std::int64_t size) {
  int fd = fd_->fd();
  if (fd < 0) return {Exception::kIo};
  std::vector<char> buffer(size);
  ssize_t n = recv(fd, buffer.data(), buffer.size(), 0);
  if (n < 0) {
    LOG(ERROR) << "WifiLan socket recv() failed: " << strerror(errno);
    return {Exception::kIo};
  }
  // n == 0 means the peer closed the connection -- return an empty
  // ByteArray (EOF), matching other backends' Read() contract.
  return ExceptionOr<ByteArray>(ByteArray(buffer.data(), n));
}

Exception WifiLanInputStream::Close() { return {Exception::kSuccess}; }

Exception WifiLanOutputStream::Write(absl::string_view data) {
  int fd = fd_->fd();
  if (fd < 0) return {Exception::kIo};
  size_t written = 0;
  while (written < data.size()) {
    ssize_t n = send(fd, data.data() + written, data.size() - written,
                     MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      LOG(ERROR) << "WifiLan socket send() failed: " << strerror(errno);
      return {Exception::kIo};
    }
    written += n;
  }
  return {Exception::kSuccess};
}

Exception WifiLanOutputStream::Flush() { return {Exception::kSuccess}; }

Exception WifiLanOutputStream::Close() { return {Exception::kSuccess}; }

WifiLanSocket::WifiLanSocket(int fd)
    : fd_(std::make_shared<SharedSocketFd>(fd)),
      input_stream_(fd_),
      output_stream_(fd_) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

WifiLanSocket::~WifiLanSocket() { Close(); }

Exception WifiLanSocket::Close() {
  fd_->Close();
  return {Exception::kSuccess};
}

std::unique_ptr<WifiLanServerSocket> WifiLanServerSocket::Listen(int port) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG(ERROR) << "Failed to create listening socket: " << strerror(errno);
    return nullptr;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  // Accept both IPv4 (via v4-mapped addresses) and IPv6 connections on a
  // single dual-stack socket, same as the common default on Linux.
  int zero = 0;
  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

  struct sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(static_cast<uint16_t>(port));
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    LOG(ERROR) << "Failed to bind WifiLan server socket on port " << port
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
  int bound_port = ntohs(addr.sin6_port);
  if (listen(fd, /*backlog=*/4) != 0) {
    LOG(ERROR) << "Failed to listen on WifiLan server socket: "
               << strerror(errno);
    close(fd);
    return nullptr;
  }
  LOG(INFO) << "WifiLan server socket listening on port " << bound_port;
  // The constructor is private; std::unique_ptr<WifiLanServerSocket> can't
  // use make_unique on it from outside the class, but we're inside.
  return std::unique_ptr<WifiLanServerSocket>(
      new WifiLanServerSocket(fd, bound_port));
}

WifiLanServerSocket::~WifiLanServerSocket() { Close(); }

std::string WifiLanServerSocket::GetIPAddress() const {
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return "";
  std::string result;
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
      continue;
    if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
    auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    result = RawIPv4Bytes(sin->sin_addr);
    break;
  }
  freeifaddrs(ifaddr);
  return result;
}

std::unique_ptr<api::WifiLanSocket> WifiLanServerSocket::Accept() {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
  }
  if (fd < 0) return nullptr;
  int client_fd = accept(fd, nullptr, nullptr);
  if (client_fd < 0) {
    if (errno != EBADF && errno != EINVAL) {
      LOG(ERROR) << "WifiLan accept() failed: " << strerror(errno);
    }
    return nullptr;
  }
  return std::make_unique<WifiLanSocket>(client_fd);
}

Exception WifiLanServerSocket::Close() {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fd = fd_;
    fd_ = -1;
  }
  if (fd >= 0) close(fd);
  return {Exception::kSuccess};
}

WifiLanMedium::~WifiLanMedium() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Best-effort teardown of anything still registered; the Free() calls
  // below are fire-and-forget since the medium is being destroyed anyway.
  for (auto& [type, entry] : advertising_) {
    try {
      entry.entry_group_proxy->callMethod("Free")
          .onInterface(kAvahiEntryGroupInterface);
    } catch (const sdbus::Error&) {
    }
  }
  for (auto& [type, entry] : discovering_) {
    try {
      entry.browser_proxy->callMethod("Free")
          .onInterface(kAvahiServiceBrowserInterface);
    } catch (const sdbus::Error&) {
    }
  }
}

bool WifiLanMedium::IsNetworkConnected() const {
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return false;
  bool connected = false;
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
    if ((ifa->ifa_flags & IFF_UP) == 0 ||
        (ifa->ifa_flags & IFF_RUNNING) == 0) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET ||
        ifa->ifa_addr->sa_family == AF_INET6) {
      connected = true;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return connected;
}

bool WifiLanMedium::StartAdvertising(const NsdServiceInfo& nsd_service_info) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string service_type = nsd_service_info.GetServiceType();
  if (advertising_.contains(service_type)) {
    LOG(WARNING) << "Already advertising WifiLan service type "
                 << service_type;
    return false;
  }
  if (nsd_service_info.GetServiceName().empty() ||
      nsd_service_info.GetPort() == 0) {
    LOG(ERROR) << "Cannot start WifiLan advertising without a service name "
                  "and port.";
    return false;
  }

  std::string avahi_type = StripTrailingDot(service_type);
  auto server_proxy = CreateAvahiProxy(kAvahiServerPath);
  sdbus::ObjectPath group_path;
  try {
    server_proxy->callMethod("EntryGroupNew")
        .onInterface(kAvahiServerInterface)
        .storeResultsTo(group_path);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Avahi EntryGroupNew failed: " << e.what();
    return false;
  }

  auto group_proxy = CreateAvahiProxy(group_path);
  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  std::optional<AvahiEntryGroupState> final_state;

  // Subscribe before Commit() so the ESTABLISHED/COLLISION/FAILURE
  // transition can't be missed between subscribing and committing.
  group_proxy->uponSignal("StateChanged")
      .onInterface(kAvahiEntryGroupInterface)
      .call([&](int32_t state, const std::string& error) {
        auto s = static_cast<AvahiEntryGroupState>(state);
        if (s == AvahiEntryGroupState::kRegistering) return;
        std::lock_guard<std::mutex> wait_lock(wait_mutex);
        final_state = s;
        wait_cv.notify_all();
      });

  try {
    std::vector<std::vector<uint8_t>> txt =
        TxtRecordsToAvahi(nsd_service_info.GetTxtRecords());
    group_proxy->callMethod("AddService")
        .onInterface(kAvahiEntryGroupInterface)
        .withArguments(kAvahiIfUnspec, kAvahiProtoUnspec, uint32_t{0},
                      nsd_service_info.GetServiceName(), avahi_type,
                      std::string(""),  // domain: default
                      std::string(""),  // host: default (this host)
                      static_cast<uint16_t>(nsd_service_info.GetPort()), txt);
    group_proxy->callMethod("Commit").onInterface(kAvahiEntryGroupInterface);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Avahi AddService/Commit failed: " << e.what();
    try {
      group_proxy->callMethod("Free").onInterface(kAvahiEntryGroupInterface);
    } catch (const sdbus::Error&) {
    }
    return false;
  }

  {
    std::unique_lock<std::mutex> wait_lock(wait_mutex);
    wait_cv.wait_for(wait_lock, kEntryGroupCommitTimeout,
                     [&] { return final_state.has_value(); });
  }

  if (!final_state.has_value() ||
      *final_state != AvahiEntryGroupState::kEstablished) {
    LOG(ERROR) << "Failed to establish WifiLan mDNS advertisement for "
               << nsd_service_info.GetServiceName() << " (state="
               << (final_state.has_value()
                       ? static_cast<int>(*final_state)
                       : -1)
               << ")";
    try {
      group_proxy->callMethod("Free").onInterface(kAvahiEntryGroupInterface);
    } catch (const sdbus::Error&) {
    }
    return false;
  }

  LOG(INFO) << "Started WifiLan mDNS advertising: "
            << nsd_service_info.GetServiceName() << "." << avahi_type
            << " on port " << nsd_service_info.GetPort();
  advertising_[service_type] = {group_path, std::move(group_proxy)};
  return true;
}

bool WifiLanMedium::StopAdvertising(const NsdServiceInfo& nsd_service_info) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string service_type = nsd_service_info.GetServiceType();
  auto it = advertising_.find(service_type);
  if (it == advertising_.end()) {
    LOG(WARNING) << "Not advertising WifiLan service type " << service_type;
    return false;
  }
  try {
    it->second.entry_group_proxy->callMethod("Free")
        .onInterface(kAvahiEntryGroupInterface);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Avahi EntryGroup Free failed: " << e.what();
  }
  advertising_.erase(it);
  return true;
}

bool WifiLanMedium::StartDiscovery(const std::string& service_type,
                                   DiscoveredServiceCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (discovering_.contains(service_type)) {
    LOG(WARNING) << "Already discovering WifiLan service type "
                 << service_type;
    return false;
  }

  std::string avahi_type = StripTrailingDot(service_type);
  auto server_proxy = CreateAvahiProxy(kAvahiServerPath);
  sdbus::ObjectPath browser_path;
  try {
    server_proxy->callMethod("ServiceBrowserNew")
        .onInterface(kAvahiServerInterface)
        .withArguments(kAvahiIfUnspec, kAvahiProtoUnspec, avahi_type,
                      std::string(""),  // domain: default ("local")
                      uint32_t{0})
        .storeResultsTo(browser_path);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Avahi ServiceBrowserNew failed: " << e.what();
    return false;
  }

  auto browser_proxy = CreateAvahiProxy(browser_path);
  browser_proxy->uponSignal("ItemNew")
      .onInterface(kAvahiServiceBrowserInterface)
      .call([this, service_type](int32_t interface, int32_t protocol,
                                 const std::string& name,
                                 const std::string& type,
                                 const std::string& domain, uint32_t flags) {
        if ((flags & kAvahiLookupResultLocal) != 0) {
          VLOG(1) << "Ignoring our own locally-registered service " << name;
          return;
        }
        // ResolveService is a synchronous blocking D-Bus call, but we don't
        // expose any D-Bus object of our own for Avahi to call back into, so
        // there's no reentrancy/deadlock concern making it from this signal
        // handler (same reasoning as GattClient::EnsureConnected's plain
        // blocking Connect() call -- see ble_gatt_client.cc).
        auto server_proxy = CreateAvahiProxy(kAvahiServerPath);
        int32_t out_interface, out_protocol, out_aprotocol;
        std::string out_name, out_type, out_domain, out_host, out_address;
        uint16_t out_port;
        std::vector<std::vector<uint8_t>> out_txt;
        uint32_t out_flags;
        try {
          server_proxy->callMethod("ResolveService")
              .onInterface(kAvahiServerInterface)
              .withArguments(interface, protocol, name, type, domain,
                            kAvahiProtoUnspec, uint32_t{0})
              .storeResultsTo(out_interface, out_protocol, out_name, out_type,
                              out_domain, out_host, out_aprotocol,
                              out_address, out_port, out_txt, out_flags);
        } catch (const sdbus::Error& e) {
          VLOG(1) << "Avahi ResolveService failed for " << name << ": "
                  << e.what();
          return;
        }

        NsdServiceInfo nsd_service_info;
        nsd_service_info.SetServiceName(out_name);
        nsd_service_info.SetServiceType(service_type);
        nsd_service_info.SetPort(out_port);
        nsd_service_info.SetTxtRecords(TxtRecordsFromAvahi(out_txt));
        if (out_aprotocol == kAvahiProtoInet) {
          struct in_addr v4addr{};
          if (inet_pton(AF_INET, out_address.c_str(), &v4addr) == 1) {
            nsd_service_info.SetIPAddress(RawIPv4Bytes(v4addr));
          }
        } else if (out_aprotocol == kAvahiProtoInet6) {
          std::string address = out_address;
          // Link-local IPv6 addresses are only meaningful qualified with a
          // zone id; getaddrinfo(AI_NUMERICHOST) understands the
          // "<address>%<ifname>" RFC 4007 form on Linux, so attach it using
          // the interface Avahi told us this was seen on.
          if (address.rfind("fe80:", 0) == 0 && interface > 0) {
            char ifname[IF_NAMESIZE] = {0};
            if (if_indextoname(static_cast<unsigned>(interface), ifname) !=
                nullptr) {
              address = absl::StrCat(address, "%", ifname);
            }
          }
          nsd_service_info.SetIPv6Address(address);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = discovering_.find(service_type);
        if (it == discovering_.end()) return;  // StopDiscovery raced us.
        it->second.callback.service_discovered_cb(nsd_service_info);
      });
  browser_proxy->uponSignal("ItemRemove")
      .onInterface(kAvahiServiceBrowserInterface)
      .call([this, service_type](int32_t /*interface*/, int32_t /*protocol*/,
                                 const std::string& name,
                                 const std::string& /*type*/,
                                 const std::string& /*domain*/,
                                 uint32_t flags) {
        if ((flags & kAvahiLookupResultLocal) != 0) return;
        NsdServiceInfo nsd_service_info;
        nsd_service_info.SetServiceName(name);
        nsd_service_info.SetServiceType(service_type);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = discovering_.find(service_type);
        if (it == discovering_.end()) return;
        it->second.callback.service_lost_cb(nsd_service_info);
      });

  try {
    browser_proxy->callMethod("Start")
        .onInterface(kAvahiServiceBrowserInterface);
  } catch (const sdbus::Error& e) {
    // Some Avahi daemon versions implicitly start browsing as soon as
    // ServiceBrowserNew returns and don't expose/need Start(); treat
    // UnknownMethod as fine, anything else as a real failure.
    if (std::string(e.getName()).find("UnknownMethod") == std::string::npos) {
      LOG(ERROR) << "Avahi ServiceBrowser Start failed: " << e.what();
      return false;
    }
  }

  LOG(INFO) << "Started WifiLan mDNS discovery for " << avahi_type;
  discovering_[service_type] = {browser_path, std::move(browser_proxy),
                                std::move(callback)};
  return true;
}

bool WifiLanMedium::StopDiscovery(const std::string& service_type) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = discovering_.find(service_type);
  if (it == discovering_.end()) {
    LOG(WARNING) << "Not discovering WifiLan service type " << service_type;
    return false;
  }
  try {
    it->second.browser_proxy->callMethod("Free")
        .onInterface(kAvahiServiceBrowserInterface);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Avahi ServiceBrowser Free failed: " << e.what();
  }
  discovering_.erase(it);
  return true;
}

std::unique_ptr<api::WifiLanSocket> WifiLanMedium::ConnectToService(
    const NsdServiceInfo& remote_service_info,
    CancellationFlag* cancellation_flag) {
  std::string ipv4 = remote_service_info.GetIPAddress();
  if (!ipv4.empty() && ipv4.size() == 4) {
    struct in_addr addr{};
    std::memcpy(&addr.s_addr, ipv4.data(), 4);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    ServiceAddress address{.address = {ipv4.begin(), ipv4.end()},
                           .port = static_cast<uint16_t>(
                               remote_service_info.GetPort())};
    auto socket = ConnectToService(address, cancellation_flag);
    if (socket != nullptr) return socket;
  }
  std::string ipv6 = remote_service_info.GetIPv6Address();
  if (!ipv6.empty()) {
    struct sockaddr_storage addr{};
    socklen_t addr_len = 0;
    if (!ResolveNumericAddress(ipv6, remote_service_info.GetPort(), addr,
                               addr_len)) {
      return nullptr;
    }
    if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
      return nullptr;
    }
    int fd =
        ConnectWithTimeout(addr, addr_len, kConnectTimeout, cancellation_flag);
    if (fd < 0) return nullptr;
    LOG(INFO) << "Connected to WifiLan service at [" << ipv6 << "]:"
              << remote_service_info.GetPort();
    return std::make_unique<WifiLanSocket>(fd);
  }
  LOG(ERROR) << "No usable IP address to connect to WifiLan service "
             << remote_service_info.GetServiceName();
  return nullptr;
}

std::unique_ptr<api::WifiLanSocket> WifiLanMedium::ConnectToService(
    const ServiceAddress& service_address,
    CancellationFlag* cancellation_flag) {
  if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
    LOG(INFO) << "WifiLan connect cancelled before starting.";
    return nullptr;
  }
  struct sockaddr_storage addr{};
  socklen_t addr_len;
  if (service_address.address.size() == 4) {
    auto* sin = reinterpret_cast<struct sockaddr_in*>(&addr);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(service_address.port);
    std::memcpy(&sin->sin_addr, service_address.address.data(), 4);
    addr_len = sizeof(*sin);
  } else if (service_address.address.size() == 16) {
    auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(service_address.port);
    std::memcpy(&sin6->sin6_addr, service_address.address.data(), 16);
    addr_len = sizeof(*sin6);
  } else {
    LOG(ERROR) << "ServiceAddress has neither a 4- nor 16-byte address.";
    return nullptr;
  }
  int fd =
      ConnectWithTimeout(addr, addr_len, kConnectTimeout, cancellation_flag);
  if (fd < 0) return nullptr;
  LOG(INFO) << "Connected to WifiLan service at port " << service_address.port;
  return std::make_unique<WifiLanSocket>(fd);
}

std::unique_ptr<api::WifiLanServerSocket> WifiLanMedium::ListenForService(
    int port) {
  return WifiLanServerSocket::Listen(port);
}

api::UpgradeAddressInfo WifiLanMedium::GetUpgradeAddressCandidates(
    const api::WifiLanServerSocket& server_socket) {
  api::UpgradeAddressInfo result;
  uint16_t port = static_cast<uint16_t>(server_socket.GetPort());

  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return result;

  std::vector<ServiceAddress> ipv4_addresses;
  std::vector<ServiceAddress> ipv6_addresses;
  absl::flat_hash_map<std::string, bool> interface_has_ipv4;
  absl::flat_hash_map<std::string, bool> interface_has_ipv6;

  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
    if ((ifa->ifa_flags & IFF_UP) == 0) continue;
    std::string ifname = ifa->ifa_name;

    if (ifa->ifa_addr->sa_family == AF_INET) {
      auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
      std::string raw = RawIPv4Bytes(sin->sin_addr);
      ServiceAddress address{.address = {raw.begin(), raw.end()},
                             .port = port};
      if (address.IsLinkLocalAddress() || address.IsLoopbackAddress())
        continue;
      ipv4_addresses.push_back(address);
      interface_has_ipv4[ifname] = true;
    } else if (ifa->ifa_addr->sa_family == AF_INET6) {
      auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
      std::string raw(reinterpret_cast<const char*>(&sin6->sin6_addr), 16);
      ServiceAddress address{.address = {raw.begin(), raw.end()},
                             .port = port};
      // Link-local IPv6 addresses can't be used for an upgrade candidate
      // sent to a remote peer: there's no way to tell which of the
      // remote's interfaces the zone id would even refer to.
      if (address.IsLinkLocalAddress() || address.IsLoopbackAddress())
        continue;
      ipv6_addresses.push_back(address);
      interface_has_ipv6[ifname] = true;
    }
  }
  freeifaddrs(ifaddr);

  absl::flat_hash_map<std::string, bool> interfaces_seen;
  for (const auto& [ifname, has] : interface_has_ipv4) interfaces_seen[ifname] = true;
  for (const auto& [ifname, has] : interface_has_ipv6) interfaces_seen[ifname] = true;
  result.num_interfaces = static_cast<int>(interfaces_seen.size());
  for (const auto& [ifname, has] : interfaces_seen) {
    if (interface_has_ipv6.contains(ifname) &&
        !interface_has_ipv4.contains(ifname)) {
      result.num_ipv6_only_interfaces++;
    }
  }

  // IPv6 addresses first, IPv4 appended at the end -- matches
  // UpgradeAddressInfo's documented ordering.
  result.address_candidates = std::move(ipv6_addresses);
  result.address_candidates.insert(result.address_candidates.end(),
                                   ipv4_addresses.begin(),
                                   ipv4_addresses.end());
  return result;
}

}  // namespace linux_platform
}  // namespace nearby
