#include "vw/zwwm.hpp"

#include <ext-zwwm-manager-v1-zwayland-client.h>
#include <zwwm-tags-unstable-v1-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>
#include <zwayland/client/display.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vw::zwwm {
namespace {

using Json = nlohmann::json;
namespace generated = zwayland::generated;
using zwayland::client::Proxy;

std::vector<std::uint32_t> decode_tags(std::span<const std::byte> bytes) {
  if (bytes.size() % sizeof(std::uint32_t) != 0)
    throw std::runtime_error("invalid zwwm tag array size");
  std::vector<std::uint32_t> tags(bytes.size() / sizeof(std::uint32_t));
  if (!bytes.empty()) std::memcpy(tags.data(), bytes.data(), bytes.size());
  return tags;
}

struct SnapshotReader {
  Json data = Json::array();
  std::uint32_t type = 0;
  bool done = false;
  std::string error;

  void observe(Proxy& proxy, std::uint32_t opcode, zwayland::wire::MessageParser& parser) {
    (void)proxy;
    switch (opcode) {
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_STATUS_OPCODE:
        data = Json{{"protocol_version", parser.read_uint()}, {"outputs", parser.read_uint()},
                    {"toplevels", parser.read_uint()}, {"layers", parser.read_uint()}};
        break;
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_OUTPUT_OPCODE: {
        Json output{{"connector", std::string(parser.read_string())},
                    {"id_hi", parser.read_uint()}, {"id_lo", parser.read_uint()},
                    {"x", parser.read_int()}, {"y", parser.read_int()},
                    {"logical_width", parser.read_uint()}, {"logical_height", parser.read_uint()},
                    {"physical_width", parser.read_uint()}, {"physical_height", parser.read_uint()},
                    {"refresh_millihz", parser.read_uint()}, {"scale_per_mille", parser.read_uint()},
                    {"transform", parser.read_uint()}, {"bit_depth", parser.read_uint()},
                    {"enabled", parser.read_uint()}, {"mode_count", parser.read_uint()},
                    {"modes", Json::array()}};
        output["scale"] = output["scale_per_mille"].get<double>() / 1000.0;
        data.push_back(std::move(output));
        break;
      }
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_MODE_OPCODE: {
        Json mode{{"width", parser.read_uint()}, {"height", parser.read_uint()},
                  {"refresh_millihz", parser.read_uint()}, {"preferred", parser.read_uint()}};
        if (data.is_array() && !data.empty()) data.back()["modes"].push_back(std::move(mode));
        break;
      }
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_TOPLEVEL_OPCODE:
        (type == generated::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LIVE
            ? data["toplevels"] : data).push_back(Json{{"id_hi", parser.read_uint()}, {"id_lo", parser.read_uint()},
                            {"app_id", std::string(parser.read_string())},
                            {"title", std::string(parser.read_string())},
                            {"output_hi", parser.read_uint()}, {"output_lo", parser.read_uint()},
                            {"x", parser.read_int()}, {"y", parser.read_int()},
                            {"width", parser.read_int()}, {"height", parser.read_int()},
                            {"state", parser.read_uint()}});
        break;
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_TAG_OPCODE:
        (type == generated::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LIVE
            ? data["tags"] : data).push_back(Json{{"connector", std::string(parser.read_string())},
                            {"output_hi", parser.read_uint()}, {"output_lo", parser.read_uint()},
                            {"active", parser.read_uint()}});
        break;
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_LAYER_OPCODE:
        data.push_back(Json{{"id_hi", parser.read_uint()}, {"id_lo", parser.read_uint()},
                            {"output_hi", parser.read_uint()}, {"output_lo", parser.read_uint()},
                            {"namespace", std::string(parser.read_string())},
                            {"layer", parser.read_uint()}, {"priority", parser.read_int()},
                            {"exclusive_zone", parser.read_int()}, {"blur_radius", parser.read_uint()},
                            {"opacity_millionths", parser.read_uint()}, {"mapped", parser.read_uint()}});
        break;
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_CAMERA_OPCODE:
        (type == generated::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LIVE
            ? data["cameras"] : data).push_back(Json{{"connector", std::string(parser.read_string())},
                            {"output_hi", parser.read_uint()}, {"output_lo", parser.read_uint()},
                            {"tag", parser.read_uint()}, {"x", std::string(parser.read_string())},
                            {"y", std::string(parser.read_string())},
                            {"zoom", std::string(parser.read_string())}, {"active", parser.read_uint()}});
        break;
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_KEYBOARD_OPCODE:
        (type == generated::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LIVE
            ? data["keyboard"] : data).push_back(Json{{"layout", std::string(parser.read_string())},
                            {"group", parser.read_uint()}});
        break;
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_DONE_OPCODE:
        done = true;
        break;
      case generated::ZWAYLAND_EXT_ZWWM_SNAPSHOT_V1_EVENT_FAILED_OPCODE:
        error = std::string(parser.read_string());
        done = true;
        break;
      default:
        throw std::runtime_error("unexpected zwwm snapshot event");
    }
  }
};


class Service {
 public:
  Service() : display_(zwayland::client::Display::connect()) {
    if (!display_) return;
    Proxy* registry = zwayland::client::core::get_registry(*display_);
    if (registry == nullptr) throw std::runtime_error("could not create zwwm registry");
    registry->set_observer(generated::wl_registry_observer(Registry{this}));
    check(display_->roundtrip());
    if (manager_name_ != 0) {
      manager_ = zwayland::client::core::bind(*display_, *registry, manager_name_,
          generated::ext_zwwm_manager_v1_interface,
          std::min(manager_version_, generated::ext_zwwm_manager_v1_interface.version));
      if (manager_ == nullptr) throw std::runtime_error("could not bind zwwm manager");
      const std::uint32_t mask = manager_->version >= 6 ? 0x1ffU : 0x7fU;
      const auto id = generated::ext_zwwm_manager_v1_subscribe(*display_, manager_->id, mask);
      subscription_ = display_->find_proxy(id);
      if (subscription_ != nullptr) {
        subscription_->set_observer([this](Proxy&, std::uint32_t opcode,
                                          zwayland::wire::MessageParser& parser) {
          if (opcode == generated::ZWAYLAND_EXT_ZWWM_SUBSCRIPTION_V1_EVENT_EVENT_OPCODE) {
            last_event_ = std::string(parser.read_string());
            dirty_ = true;
          }
        });
      }
    }
    if (tags_name_ != 0) {
      tags_ = zwayland::client::core::bind(*display_, *registry, tags_name_,
                                          generated::zwwm_tags_v1_interface, 1);
      if (tags_ == nullptr) throw std::runtime_error("could not bind zwwm tags");
      tags_->set_observer(generated::zwwm_tags_v1_observer(TagEvents{this}));
    }
    check(display_->roundtrip());
    dirty_ = false;
  }

