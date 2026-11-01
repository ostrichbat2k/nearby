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

#include "internal/platform/implementation/linux/ble.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/future.h"
#include "internal/platform/implementation/linux/ble_gatt_client.h"
#include "internal/platform/implementation/linux/ble_gatt_server.h"
#include "internal/platform/implementation/linux/ble_socket.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/logging.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/output_stream.h"
#include "internal/platform/pipe.h"
#include "internal/weave/socket_callback.h"
#include "internal/weave/sockets/client_socket.h"

namespace nearby {
namespace linux_platform {

BleMedium::BleMedium() {
  auto adapter_path = BluezSystemBus::Get().FindAdapterPath();
  if (!adapter_path.has_value()) {
    LOG(WARNING) << "No Bluetooth adapter found via BlueZ; BLE will be "
                    "unavailable.";
    return;
  }
  adapter_path_ = *adapter_path;
  adapter_proxy_ = BluezSystemBus::Get().CreateProxy(adapter_path_);
  object_manager_proxy_ = BluezSystemBus::Get().CreateProxy(kBluezRootPath);

  object_manager_proxy_->uponSignal("InterfacesAdded")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath& path,
                    const std::map<std::string,
                                   std::map<std::string, sdbus::Variant>>&
                        interfaces_and_properties) {
        OnDeviceInterfacesAdded(path, interfaces_and_properties);
      });
}

BleMedium::~BleMedium() {
  StopAdvertising();
  StopScanning();
}

void BleMedium::OnDeviceInterfacesAdded(
    const sdbus::ObjectPath& path,
    const std::map<std::string, std::map<std::string, sdbus::Variant>>&
        interfaces_and_properties) {
  auto it = interfaces_and_properties.find(kDevice1Interface);
  if (it == interfaces_and_properties.end()) return;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scanning_) return;
  }

  // Watch this device for future advertisement refreshes (RSSI/ServiceData
  // updates arrive as PropertiesChanged, not further InterfacesAdded).
  std::string device_path(path);
  auto watcher = BluezSystemBus::Get().CreateProxy(device_path);
  watcher->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this, device_path](
                const std::string& interface,
                const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/) {
        OnDevicePropertiesChanged(device_path, interface, changed);
      });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    device_property_watchers_.push_back(std::move(watcher));
  }

  HandleDeviceProperties(device_path, it->second);
}

void BleMedium::OnDevicePropertiesChanged(
    const std::string& device_path, const std::string& interface,
    const std::map<std::string, sdbus::Variant>& changed_properties) {
  if (interface != kDevice1Interface) return;
  HandleDeviceProperties(device_path, changed_properties);
}

void BleMedium::HandleDeviceProperties(
    const std::string& device_path,
    const std::map<std::string, sdbus::Variant>& properties) {
  auto service_data_it = properties.find("ServiceData");
  if (service_data_it == properties.end()) return;

  std::string target_uuid_str;
  absl::AnyInvocable<void(api::ble::BlePeripheral::UniqueId,
                          api::ble::BleAdvertisementData)>*
      cb = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scanning_) return;
    target_uuid_str = ToLowerUuidString(scanning_service_uuid_);
    cb = &advertisement_found_cb_;
  }

  std::map<std::string, sdbus::Variant> service_data =
      service_data_it->second.get<std::map<std::string, sdbus::Variant>>();
  if (service_data.find(target_uuid_str) == service_data.end()) {
    // BlueZ reports every service a device advertises; only report
    // advertisements for the service Nearby actually asked to scan for.
    return;
  }

  MacAddress mac;
  if (!MacAddressFromDevicePath(device_path, mac)) {
    LOG(ERROR) << "Could not parse MAC address from device path: "
               << device_path;
    return;
  }

  api::ble::BleAdvertisementData advertisement_data;
  advertisement_data.is_extended_advertisement = false;
  for (const auto& [uuid_str, variant] : service_data) {
    std::optional<Uuid> uuid = Uuid::FromString(uuid_str);
    if (!uuid.has_value()) continue;
    advertisement_data.service_data[*uuid] =
        FromBytes(variant.get<std::vector<uint8_t>>());
  }

  if (cb != nullptr && *cb) {
    (*cb)(mac.address(), std::move(advertisement_data));
  }
}

