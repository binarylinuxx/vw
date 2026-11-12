#include "vw/capture.hpp"

#include <zwwm-screencopy-view-unstable-v1-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>
#include <zwayland/client/display.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace vw {
namespace {

namespace generated = zwayland::generated;
using zwayland::client::Proxy;

struct Output {
  Proxy* proxy = nullptr;
  std::string name;
};

// Converts one wl_shm pixel to RGBA with red in the low byte.
std::uint32_t to_rgba(std::uint32_t pixel, std::uint32_t format) {
  switch (format) {
    case generated::WL_SHM_FORMAT_ARGB8888:
    case generated::WL_SHM_FORMAT_XRGB8888: {
      const std::uint32_t alpha = format == generated::WL_SHM_FORMAT_ARGB8888 ? pixel >> 24U : 0xffU;
      return ((pixel >> 16U) & 0xffU) | (pixel & 0xff00U) | ((pixel & 0xffU) << 16U) | (alpha << 24U);
    }
    case generated::WL_SHM_FORMAT_XBGR8888: return pixel | 0xff000000U;
    default: return pixel;
  }
}

class Capturer {
 public:
  Capturer() : display_(zwayland::client::Display::connect()) {
    if (!display_) throw std::runtime_error("could not connect to the Wayland compositor");
    Proxy* registry = zwayland::client::core::get_registry(*display_);
    if (registry == nullptr) throw std::runtime_error("could not create the Wayland registry");
    struct Registry {
      Capturer* self;
      Proxy* registry;
      void global(Proxy&, std::uint32_t name, const std::string& interface, std::uint32_t version) {
        auto& display = *self->display_;
        if (interface == generated::zwlr_screencopy_manager_v1_interface.name) {
          self->manager_ = zwayland::client::core::bind(display, *registry, name,
              generated::zwlr_screencopy_manager_v1_interface,
              std::min(version, generated::zwlr_screencopy_manager_v1_interface.version));
        } else if (interface == generated::wl_shm_interface.name) {
          self->shm_ = zwayland::client::core::bind(display, *registry, name,
                                                    generated::wl_shm_interface, 1);
          // Formats are known from the buffer event; ignore the list.
          if (self->shm_ != nullptr)
            self->shm_->set_observer([](Proxy& proxy, std::uint32_t opcode,
                                        zwayland::wire::MessageParser& parser) {
              parser.discard(proxy.interface->events[opcode].signature);
            });
        } else if (interface == generated::wl_output_interface.name) {
          self->add_output(*registry, name, version);
        }
      }
      void global_remove(Proxy&, std::uint32_t) {}
    };
    registry->set_observer(generated::wl_registry_observer(Registry{this, registry}));
    // Bind, then let the outputs report their names.
    check(display_->roundtrip());
    check(display_->roundtrip());
  }