  bool available() const { return manager_ != nullptr || tags_ != nullptr; }
  bool has_manager() const { return manager_ != nullptr; }
  bool has_tags() const { return tags_ != nullptr; }
  int fd() const { return available() ? display_->fd() : -1; }
  bool drain() {
    if (!available()) return false;
    check(display_->dispatch_pending());
    return std::exchange(dirty_, false);
  }
  const std::map<std::uint32_t, std::string>& tags() const { return tag_names_; }
  const std::vector<std::uint32_t>& active_tags() const { return active_tags_; }
  const std::string& last_event() const { return last_event_; }

  Json snapshot(std::uint32_t type) {
    require_manager();
    if (type == generated::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LIVE && manager_->version < 6)
      throw std::runtime_error("zwwm live snapshots require manager version 6");
    SnapshotReader result;
    result.type = type;
    if (type == generated::EXT_ZWWM_MANAGER_V1_SNAPSHOT_TYPE_LIVE)
      result.data = Json{{"toplevels", Json::array()}, {"tags", Json::array()},
                         {"cameras", Json::array()}, {"keyboard", Json::array()}};
    const auto id = generated::ext_zwwm_manager_v1_get_snapshot(*display_, manager_->id, type);
    Proxy* proxy = display_->find_proxy(id);
    proxy->set_observer([&result](Proxy& object, std::uint32_t opcode,
                                  zwayland::wire::MessageParser& parser) {
      result.observe(object, opcode, parser);
    });
    check(display_->roundtrip());
    generated::ext_zwwm_snapshot_v1_destroy(*display_, id);
    if (!result.error.empty()) throw std::runtime_error(result.error);
    if (!result.done) throw std::runtime_error("zwwm snapshot did not complete");
    return result.data;
  }

  Result result(std::uint32_t id) {
    Result result;
    bool done = false;
    Proxy* proxy = display_->find_proxy(id);
    proxy->set_observer(generated::ext_zwwm_result_v1_observer(ResultEvents{&result, &done}));
    check(display_->roundtrip());
    generated::ext_zwwm_result_v1_destroy(*display_, id);
    if (!done) throw std::runtime_error("zwwm action did not complete");
    return result;
  }