bool BleMedium::StartAdvertising(
    const api::ble::BleAdvertisementData& advertising_data,
    api::ble::AdvertiseParameters advertise_set_parameters) {
  if (!adapter_proxy_) return false;
  StopAdvertising();

  auto ad = std::make_unique<Advertisement>();
  ad->object_path = "/com/github/nearby/linux/advertisement" +
                     std::to_string(next_advertisement_id_++);
  ad->type = advertise_set_parameters.is_connectable ? "peripheral"
                                                      : "broadcast";
  for (const auto& [uuid, data] : advertising_data.service_data) {
    std::string uuid_str = ToLowerUuidString(uuid);
    ad->service_uuids.push_back(uuid_str);
    ad->service_data[uuid_str] = sdbus::Variant(ToBytes(data));
  }

  sdbus::IConnection& connection = BluezSystemBus::Get().connection();
  ad->dbus_object =
      sdbus::createObject(connection, sdbus::ObjectPath{ad->object_path});
  ad->dbus_object
      ->addVTable(
          sdbus::registerMethod("Release").implementedAs([]() {}),
          sdbus::registerProperty("Type").withGetter(
              [p = ad.get()]() { return p->type; }),
          sdbus::registerProperty("ServiceUUIDs")
              .withGetter([p = ad.get()]() { return p->service_uuids; }),
          sdbus::registerProperty("ServiceData")
              .withGetter([p = ad.get()]() { return p->service_data; }))
      .forInterface(kLEAdvertisement1Interface);

  try {
    // BlueZ calls GetAll on our LEAdvertisement1 object's properties as
    // part of handling this request, which arrives on this same shared
    // connection -- a blocking callMethod() here would hold up the
    // connection's send path while that incoming call is waiting to be
    // dispatched, deadlocking against ourselves. callMethodAsync + a future
    // avoids that: the wait happens on a plain condition variable, not
    // inside the connection's I/O path.
    std::map<std::string, sdbus::Variant> options;
    auto future = adapter_proxy_->callMethodAsync("RegisterAdvertisement")
                      .onInterface(kLEAdvertisingManager1Interface)
                      .withArguments(sdbus::ObjectPath{ad->object_path},
                                     options)
                      .getResultAsFuture<>();
    future.get();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to register BLE advertisement: " << e.what();
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  advertisement_ = std::move(ad);
  return true;
}

std::unique_ptr<api::ble::BleMedium::AdvertisingSession>
BleMedium::StartAdvertising(
    const api::ble::BleAdvertisementData& advertising_data,
    api::ble::AdvertiseParameters advertise_set_parameters,
    AdvertisingCallback callback) {
  bool success = StartAdvertising(advertising_data, advertise_set_parameters);
  if (callback.start_advertising_result) {
    callback.start_advertising_result(
        success ? absl::OkStatus()
                : absl::InternalError("Failed to register BLE "
                                       "advertisement with BlueZ"));
  }
  if (!success) return nullptr;

  auto session = std::make_unique<AdvertisingSession>();
  session->stop_advertising = [this]() -> absl::Status {
    return StopAdvertising() ? absl::OkStatus()
                              : absl::InternalError(
                                    "Failed to unregister BLE advertisement");
  };
  return session;
}

bool BleMedium::StopAdvertising() {
  std::unique_ptr<Advertisement> ad;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ad = std::move(advertisement_);
  }
  if (!ad) return true;

  bool success = true;
  try {
    // See the comment in StartAdvertising: BlueZ may call Release() on our
    // object as part of unregistering it, so this must be async too.
    auto future = adapter_proxy_->callMethodAsync("UnregisterAdvertisement")
                      .onInterface(kLEAdvertisingManager1Interface)
                      .withArguments(sdbus::ObjectPath{ad->object_path})
                      .getResultAsFuture<>();
    future.get();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to unregister BLE advertisement: " << e.what();
    success = false;
  }
  ad->dbus_object->unregister();
  return success;
}

