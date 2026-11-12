#include "vw/pipewire.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/eventfd.h>
#include <unistd.h>

namespace vw::pipewire {
namespace {
using AudioNode = Node;

bool sink(const AudioNode& node) { return node.media_class == "Audio/Sink"; }
bool source(const AudioNode& node) { return node.media_class.starts_with("Audio/Source"); }
double linear_volume(const AudioNode& node) { return node.linear_volume(); }

class Pipewire {
  struct Bound {
    Pipewire* self;
    pw_node* proxy = nullptr;
    spa_hook listener{};
    AudioNode state;
    bool writable = false;
  };
  struct Metadata { pw_metadata* proxy = nullptr; spa_hook listener{}; };
  struct LoopLock {
    pw_thread_loop* loop;
    explicit LoopLock(pw_thread_loop* loop) : loop(loop) { pw_thread_loop_lock(loop); }
    ~LoopLock() { pw_thread_loop_unlock(loop); }
  };

 public:
  Pipewire() : event_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (event_ < 0) throw std::runtime_error("could not create PipeWire event fd");
  }
  ~Pipewire() { cleanup(); close(event_); }
  int fd() const { return event_; }
  bool drain() {
    std::uint64_t count = 0;
    bool changed = false;
    while (read(event_, &count, sizeof(count)) > 0) changed = true;
    return changed;
  }
  bool start() {
    std::lock_guard init(initialization_);
    if (attempted_) return connected_.load();
    attempted_ = true;
    try {
      static std::once_flag initialized;
      std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
      loop_ = pw_thread_loop_new("vw-pipewire", nullptr);
      if (loop_ == nullptr) throw std::runtime_error("could not create PipeWire loop");
      context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
      if (context_ == nullptr) throw std::runtime_error("could not create PipeWire context");
      core_ = pw_context_connect(context_, pw_properties_new(
          PW_KEY_APP_NAME, "vw", PW_KEY_APP_ID, "vw", nullptr), 0);
      if (core_ == nullptr) throw std::runtime_error(std::string("PipeWire connection failed: ") + std::strerror(errno));
      static const pw_core_events core_events = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = [](void* data, std::uint32_t id, int sequence) {
          auto* self = static_cast<Pipewire*>(data);
          if (id == PW_ID_CORE) {
            self->completed_ = sequence;
            pw_thread_loop_signal(self->loop_, false);
          }
        };
        events.error = [](void* data, std::uint32_t id, int, int result, const char* message) {
          auto* self = static_cast<Pipewire*>(data);
          self->error_ = message == nullptr ? "PipeWire error" : message;
          if (id == PW_ID_CORE && (result == -EPIPE || result == -ECONNRESET || result == -ENOTCONN))
            self->connected_.store(false);
          pw_thread_loop_signal(self->loop_, false);
          self->wake();
        };
        return events;
      }();
      pw_core_add_listener(core_, &core_listener_, &core_events, this);
      registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
      if (registry_ == nullptr) throw std::runtime_error("could not get PipeWire registry");
      static const pw_registry_events registry_events = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = global;
        events.global_remove = removed;
        return events;
      }();
      pw_registry_add_listener(registry_, &registry_listener_, &registry_events, this);
      connected_.store(true);
      if (pw_thread_loop_start(loop_) < 0) throw std::runtime_error("could not start PipeWire loop");
      loop_started_ = true;
      {
        LoopLock lock(loop_);
        timespec deadline{};
        pw_thread_loop_get_time(loop_, &deadline, 1500000000);
        // Registry enumeration, node binding, then subscribed properties.
        for (int round = 0; round < 3; ++round) {
          const int sequence = pw_core_sync(core_, PW_ID_CORE, 0);
          if (sequence < 0) throw std::runtime_error("PipeWire registry synchronization failed");
          while (completed_ != sequence && connected_.load()) {
            if (pw_thread_loop_timed_wait_full(loop_, &deadline) < 0)
              throw std::runtime_error("PipeWire registry synchronization timed out");
          }
          if (!connected_.load()) throw std::runtime_error(error_);
        }
      }
      return true;
    } catch (const std::exception& error) {
      const std::string message = error.what();
      cleanup();
      error_ = message;
      return false;
    }
  }
  std::string error() {
    (void)start();
    if (loop_ == nullptr) return error_;
    LoopLock lock(loop_);
    return error_;
  }
  std::vector<AudioNode> nodes(int filter = 0) {
    if (!start()) return {};
    LoopLock lock(loop_);
    std::vector<AudioNode> result;
    for (const auto& [id, node] : nodes_) {
      if (filter == 1 && !sink(node->state)) continue;
      if (filter == 2 && !source(node->state)) continue;
      if (filter == 3 && !node->state.media_class.starts_with("Stream/")) continue;
      result.push_back(node->state);
    }
    return result;
  }
  std::optional<AudioNode> default_node(bool input) {
    if (!start()) return {};
    LoopLock lock(loop_);
    const auto& name = input ? default_source_ : default_sink_;
    for (const auto& [id, node] : nodes_)
      if (node->state.name == name && (input ? source(node->state) : sink(node->state)))
        return node->state;
    if (name.empty()) for (const auto& [id, node] : nodes_)
      if (input ? source(node->state) : sink(node->state)) return node->state;
    return {};
  }
  std::pair<bool, std::string> set_volume(std::uint32_t id, double volume) {
    if (!start()) return {false, error()};
    LoopLock lock(loop_);
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) return {false, "audio node was not found"};
    const auto& node = found->second;
    if (!node->state.can_control || !node->state.volume_known)
      return {false, "audio node has no writable volume controls"};
    auto channels = node->state.channel_volumes;
    if (channels.empty()) channels.push_back(0);
    const double target = volume * volume * volume;
    const double old = linear_volume(node->state);
    for (auto& channel : channels)
      channel = static_cast<float>(old > 0.000001 ? channel * target / old : target);
    alignas(8) std::array<std::byte, 4096> data{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, data.data(), data.size());
    spa_pod_frame frame{};
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&builder, sizeof(float), SPA_TYPE_Float, channels.size(), channels.data());
    auto* parameter = static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &frame));
    if (parameter == nullptr) return {false, "could not encode volume parameters"};
    const int status = pw_node_set_param(node->proxy, SPA_PARAM_Props, 0, parameter);
    return {status >= 0, status < 0 ? std::strerror(-status) : ""};
  }
  std::pair<bool, std::string> set_mute(std::uint32_t id, std::optional<bool> mute) {
    if (!start()) return {false, error()};
    LoopLock lock(loop_);
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) return {false, "audio node was not found"};
    const auto& node = found->second;
    if (!node->state.can_control) return {false, "audio node has no writable mute controls"};
    alignas(8) std::array<std::byte, 128> data{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, data.data(), data.size());
    spa_pod_frame frame{};
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&builder, mute.value_or(!node->state.muted));
    auto* parameter = static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &frame));
    if (parameter == nullptr) return {false, "could not encode mute parameters"};
    const int status = pw_node_set_param(node->proxy, SPA_PARAM_Props, 0, parameter);
    return {status >= 0, status < 0 ? std::strerror(-status) : ""};
  }
  std::pair<bool, std::string> set_default(std::uint32_t id, bool input) {
    if (!start()) return {false, error()};
    LoopLock lock(loop_);
    const auto node = nodes_.find(id);
    if (node == nodes_.end() || !(input ? source(node->second->state) : sink(node->second->state)))
      return {false, "audio node is not a matching device"};
    if (metadata_.empty()) return {false, "default-device metadata is unavailable"};
    const auto value = nlohmann::json{{"name", node->second->state.name}}.dump();
    const int status = pw_metadata_set_property(metadata_.begin()->second->proxy, PW_ID_CORE,
        input ? "default.configured.audio.source" : "default.configured.audio.sink",
        "Spa:String:JSON", value.c_str());
    return {status >= 0, status < 0 ? std::strerror(-status) : ""};
  }

 private:
  void wake() {
    const std::uint64_t one = 1;
    const auto result = write(event_, &one, sizeof(one));
    (void)result;
  }
  static void update(std::string& value, const spa_dict* properties, const char* key) {
    const char* text = properties == nullptr ? nullptr : spa_dict_lookup(properties, key);
    if (text != nullptr) value = text;
  }
  static void node_info(void* data, const pw_node_info* info) {
    auto* node = static_cast<Bound*>(data);
    const AudioNode previous = node->state;
    update(node->state.name, info->props, PW_KEY_NODE_NAME);
    update(node->state.description, info->props, PW_KEY_NODE_DESCRIPTION);
    update(node->state.app_name, info->props, PW_KEY_APP_NAME);
    node->state.state = pw_node_state_as_string(info->state);
    if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
      node->state.can_control = false;
      for (std::uint32_t index = 0; index < info->n_params; ++index)
        if (info->params[index].id == SPA_PARAM_Props)
          node->state.can_control = node->writable && (info->params[index].flags & SPA_PARAM_INFO_WRITE);
    }
    if (node->state != previous) node->self->wake();
  }
  static void node_param(void* data, int, std::uint32_t id, std::uint32_t, std::uint32_t,
                         const spa_pod* parameter) {
    if (id != SPA_PARAM_Props || parameter == nullptr || !spa_pod_is_object(parameter)) return;
    auto* node = static_cast<Bound*>(data);
    const AudioNode previous = node->state;
    if (const auto* mute = spa_pod_find_prop(parameter, nullptr, SPA_PROP_mute))
      spa_pod_get_bool(&mute->value, &node->state.muted);
    if (const auto* volumes = spa_pod_find_prop(parameter, nullptr, SPA_PROP_channelVolumes)) {
      std::array<float, 64> values{};
      const auto count = spa_pod_copy_array_full(&volumes->value, SPA_TYPE_Float,
                                                sizeof(float), values.data(), values.size());
      if (count > 0) {
        node->state.channel_volumes.assign(values.begin(), values.begin() + count);
        for (auto& value : node->state.channel_volumes)
          if (!std::isfinite(value) || value < 0) value = 0;
        node->state.volume_known = true;
      }
    } else if (const auto* volume = spa_pod_find_prop(parameter, nullptr, SPA_PROP_volume)) {
      float value = 0;
      if (spa_pod_get_float(&volume->value, &value) == 0 && std::isfinite(value) && value >= 0) {
        node->state.channel_volumes = {value};
        node->state.volume_known = true;
      }
    }
    if (node->state != previous) node->self->wake();
  }
  static int metadata_property(void* data, std::uint32_t subject, const char* key,
                                const char*, const char* value) {
    auto* self = static_cast<Pipewire*>(data);
    if (subject != PW_ID_CORE) return 0;
    if (key == nullptr) { self->default_sink_.clear(); self->default_source_.clear(); self->wake(); return 0; }
    std::string* target = nullptr;
    if (std::strcmp(key, "default.audio.sink") == 0) target = &self->default_sink_;
    if (std::strcmp(key, "default.audio.source") == 0) target = &self->default_source_;
    if (target == nullptr) return 0;
    const auto previous = *target;
    target->clear();
    if (value != nullptr) {
      const auto json = nlohmann::json::parse(value, nullptr, false);
      if (json.is_object() && json.contains("name") && json["name"].is_string()) *target = json["name"].get<std::string>();
    }
    if (*target != previous) self->wake();
    return 0;
  }
  static void global(void* data, std::uint32_t id, std::uint32_t permissions,
                      const char* type, std::uint32_t version, const spa_dict* properties) {
    auto* self = static_cast<Pipewire*>(data);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      const char* media = properties == nullptr ? nullptr : spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
      if (media == nullptr || std::string_view(media).find("Audio") == std::string_view::npos) return;
      auto node = std::make_unique<Bound>();
      node->self = self;
      node->state.id = id;
      node->state.media_class = media;
      update(node->state.name, properties, PW_KEY_NODE_NAME);
      update(node->state.description, properties, PW_KEY_NODE_DESCRIPTION);
      update(node->state.app_name, properties, PW_KEY_APP_NAME);
      if (node->state.description.empty()) node->state.description = node->state.name;
      node->writable = (permissions & PW_PERM_W) != 0;
      node->proxy = static_cast<pw_node*>(pw_registry_bind(self->registry_, id, type,
                                                          std::min<std::uint32_t>(version, PW_VERSION_NODE), 0));
      if (node->proxy == nullptr) return;
      static const pw_node_events events = [] {
        pw_node_events result{}; result.version = PW_VERSION_NODE_EVENTS;
        result.info = node_info; result.param = node_param; return result;
      }();
      pw_node_add_listener(node->proxy, &node->listener, &events, node.get());
      std::uint32_t parameters[] = {SPA_PARAM_Props};
      pw_node_subscribe_params(node->proxy, parameters, 1);
      pw_node_enum_params(node->proxy, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
      self->nodes_[id] = std::move(node);
      self->wake();
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
      const char* name = properties == nullptr ? nullptr : spa_dict_lookup(properties, PW_KEY_METADATA_NAME);
      if (name == nullptr || std::strcmp(name, "default") != 0) return;
      auto metadata = std::make_unique<Metadata>();
      metadata->proxy = static_cast<pw_metadata*>(pw_registry_bind(self->registry_, id, type,
                                                                  std::min<std::uint32_t>(version, PW_VERSION_METADATA), 0));
      if (metadata->proxy == nullptr) return;
      static const pw_metadata_events events = [] {
        pw_metadata_events result{}; result.version = PW_VERSION_METADATA_EVENTS;
        result.property = metadata_property; return result;
      }();
      pw_metadata_add_listener(metadata->proxy, &metadata->listener, &events, self);
      self->metadata_[id] = std::move(metadata);
    }
  }
  static void removed(void* data, std::uint32_t id) {
    auto* self = static_cast<Pipewire*>(data);
    bool changed = false;
    if (const auto found = self->nodes_.find(id); found != self->nodes_.end()) {
      spa_hook_remove(&found->second->listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(found->second->proxy));
      self->nodes_.erase(found);
      changed = true;
    }
    if (const auto found = self->metadata_.find(id); found != self->metadata_.end()) {
      spa_hook_remove(&found->second->listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(found->second->proxy));
      self->metadata_.erase(found);
      self->default_sink_.clear(); self->default_source_.clear();
      changed = true;
    }
    if (changed) self->wake();
  }
  void cleanup() {
    connected_.store(false);
    if (loop_started_) { pw_thread_loop_stop(loop_); loop_started_ = false; }
    for (auto& [id, node] : nodes_) {
      spa_hook_remove(&node->listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node->proxy));
    }
    nodes_.clear();
    for (auto& [id, metadata] : metadata_) {
      spa_hook_remove(&metadata->listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata->proxy));
    }
    metadata_.clear();
    if (registry_ != nullptr) {
      spa_hook_remove(&registry_listener_);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_)); registry_ = nullptr;
    }
    if (core_ != nullptr) { spa_hook_remove(&core_listener_); pw_core_disconnect(core_); core_ = nullptr; }
    if (context_ != nullptr) { pw_context_destroy(context_); context_ = nullptr; }
    if (loop_ != nullptr) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
  }

  int event_;
  std::mutex initialization_;
  std::atomic_bool connected_ = false;
  bool attempted_ = false, loop_started_ = false;
  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_registry* registry_ = nullptr;
  spa_hook core_listener_{}, registry_listener_{};
  int completed_ = -1;
  std::map<std::uint32_t, std::unique_ptr<Bound>> nodes_;
  std::map<std::uint32_t, std::unique_ptr<Metadata>> metadata_;
  std::string default_sink_, default_source_, error_;
};

Pipewire& service() { static Pipewire instance; return instance; }

Result result(std::pair<bool, std::string> value) { return {value.first, std::move(value.second)}; }

}  // namespace

bool available() { return service().start(); }
std::string error() { return service().error(); }
std::vector<Node> nodes(Filter filter) { return service().nodes(static_cast<int>(filter)); }
std::optional<Node> default_sink() { return service().default_node(false); }
std::optional<Node> default_source() { return service().default_node(true); }

Result set_volume(std::uint32_t id, double volume) {
  if (!std::isfinite(volume) || volume < 0 || volume > 4)
    return {false, "volume must be finite and between 0 and 4"};
  return result(service().set_volume(id, volume));
}
Result set_mute(std::uint32_t id, bool muted) { return result(service().set_mute(id, muted)); }
Result toggle_mute(std::uint32_t id) { return result(service().set_mute(id, std::nullopt)); }
Result set_default_sink(std::uint32_t id) { return result(service().set_default(id, false)); }
Result set_default_source(std::uint32_t id) { return result(service().set_default(id, true)); }

int event_fd() { return service().fd(); }
bool drain_events() { return service().drain(); }

}  // namespace vw::pipewire