  Result dispatch(std::string_view action, std::string_view argument) {
    require_manager();
    return result(generated::ext_zwwm_manager_v1_dispatch(
        *display_, manager_->id, action, argument));
  }
  Result reload() {
    require_manager();
    return result(generated::ext_zwwm_manager_v1_reload(*display_, manager_->id));
  }
  Result rebuild_switch_shaders() {
    require_manager();
    if (manager_->version < 2) throw std::runtime_error("zwwm shader rebuild requires manager version 2");
    return result(generated::ext_zwwm_manager_v1_rebuild_switch_shaders(
        *display_, manager_->id));
  }
  Result set_cursor(std::string_view theme, std::uint32_t size) {
    require_manager();
    if (manager_->version < 3) throw std::runtime_error("zwwm cursor control requires manager version 3");
    return result(generated::ext_zwwm_manager_v1_set_cursor(
        *display_, manager_->id, theme, size));
  }
  void set_active_tags(const std::vector<std::uint32_t>& ids) {
    if (tags_ == nullptr) throw std::runtime_error("zwwm tags protocol is unavailable");
    const auto* bytes = reinterpret_cast<const std::byte*>(ids.data());
    generated::zwwm_tags_v1_set_active_tags(*display_, tags_->id,
                                            {bytes, ids.size() * sizeof(std::uint32_t)});
    check(display_->roundtrip());
  }

 private:
  struct Registry {
    Service* self;
    void global(Proxy&, std::uint32_t name, const std::string& interface,
                std::uint32_t version) {
      if (interface == generated::ext_zwwm_manager_v1_interface.name) {
        self->manager_name_ = name;
        self->manager_version_ = version;
      } else if (interface == generated::zwwm_tags_v1_interface.name) {
        self->tags_name_ = name;
      }
    }
    void global_remove(Proxy&, std::uint32_t name) {
      if (name == self->manager_name_ || name == self->tags_name_) self->dirty_ = true;
    }
  };
  struct TagEvents {
    Service* self;
    void tag(Proxy&, std::uint32_t id, const std::string& name) {
      self->pending_tags_[id] = name;
    }
    void tag_removed(Proxy&, std::uint32_t id) {
      self->tag_names_.erase(id);
      self->pending_tags_.erase(id);
    }
    void done(Proxy&) {
      self->tag_names_ = std::move(self->pending_tags_);
      self->pending_tags_.clear();
      self->dirty_ = true;
    }
    void active_tags(Proxy&, std::span<const std::byte> tags) {
      self->active_tags_ = decode_tags(tags);
      self->dirty_ = true;
    }
    void toplevel_tags(Proxy&, Proxy*, std::span<const std::byte>) { self->dirty_ = true; }
    void toplevel_closed(Proxy&, Proxy*) { self->dirty_ = true; }
  };
  struct ResultEvents {
    Result* result;
    bool* done;
    void succeeded(Proxy&, const std::string& message) {
      *done = true;
      result->success = true;
      result->message = message;
    }
    void failed(Proxy&, const std::string& message) {
      *done = true;
      result->message = message;
    }
  };
  void require_manager() const {
    if (manager_ == nullptr) throw std::runtime_error("zwwm manager protocol is unavailable");
  }
  void check(bool succeeded) const {
    if (!succeeded || display_->has_error())
      throw std::runtime_error(display_->has_error() ? display_->error_message()
                                                       : "zwwm Wayland connection failed");
  }

  std::unique_ptr<zwayland::client::Display> display_;
  std::uint32_t manager_name_ = 0;
  std::uint32_t manager_version_ = 0;
  std::uint32_t tags_name_ = 0;
  Proxy* manager_ = nullptr;
  Proxy* tags_ = nullptr;
  Proxy* subscription_ = nullptr;
  std::map<std::uint32_t, std::string> tag_names_;
  std::map<std::uint32_t, std::string> pending_tags_;
  std::vector<std::uint32_t> active_tags_;
  std::string last_event_;
  bool dirty_ = false;
};

Service& service() {
  static Service instance;
  return instance;
}

void require_tags() {
  if (!service().has_tags()) throw std::runtime_error("zwwm tags protocol is unavailable");
}

}  // namespace

bool available() {
  try {
    return service().available();
  } catch (...) {
    return false;
  }
}

std::vector<Tag> tags() {
  require_tags();
  const auto& active = service().active_tags();
  std::vector<Tag> result;
  for (const auto& [id, name] : service().tags())
    result.push_back({id, name, std::ranges::find(active, id) != active.end()});
  return result;
}

std::vector<std::uint32_t> active_tags() {
  require_tags();
  return service().active_tags();
}

void set_active_tags(const std::vector<std::uint32_t>& ids) { service().set_active_tags(ids); }
std::string last_event() { return service().last_event(); }

nlohmann::json snapshot(Snapshot type) { return service().snapshot(static_cast<std::uint32_t>(type)); }
Result dispatch(std::string_view action, std::string_view argument) {
  return service().dispatch(action, argument);
}
Result reload() { return service().reload(); }
Result rebuild_switch_shaders() { return service().rebuild_switch_shaders(); }
Result set_cursor(std::string_view theme, std::uint32_t size) { return service().set_cursor(theme, size); }

int event_fd() {
  try {
    return service().fd();
  } catch (...) {
    return -1;
  }
}

bool drain_events() {
  try {
    return service().drain();
  } catch (...) {
    return false;
  }
}

}  // namespace vw::zwwm