  Bitmap capture(const CaptureProps& props) {
    if (manager_ == nullptr || shm_ == nullptr)
      throw std::runtime_error("compositor does not offer wlr-screencopy");
    const auto output = std::ranges::find_if(outputs_, [&](const auto& entry) {
      return props.output.empty() || entry->name == props.output;
    });
    if (output == outputs_.end())
      throw std::runtime_error("no output named " + std::string(props.output));
    const std::int32_t cursor = props.cursor ? 1 : 0;
    const std::uint32_t id = props.region
        ? generated::zwlr_screencopy_manager_v1_capture_output_region(
              *display_, manager_->id, cursor, (*output)->proxy, props.region->x, props.region->y,
              props.region->width, props.region->height)
        : generated::zwlr_screencopy_manager_v1_capture_output(*display_, manager_->id, cursor,
                                                              (*output)->proxy);
    Proxy* frame = display_->find_proxy(id);
    if (frame == nullptr) throw std::runtime_error("could not create screencopy frame");
    Frame state;
    frame->set_observer([&state](Proxy& proxy, std::uint32_t opcode,
                                 zwayland::wire::MessageParser& parser) {
      switch (opcode) {
        case generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_BUFFER_OPCODE:
          state.format = parser.read_uint();
          state.width = parser.read_uint();
          state.height = parser.read_uint();
          state.stride = parser.read_uint();
          state.has_buffer = true;
          break;
        case generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_FLAGS_OPCODE:
          state.y_invert = (parser.read_uint() & generated::ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
          break;
        case generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_READY_OPCODE:
          parser.discard(proxy.interface->events[opcode].signature);
          state.ready = true;
          break;
        case generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_FAILED_OPCODE:
          state.failed = true;
          break;
        case generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_BUFFER_DONE_OPCODE:
          state.buffer_done = true;
          break;
        default:
          parser.discard(proxy.interface->events[opcode].signature);
      }
    });
    // Before version 3 there is no buffer_done; the buffer event comes first.
    const bool announces_done = frame->version >= 3;
    try {
      while (!state.failed && !(announces_done ? state.buffer_done : state.has_buffer))
        check(display_->dispatch());
      if (state.failed || !state.has_buffer) throw std::runtime_error("screen capture failed");
      if (state.width == 0 || state.height == 0 || state.width > 16384 || state.height > 16384 ||
          state.stride < state.width * 4)
        throw std::runtime_error("screen capture has an unusable size");
      const auto size = static_cast<std::size_t>(state.stride) * state.height;
      Shm shm(size);
      const auto pool = generated::wl_shm_create_pool(*display_, shm_->id, shm.fd,
                                                      static_cast<std::int32_t>(size));
      const auto buffer = generated::wl_shm_pool_create_buffer(
          *display_, pool, 0, static_cast<std::int32_t>(state.width),
          static_cast<std::int32_t>(state.height), static_cast<std::int32_t>(state.stride),
          state.format);
      generated::zwlr_screencopy_frame_v1_copy(*display_, id, display_->find_proxy(buffer));
      while (!state.ready && !state.failed) check(display_->dispatch());
      generated::wl_buffer_destroy(*display_, buffer);
      generated::wl_shm_pool_destroy(*display_, pool);
      generated::zwlr_screencopy_frame_v1_destroy(*display_, id);
      check(display_->flush());
      if (state.failed) throw std::runtime_error("screen capture failed");
      return convert(state, shm);
    } catch (...) {
      if (display_->find_proxy(id) != nullptr) {
        generated::zwlr_screencopy_frame_v1_destroy(*display_, id);
        (void)display_->flush();
      }
      throw;
    }
  }

 private:
  struct Frame {
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    bool has_buffer = false;
    bool buffer_done = false;
    bool y_invert = false;
    bool ready = false;
    bool failed = false;
  };

  struct Shm {
    explicit Shm(std::size_t bytes) : size(bytes) {
      fd = memfd_create("vw-capture", MFD_CLOEXEC);
      if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        if (fd >= 0) close(fd);
        throw std::runtime_error("could not allocate capture memory");
      }
      data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (data == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("could not map capture memory");
      }
    }
    ~Shm() {
      munmap(data, size);
      close(fd);
    }
    Shm(const Shm&) = delete;
    Shm& operator=(const Shm&) = delete;
    int fd = -1;
    void* data = nullptr;
    std::size_t size = 0;
  };

  static Bitmap convert(const Frame& frame, const Shm& shm) {
    switch (frame.format) {
      case generated::WL_SHM_FORMAT_ARGB8888:
      case generated::WL_SHM_FORMAT_XRGB8888:
      case generated::WL_SHM_FORMAT_ABGR8888:
      case generated::WL_SHM_FORMAT_XBGR8888: break;
      default: throw std::runtime_error("screen capture uses an unsupported pixel format");
    }
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(frame.width) * frame.height);
    const auto* bytes = static_cast<const unsigned char*>(shm.data);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
      const std::uint32_t row = frame.y_invert ? frame.height - 1 - y : y;
      const auto* source = reinterpret_cast<const std::uint32_t*>(bytes + static_cast<std::size_t>(row) * frame.stride);
      auto* target = pixels.data() + static_cast<std::size_t>(y) * frame.width;
      for (std::uint32_t x = 0; x < frame.width; ++x) target[x] = to_rgba(source[x], frame.format);
    }
    return {frame.width, frame.height,
            std::make_shared<const std::vector<std::uint32_t>>(std::move(pixels))};
  }

  void add_output(Proxy& registry, std::uint32_t name, std::uint32_t version) {
    auto output = std::make_unique<Output>();
    output->proxy = zwayland::client::core::bind(*display_, registry, name,
        generated::wl_output_interface, std::min(version, generated::wl_output_interface.version));
    if (output->proxy == nullptr) return;
    struct OutputEvents {
      Output* output;
      void geometry(Proxy&, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                    const std::string&, const std::string&, std::int32_t) {}
      void mode(Proxy&, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
      void done(Proxy&) {}
      void scale(Proxy&, std::int32_t) {}
      void name(Proxy&, const std::string& name) { output->name = name; }
      void description(Proxy&, const std::string&) {}
    };
    output->proxy->set_observer(generated::wl_output_observer(OutputEvents{output.get()}));
    outputs_.push_back(std::move(output));
  }

  void check(bool succeeded) const {
    if (!succeeded || display_->has_error())
      throw std::runtime_error(display_->has_error() ? display_->error_message()
                                                       : "capture connection failed");
  }

  std::unique_ptr<zwayland::client::Display> display_;
  Proxy* manager_ = nullptr;
  Proxy* shm_ = nullptr;
  std::vector<std::unique_ptr<Output>> outputs_;
};

}  // namespace

Bitmap capture_output(const CaptureProps& props) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  static std::unique_ptr<Capturer> capturer;
  if (!capturer) capturer = std::make_unique<Capturer>();
  try {
    return capturer->capture(props);
  } catch (...) {
    // A broken connection is rebuilt on the next call.
    capturer.reset();
    throw;
  }
}

}  // namespace vw