bool BleMedium::StartScanningInternal(
    const Uuid& service_uuid,
    absl::AnyInvocable<void(api::ble::BlePeripheral::UniqueId,
                            api::ble::BleAdvertisementData)>
        advertisement_found_cb) {
  if (!adapter_proxy_) return false;
  StopScanning();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    scanning_service_uuid_ = service_uuid;
    advertisement_found_cb_ = std::move(advertisement_found_cb);
    scanning_ = true;
  }

  try {
    std::map<std::string, sdbus::Variant> filter;
    filter["Transport"] = sdbus::Variant(std::string("le"));
    filter["DuplicateData"] = sdbus::Variant(true);
    adapter_proxy_->callMethod("SetDiscoveryFilter")
        .onInterface(kAdapter1Interface)
        .withArguments(filter);
    adapter_proxy_->callMethod("StartDiscovery").onInterface(
        kAdapter1Interface);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to start BLE discovery: " << e.what();
    std::lock_guard<std::mutex> lock(mutex_);
    scanning_ = false;
    return false;
  }

  // Devices BlueZ already knew about before this call won't fire a fresh
  // InterfacesAdded signal, so bootstrap from the current object cache too.
  // This must run asynchronously, not inline here: OnDeviceInterfacesAdded
  // can synchronously invoke advertisement_found_cb_, which is owned by the
  // connections-layer BleMedium wrapper that is itself still inside its own
  // StartScanning() call (still holding its own mutex) while this function
  // runs -- replaying already-known devices inline reenters that mutex and
  // self-deadlocks (caught by absl's Mutex deadlock detector, which aborts
  // the process rather than actually hang). Every other platform backend
  // only ever reports scan results asynchronously via a real OS scan
  // callback, so this reentrant-during-setup case is Linux-specific.
  bootstrap_thread_ = std::thread(
      [this, objects = BluezSystemBus::Get().GetManagedObjects()]() {
        for (const auto& [path, interfaces] : objects) {
          OnDeviceInterfacesAdded(path, interfaces);
        }
      });

  return true;
}

bool BleMedium::StartScanning(const Uuid& service_uuid,
                              api::ble::TxPowerLevel tx_power_level,
                              ScanCallback callback) {
  return StartScanningInternal(
      service_uuid,
      [cb = std::move(callback)](api::ble::BlePeripheral::UniqueId id,
                                  api::ble::BleAdvertisementData data) mutable {
        if (cb.advertisement_found_cb) {
          cb.advertisement_found_cb(id, std::move(data));
        }
      });
}

std::unique_ptr<api::ble::BleMedium::ScanningSession> BleMedium::StartScanning(
    const Uuid& service_uuid, api::ble::TxPowerLevel tx_power_level,
    ScanningCallback callback) {
  auto shared_callback =
      std::make_shared<ScanningCallback>(std::move(callback));
  // TODO: BlueZ doesn't give a clean "advertisement lost" signal (it purges
  // stale Device1 objects on its own schedule, not tied to our scan
  // session), so advertisement_lost_cb is never invoked yet.
  bool success = StartScanningInternal(
      service_uuid,
      [shared_callback](api::ble::BlePeripheral::UniqueId id,
                         api::ble::BleAdvertisementData data) {
        if (shared_callback->advertisement_found_cb) {
          shared_callback->advertisement_found_cb(id, std::move(data));
        }
      });
  if (shared_callback->start_scanning_result) {
    shared_callback->start_scanning_result(
        success ? absl::OkStatus()
                : absl::InternalError("Failed to start BLE discovery"));
  }
  if (!success) return nullptr;

  auto session = std::make_unique<ScanningSession>();
  session->stop_scanning = [this]() -> absl::Status {
    return StopScanning() ? absl::OkStatus()
                           : absl::InternalError("Failed to stop BLE "
                                                  "discovery");
  };
  return session;
}

