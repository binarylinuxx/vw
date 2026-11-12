#include "vw/outputs.hpp"

#include <xdg-output-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>
#include <zwayland/client/display.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace vw {
namespace {

namespace generated = zwayland::generated;
using zwayland::client::Proxy;

// One wl_output and its xdg_output. Values arrive in pieces and become
// current on done.
struct Entry {
  std::uint32_t global = 0;
  Proxy* output = nullptr;
  Proxy* logical = nullptr;  // zxdg_output_v1
  Output pending;
  Output current;
};

class Watcher {
 public:
  Watcher() : display_(zwayland::client::Display::connect()) {
    if (!display_) return;
    registry_ = zwayland::client::core::get_registry(*display_);
    if (registry_ == nullptr) {
      display_.reset();
      return;
    }
    struct Registry {
      Watcher* self;
      void global(Proxy&, std::uint32_t name, const std::string& interface, std::uint32_t version) {
        if (interface == generated::wl_output_interface.name) self->add(name, version);
        else if (interface == generated::zxdg_output_manager_v1_interface.name) self->bind_manager(name, version);
      }
      void global_remove(Proxy&, std::uint32_t name) { self->remove(name); }
    };
    registry_->set_observer(generated::wl_registry_observer(Registry{this}));
    // Globals, then each output's details, then the xdg_outputs made for
    // outputs announced before the manager.
    for (int round = 0; round < 3 && display_; ++round)
      if (!display_->roundtrip() || display_->has_error()) display_.reset();
    changed_ = false;
  }

  int fd() const { return display_ ? display_->fd() : -1; }

  bool drain() {
    if (!display_) return false;
    if (!display_->dispatch_pending() || display_->has_error()) {
      display_.reset();
      entries_.clear();
      return true;
    }
    display_->flush();
    return std::exchange(changed_, false);
  }

  std::vector<Output> outputs() const {
    std::vector<Output> result;
    for (const auto& entry : entries_)
      if (!entry->current.name.empty() && entry->current.width > 0) result.push_back(entry->current);
    return result;
  }

 private:
  void bind_manager(std::uint32_t name, std::uint32_t version) {
    manager_ = zwayland::client::core::bind(*display_, *registry_, name, generated::zxdg_output_manager_v1_interface,
                                            std::min(version, generated::zxdg_output_manager_v1_interface.version));
    if (manager_ == nullptr) return;
    for (const auto& entry : entries_) watch_logical(*entry);
  }

  void add(std::uint32_t name, std::uint32_t version) {
    auto entry = std::make_unique<Entry>();
    entry->global = name;
    entry->output = zwayland::client::core::bind(*display_, *registry_, name, generated::wl_output_interface,
                                                 std::min(version, generated::wl_output_interface.version));
    if (entry->output == nullptr) return;
    struct Events {
      Watcher* self;
      Entry* entry;
      void geometry(Proxy&, std::int32_t x, std::int32_t y, std::int32_t, std::int32_t, std::int32_t,
                    const std::string& make, const std::string& model, std::int32_t) {
        // Superseded by xdg_output when the compositor has it.
        if (entry->logical == nullptr) {
          entry->pending.x = x;
          entry->pending.y = y;
        }
        if (entry->pending.description.empty()) entry->pending.description = make + " " + model;
      }
      void mode(Proxy&, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t refresh) {
        if ((flags & generated::WL_OUTPUT_MODE_CURRENT) == 0) return;
        entry->pending.pixel_width = static_cast<std::uint32_t>(std::max(width, 0));
        entry->pending.pixel_height = static_cast<std::uint32_t>(std::max(height, 0));
        entry->pending.refresh = static_cast<std::uint32_t>(std::max(refresh, 0));
      }
      void scale(Proxy&, std::int32_t factor) { entry->pending.scale = std::max(factor, 1); }
      void name(Proxy&, const std::string& value) { entry->pending.name = value; }
      void description(Proxy&, const std::string& value) { entry->pending.description = value; }
      void done(Proxy&) { self->commit(*entry); }
    };
    entry->output->set_observer(generated::wl_output_observer(Events{this, entry.get()}));
    if (manager_ != nullptr) watch_logical(*entry);
    entries_.push_back(std::move(entry));
  }

