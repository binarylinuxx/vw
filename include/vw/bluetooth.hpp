#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// BlueZ on the system bus: adapters, devices and their batteries. State is
// kept fresh by a background thread; pass event_fd()/drain_events() to
// vw::App::on_event to rebuild on changes.
namespace vw::bluetooth {

struct Adapter {
  // D-Bus object path, e.g. "/org/bluez/hci0".
  std::string path;
  std::string name;
  std::string address;
  bool powered = false;
  bool discovering = false;
  bool discoverable = false;
};

struct Device {
  std::string path;
  std::string adapter;
  std::string name;
  std::string address;
  // Freedesktop icon name from BlueZ, e.g. "audio-headset".
  std::string icon;
  bool paired = false;
  bool trusted = false;
  bool connected = false;
  // A connect, disconnect or pair request from this process is in flight.
  bool busy = false;
  // 0 to 100, when the device reports it.
  std::optional<std::uint8_t> battery;
};

bool available();
// The first adapter, if any.
std::optional<Adapter> adapter();
std::vector<Adapter> adapters();
// Connected first, then paired, then by name. Unnamed devices found by
// discovery are left out.
std::vector<Device> devices();

// Act on the first adapter.
bool set_powered(bool powered);
bool set_discovering(bool discovering);

// Device actions take a Device::path and return once the request is sent;
// progress shows up as Device::busy and connected/paired. A failure is
// reported by last_error().
bool connect(const std::string& device);
bool disconnect(const std::string& device);
// Pairs, then trusts and connects. Devices needing a PIN or confirmation
// need an agent, which vw does not provide.
bool pair(const std::string& device);
bool forget(const std::string& device);
std::string last_error();

int event_fd();
bool drain_events();

}  // namespace vw::bluetooth