std::unique_ptr<api::ble::GattServer> BleMedium::StartGattServer(
    api::ble::ServerGattConnectionCallback callback) {
  return std::make_unique<GattServer>(std::move(callback));
}

std::unique_ptr<api::ble::GattClient> BleMedium::ConnectToGattServer(
    api::ble::BlePeripheral::UniqueId peripheral_id,
    api::ble::TxPowerLevel tx_power_level,
    api::ble::ClientGattConnectionCallback callback) {
  if (adapter_path_.empty()) return nullptr;

  MacAddress mac;
  if (!MacAddress::FromUint64(peripheral_id, mac)) {
    LOG(ERROR) << "ConnectToGattServer: invalid peripheral id "
               << peripheral_id;
    return nullptr;
  }
  std::string device_path = DevicePathForMacAddress(adapter_path_, mac);
  return std::make_unique<GattClient>(device_path, std::move(callback));
}

std::unique_ptr<api::ble::BleServerSocket> BleMedium::OpenServerSocket(
    const std::string& service_id) {
  auto server_socket = std::make_unique<BleServerSocket>();
  BleServerSocket* server_socket_raw = server_socket.get();

  api::ble::ServerGattConnectionCallback gatt_callback;
  gatt_callback.on_characteristic_write_cb =
      [server_socket_raw](
          api::ble::BlePeripheral::UniqueId remote_device_id,
          const api::ble::GattCharacteristic& characteristic, int offset,
          absl::string_view data,
          api::ble::ServerGattConnectionCallback::WriteValueCallback
              write_done) {
        server_socket_raw->OnCharacteristicWrite(remote_device_id,
                                                 characteristic, data);
        write_done(absl::OkStatus());
      };

  std::unique_ptr<api::ble::GattServer> gatt_server =
      StartGattServer(std::move(gatt_callback));
  if (gatt_server == nullptr) {
    LOG(ERROR) << "OpenServerSocket(" << service_id
              << "): failed to start the GATT server.";
    return nullptr;
  }

  std::optional<api::ble::GattCharacteristic> write_characteristic =
      gatt_server->CreateCharacteristic(
          WeaveServiceUuid(), WeaveToPeripheralCharacteristicUuid(),
          api::ble::GattCharacteristic::Permission::kWrite,
          api::ble::GattCharacteristic::Property::kWrite, ByteArray());
  std::optional<api::ble::GattCharacteristic> notify_characteristic =
      gatt_server->CreateCharacteristic(
          WeaveServiceUuid(), WeaveFromPeripheralCharacteristicUuid(),
          api::ble::GattCharacteristic::Permission::kRead,
          api::ble::GattCharacteristic::Property::kNotify, ByteArray());
  if (!write_characteristic.has_value() ||
      !notify_characteristic.has_value()) {
    LOG(ERROR) << "OpenServerSocket(" << service_id
              << "): failed to create the Weave GATT characteristics.";
    gatt_server->Stop();
    return nullptr;
  }

  server_socket_raw->Initialize(std::move(gatt_server), *write_characteristic,
                                *notify_characteristic);
  LOG(INFO) << "OpenServerSocket(" << service_id
           << "): Weave GATT server socket ready.";
  return std::move(server_socket);
}

