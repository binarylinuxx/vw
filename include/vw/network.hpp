#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// NetworkManager on the system bus: connection state, Wi-Fi networks and
// basic control. State is kept fresh by a background thread; pass
// event_fd()/drain_events() to vw::App::on_event to rebuild on changes.
namespace vw::network {

enum class Connectivity : std::uint32_t { unknown, none, portal, limited, full };
enum class Kind { none, wired, wifi, vpn, other };

struct Status {
  bool available = false;
  bool networking_enabled = false;
  bool wifi_enabled = false;
  // False while a hardware or rfkill switch blocks Wi-Fi.
  bool wifi_hardware_enabled = false;
  // The machine has a Wi-Fi device at all.
  bool has_wifi = false;
  // An Ethernet device is connected.
  bool wired = false;
  Connectivity connectivity = Connectivity::unknown;
  // Type, name and first IPv4 address ("192.168.1.20/24") of the primary
  // connection.
  Kind primary = Kind::none;
  std::string connection;
  std::string address;
  // The connected Wi-Fi network, when there is one.
  std::string ssid;
  // 0 to 100.
  std::uint8_t strength = 0;
};

struct AccessPoint {
  std::string ssid;
  std::string bssid;
  // 0 to 100.
  std::uint8_t strength = 0;
  // MHz.
  std::uint32_t frequency = 0;
  bool secured = false;
  bool active = false;
  // A saved connection exists, so connect() needs no password.
  bool saved = false;
};

Status status();
// Visible networks, one per SSID (the strongest), active first then by
// strength. Hidden networks are left out.
std::vector<AccessPoint> access_points();
// Freedesktop icon name for the status, e.g.
// "network-wireless-signal-good-symbolic".
std::string icon_name(const Status& status);

bool set_wifi_enabled(bool enabled);
// Asks the Wi-Fi device for a fresh scan; results arrive as events.
bool scan();
// Activates the saved connection for ssid, or creates one (WPA-PSK when a
// password is given). Returns once NetworkManager accepted the request;
// whether it connected shows up in status().
bool connect(std::string_view ssid, std::string_view password = {});
bool disconnect_wifi();

int event_fd();
bool drain_events();

}  // namespace vw::network
