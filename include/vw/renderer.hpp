#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "vw/scene.hpp"

namespace vw {

struct Frame {
  Frame(std::uint32_t width, std::uint32_t height, int fd,
        std::uint32_t stride, std::uint64_t modifier);
  std::uint32_t width;
  std::uint32_t height;
  int fd = -1;
  std::uint32_t stride = 0;
  std::uint64_t modifier = 0;

  ~Frame();
  Frame(Frame&& other) noexcept;
  Frame& operator=(Frame&& other) noexcept;
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

};

class Renderer {
 public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  Frame render(const Scene& scene) const;

 private:
  struct State;
  State* state_;
};

}  // namespace vw