std::unique_ptr<api::ble::BleSocket> BleMedium::Connect(
    const std::string& service_id, api::ble::TxPowerLevel tx_power_level,
    api::ble::BlePeripheral::UniqueId peripheral_id,
    CancellationFlag* cancellation_flag) {
  auto connection = std::make_unique<GattClientWeaveConnection>();
  GattClientWeaveConnection* connection_raw = connection.get();

  api::ble::ClientGattConnectionCallback gatt_callback;
  gatt_callback.on_characteristic_changed_cb =
      [connection_raw](absl::string_view value) {
        connection_raw->OnNotifyReceived(value);
      };
  gatt_callback.disconnected_cb = [connection_raw]() {
    connection_raw->OnGattDisconnected();
  };

  std::unique_ptr<api::ble::GattClient> gatt_client = ConnectToGattServer(
      peripheral_id, tx_power_level, std::move(gatt_callback));
  if (gatt_client == nullptr) {
    LOG(ERROR) << "Connect(" << service_id << "): failed to connect to peripheral "
              << peripheral_id << "'s GATT server.";
    return nullptr;
  }

  if (!gatt_client->DiscoverServiceAndCharacteristics(
          WeaveServiceUuid(), {WeaveToPeripheralCharacteristicUuid(),
                              WeaveFromPeripheralCharacteristicUuid()})) {
    LOG(ERROR) << "Connect(" << service_id
              << "): failed to discover the Weave GATT characteristics on "
                 "peripheral "
              << peripheral_id;
    return nullptr;
  }
  std::optional<api::ble::GattCharacteristic> write_characteristic =
      gatt_client->GetCharacteristic(WeaveServiceUuid(),
                                     WeaveToPeripheralCharacteristicUuid());
  std::optional<api::ble::GattCharacteristic> notify_characteristic =
      gatt_client->GetCharacteristic(WeaveServiceUuid(),
                                     WeaveFromPeripheralCharacteristicUuid());
  if (!write_characteristic.has_value() ||
      !notify_characteristic.has_value()) {
    LOG(ERROR) << "Connect(" << service_id
              << "): Weave GATT characteristics missing on peripheral "
              << peripheral_id;
    return nullptr;
  }
  connection_raw->SetGattClient(std::move(gatt_client), *write_characteristic,
                                *notify_characteristic);

  auto [pipe_input, pipe_output] = CreatePipe();
  std::shared_ptr<OutputStream> shared_pipe_output(std::move(pipe_output));
  nearby::Future<bool> handshake_done;

  auto weave_socket = std::make_unique<weave::ClientSocket>(
      *connection_raw,
      weave::SocketCallback{
          .on_connected_cb = [handshake_done]() mutable {
            handshake_done.Set(true);
          },
          .on_disconnected_cb = [handshake_done]() mutable {
            handshake_done.Set(false);
          },
          .on_receive_cb = [shared_pipe_output](std::string message) {
            shared_pipe_output->Write(message);
          },
          .on_error_cb = [handshake_done](absl::Status status) mutable {
            LOG(WARNING) << "Weave client socket error: " << status;
            handshake_done.Set(false);
          },
      });
  weave_socket->Connect();

  ExceptionOr<bool> connected = handshake_done.Get(absl::Seconds(10));
  if (!connected.ok() || !connected.result()) {
    LOG(ERROR) << "Connect(" << service_id
              << "): Weave handshake with peripheral " << peripheral_id
              << " failed or timed out.";
    return nullptr;
  }

  LOG(INFO) << "Connect(" << service_id << "): Weave socket to peripheral "
           << peripheral_id << " connected.";
  return std::make_unique<BleSocket>(std::move(connection),
                                     std::move(weave_socket), peripheral_id,
                                     std::move(pipe_input));
}

bool BleMedium::StopScanning() {
  bool was_scanning;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_scanning = scanning_;
    scanning_ = false;
    advertisement_found_cb_ = nullptr;
  }
  // Not holding mutex_ here: bootstrap_thread_ (if still running) takes it
  // itself via OnDeviceInterfacesAdded, and will see scanning_ already
  // false above and return quickly. Always join (not just when
  // was_scanning) so this object is never destroyed out from under it --
  // StartScanningInternal calls StopScanning() first too, which is what
  // serializes against a still-running bootstrap from a prior scan.
  if (bootstrap_thread_.joinable()) bootstrap_thread_.join();
  if (!was_scanning || !adapter_proxy_) return true;

  try {
    adapter_proxy_->callMethod("StopDiscovery").onInterface(
        kAdapter1Interface);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to stop BLE discovery: " << e.what();
    return false;
  }
  return true;
}

}  // namespace linux_platform
}  // namespace nearby