  void watch_logical(Entry& entry) {
    if (entry.logical != nullptr) return;
    const std::uint32_t id = generated::zxdg_output_manager_v1_get_xdg_output(*display_, manager_->id, entry.output);
    entry.logical = display_->find_proxy(id);
    if (entry.logical == nullptr) return;
    struct Events {
      Watcher* self;
      Entry* entry;
      void logical_position(Proxy&, std::int32_t x, std::int32_t y) {
        entry->pending.x = x;
        entry->pending.y = y;
      }
      void logical_size(Proxy&, std::int32_t width, std::int32_t height) {
        entry->pending.width = static_cast<std::uint32_t>(std::max(width, 0));
        entry->pending.height = static_cast<std::uint32_t>(std::max(height, 0));
      }
      void name(Proxy&, const std::string& value) {
        if (entry->pending.name.empty()) entry->pending.name = value;
      }
      void description(Proxy&, const std::string& value) {
        if (entry->pending.description.empty()) entry->pending.description = value;
      }
      // xdg_output v3 sends its changes before wl_output.done; older ones
      // send their own done.
      void done(Proxy&) {
        if (entry->logical->version < 3) self->commit(*entry);
      }
    };
    entry.logical->set_observer(generated::zxdg_output_v1_observer(Events{this, &entry}));
  }

  void commit(Entry& entry) {
    Output next = entry.pending;
    if (entry.logical == nullptr || next.width == 0) {
      // No xdg_output: the mode divided by the integer scale.
      const auto factor = static_cast<std::uint32_t>(std::max(next.scale, 1.0));
      next.width = next.pixel_width / factor;
      next.height = next.pixel_height / factor;
    }
    if (next.width > 0 && next.pixel_width > 0)
      next.scale = static_cast<double>(next.pixel_width) / next.width;
    const Output& before = entry.current;
    if (next.name != before.name || next.description != before.description || next.x != before.x ||
        next.y != before.y || next.width != before.width || next.height != before.height ||
        next.pixel_width != before.pixel_width || next.pixel_height != before.pixel_height ||
        next.refresh != before.refresh)
      changed_ = true;
    entry.current = std::move(next);
  }

  void remove(std::uint32_t name) {
    const auto found = std::ranges::find(entries_, name, [](const auto& entry) { return entry->global; });
    if (found == entries_.end()) return;
    Entry& entry = **found;
    if (entry.logical != nullptr) generated::zxdg_output_v1_destroy(*display_, entry.logical->id);
    if (entry.output != nullptr) {
      if (entry.output->version >= 3) generated::wl_output_release(*display_, entry.output->id);
      else entry.output->set_observer([](Proxy&, std::uint32_t, zwayland::wire::MessageParser&) {});
    }
    entries_.erase(found);
    changed_ = true;
  }

  std::unique_ptr<zwayland::client::Display> display_;
  Proxy* registry_ = nullptr;
  Proxy* manager_ = nullptr;
  std::vector<std::unique_ptr<Entry>> entries_;
  bool changed_ = false;
};

Watcher& watcher() {
  static Watcher instance;
  return instance;
}

}  // namespace

std::vector<Output> outputs() { return watcher().outputs(); }

std::optional<Output> output(std::string_view name) {
  for (Output& candidate : outputs())
    if (candidate.name == name) return std::move(candidate);
  return std::nullopt;
}

int output_event_fd() { return watcher().fd(); }

bool drain_output_events() {
  try {
    return watcher().drain();
  } catch (...) {
    return true;
  }
}

}  // namespace vw
