#include "vw/renderer.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>
#include <unistd.h>

#include "vws.hpp"
// Generated at build time: vw's own shaders, compiled to .vws and embedded,
// so the library never searches the filesystem for them.
#include "vw_builtin_shaders.hpp"

namespace vw {
namespace {

void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(operation) + " failed (Vulkan error " +
                             std::to_string(result) + ")");
  }
}

struct LoadedShader {
  std::vector<std::uint32_t> spirv;
  std::uint16_t flags = 0;
};

// A .vws file, or plain SPIR-V for compatibility. Plain SPIR-V says nothing
// about what it reads, so it is assumed to read everything.
LoadedShader load_shader(std::span<const std::uint8_t> bytes, const std::string& origin) {
  if (vws::is_vws(bytes)) {
    try {
      vws::Shader shader = vws::decode(bytes);
      return {std::move(shader.spirv), shader.flags};
    } catch (const std::exception& error) {
      throw std::runtime_error(origin + ": " + error.what());
    }
  }
  if (vws::is_spirv(bytes)) {
    LoadedShader shader{std::vector<std::uint32_t>(bytes.size() / 4),
                        static_cast<std::uint16_t>(vws::reads_backdrop | vws::reads_data)};
    std::memcpy(shader.spirv.data(), bytes.data(), bytes.size());
    return shader;
  }
  throw std::runtime_error(origin + ": not a .vws or SPIR-V shader (compile it with vw-shaderc)");
}

LoadedShader load_shader_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("could not open shader " + path.string());
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  return load_shader(bytes, path.string());
}

// The embedded copy, unless VW_SHADER_DIR holds a newer one to try out.
LoadedShader load_builtin(std::string_view name) {
  if (const char* directory = std::getenv("VW_SHADER_DIR"); directory != nullptr && *directory != '\0') {
    const auto path = std::filesystem::path(directory) / (std::string(name) + ".vws");
    std::error_code error;
    if (std::filesystem::exists(path, error)) return load_shader_file(path);
  }
  const std::span<const std::uint8_t> embedded = builtin_shaders::find(name);
  if (embedded.empty()) throw std::runtime_error("vw was built without its " + std::string(name) + " shader");
  return load_shader(embedded, "built-in " + std::string(name));
}

std::uint32_t memory_type(VkPhysicalDevice physical_device, std::uint32_t allowed,
                          VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
  for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
    if ((allowed & (1U << index)) != 0U &&
        (properties.memoryTypes[index].propertyFlags & required) == required) {
      return index;
    }
  }
  throw std::runtime_error("no compatible Vulkan memory type");
}

struct PushConstants {
  std::array<float, 4> bounds;
  std::array<float, 4> params;
  std::array<float, 4> color;
  std::array<float, 4> color2;
  std::array<float, 4> border_color;
  std::array<float, 4> clip;
  std::array<float, 4> clip_params;
  std::array<float, 4> shader_params;
};

struct RasterPushConstants {
  std::array<float, 4> bounds;
  std::array<float, 4> source_info;
  std::array<float, 4> tint;
  std::array<float, 4> clip;
  std::array<float, 4> params;
  std::array<float, 4> effect;
  std::array<float, 4> colorize;
};

}  // namespace

struct Renderer::State {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipeline raster_pipeline = VK_NULL_HANDLE;
  struct ShaderPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::filesystem::file_time_type modified;
    std::uint16_t flags = 0;
  };
  std::unordered_map<std::string, ShaderPipeline> custom_pipelines;

  // A custom shader's pipeline and what it reads; rebuilt when the file changes.
  ShaderPipeline rectangle_pipeline(const std::string& path) {
    if (path.empty()) return {pipeline, {}, 0};
    const auto modified = std::filesystem::last_write_time(path);
    const auto found = custom_pipelines.find(path);
    if (found != custom_pipelines.end() && found->second.modified == modified)
      return found->second;
    const LoadedShader loaded = load_shader_file(path);
    const auto& code = loaded.spirv;
    const VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        nullptr, 0, code.size() * sizeof(std::uint32_t), code.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &module_info, nullptr, &module), "vkCreateShaderModule(custom)");
    const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    const VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        nullptr, 0, stage, pipeline_layout, VK_NULL_HANDLE, 0};
    VkPipeline created = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &created);
    vkDestroyShaderModule(device, module, nullptr);
    check(result, "vkCreateComputePipelines(custom)");
    if (found != custom_pipelines.end()) vkDestroyPipeline(device, found->second.pipeline, nullptr);
    custom_pipelines[path] = ShaderPipeline{created, modified, loaded.flags};
    return custom_pipelines[path];
  }

  State() {
    const VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "vw", 1,
                                        "vw", 1, VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0,
                                              &application, 0, nullptr, 0, nullptr};
    check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");

    std::uint32_t physical_count = 0;
    check(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr),
          "vkEnumeratePhysicalDevices");
    if (physical_count == 0) throw std::runtime_error("no Vulkan device found");
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    check(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()),
          "vkEnumeratePhysicalDevices");
    for (VkPhysicalDevice candidate : physical_devices) {
      std::uint32_t family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
      std::vector<VkQueueFamilyProperties> families(family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
      for (std::uint32_t index = 0; index < family_count; ++index) {
        if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
          physical_device = candidate;
          queue_family = index;
          break;
        }
      }
      if (physical_device != VK_NULL_HANDLE) break;
    }
    if (physical_device == VK_NULL_HANDLE) throw std::runtime_error("no Vulkan compute queue found");

    constexpr float priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr,
                                              0, queue_family, 1, &priority};
    const std::array<const char*, 4> extensions{
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
    const VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1,
                                          &queue_info, 0, nullptr,
                                          static_cast<std::uint32_t>(extensions.size()),
                                          extensions.data(), nullptr};
    check(vkCreateDevice(physical_device, &device_info, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                             VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                             queue_family};
    check(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool),
          "vkCreateCommandPool");

    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
    const VkDescriptorSetLayoutCreateInfo descriptor_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, bindings.size(),
        bindings.data()};
    check(vkCreateDescriptorSetLayout(device, &descriptor_info, nullptr, &descriptor_layout),
          "vkCreateDescriptorSetLayout");

    const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                         sizeof(PushConstants)};
    const VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  nullptr, 0, 1, &descriptor_layout, 1,
                                                  &push_range};
    check(vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout),
          "vkCreatePipelineLayout");

    const auto create_pipeline = [&](std::string_view name, VkPipeline& target) {
      const auto code = load_builtin(name).spirv;
      const VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          nullptr, 0, code.size() * sizeof(std::uint32_t), code.data()};
      VkShaderModule shader = VK_NULL_HANDLE;
      check(vkCreateShaderModule(device, &shader_info, nullptr, &shader), "vkCreateShaderModule");
      const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
      const VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          nullptr, 0, stage, pipeline_layout, VK_NULL_HANDLE, 0};
      const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                                        &target);
      vkDestroyShaderModule(device, shader, nullptr);
      check(result, "vkCreateComputePipelines");
    };
    create_pipeline("rounded_rect", pipeline);
    create_pipeline("raster", raster_pipeline);
  }

  ~State() {
    if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
    for (const auto& [path, entry] : custom_pipelines)
      vkDestroyPipeline(device, entry.pipeline, nullptr);
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
    if (raster_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, raster_pipeline, nullptr);
    if (pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (descriptor_layout != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
    if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
    if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
    if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
  }
};

Renderer::Renderer() : state_(new State) {}
Renderer::~Renderer() { delete state_; }

Frame Renderer::render(const Scene& scene) const {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  VkImageView image_view = VK_NULL_HANDLE;
  VkImage backdrop = VK_NULL_HANDLE;
  VkDeviceMemory backdrop_memory = VK_NULL_HANDLE;
  VkImageView backdrop_view = VK_NULL_HANDLE;
  std::vector<VkPipeline> rectangle_pipelines;
  // Whether each rectangle's shader samples the backdrop (binding 2); only
  // those pay for the full-surface copy.
  std::vector<bool> reads_backdrop;
  rectangle_pipelines.reserve(scene.rectangles.size());
  reads_backdrop.reserve(scene.rectangles.size());
  for (const Rectangle& rectangle : scene.rectangles) {
    const auto shader = rectangle.visible ? state_->rectangle_pipeline(rectangle.shader)
                                          : State::ShaderPipeline{state_->pipeline, {}, 0};
    rectangle_pipelines.push_back(shader.pipeline);
    reads_backdrop.push_back((shader.flags & vws::reads_backdrop) != 0);
  }
  // The snapshot image exists only when some shader samples it; otherwise
  // binding 2 just aliases the target and is never read.
  const bool needs_backdrop = std::ranges::find(reads_backdrop, true) != reads_backdrop.end();
  VkBuffer raster_buffer = VK_NULL_HANDLE;
  VkDeviceMemory raster_memory = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;

  const auto cleanup = [&] {
    if (command != VK_NULL_HANDLE)
      vkFreeCommandBuffers(state_->device, state_->command_pool, 1, &command);
    if (descriptor_pool != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(state_->device, descriptor_pool, nullptr);
    if (raster_buffer != VK_NULL_HANDLE) vkDestroyBuffer(state_->device, raster_buffer, nullptr);
    if (raster_memory != VK_NULL_HANDLE) vkFreeMemory(state_->device, raster_memory, nullptr);
    if (backdrop_view != VK_NULL_HANDLE) vkDestroyImageView(state_->device, backdrop_view, nullptr);
    if (backdrop != VK_NULL_HANDLE) vkDestroyImage(state_->device, backdrop, nullptr);
    if (backdrop_memory != VK_NULL_HANDLE) vkFreeMemory(state_->device, backdrop_memory, nullptr);
    if (image_view != VK_NULL_HANDLE) vkDestroyImageView(state_->device, image_view, nullptr);
    if (image != VK_NULL_HANDLE) vkDestroyImage(state_->device, image, nullptr);
    if (image_memory != VK_NULL_HANDLE) vkFreeMemory(state_->device, image_memory, nullptr);
  };

  try {
    constexpr std::uint64_t drm_format_mod_linear = 0;
    const VkImageDrmFormatModifierListCreateInfoEXT modifier_info{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, nullptr, 1,
        &drm_format_mod_linear};
    const VkExternalMemoryImageCreateInfo external_image{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modifier_info,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkImageCreateInfo image_info{
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external_image, 0, VK_IMAGE_TYPE_2D,
        VK_FORMAT_R8G8B8A8_UNORM, {scene.width, scene.height, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
    check(vkCreateImage(state_->device, &image_info, nullptr, &image), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(state_->device, image, &requirements);
    const VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkMemoryAllocateInfo image_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info, requirements.size,
        memory_type(state_->physical_device, requirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(state_->device, &image_allocation, nullptr, &image_memory),
          "vkAllocateMemory(image)");
    check(vkBindImageMemory(state_->device, image, image_memory, 0), "vkBindImageMemory");

    const VkImageViewCreateInfo view_info{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D,
        VK_FORMAT_R8G8B8A8_UNORM, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(state_->device, &view_info, nullptr, &image_view),
           "vkCreateImageView");

    if (needs_backdrop) {
      VkImageCreateInfo info = image_info;
      info.pNext = nullptr;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      check(vkCreateImage(state_->device, &info, nullptr, &backdrop), "vkCreateImage(backdrop)");
      vkGetImageMemoryRequirements(state_->device, backdrop, &requirements);
      const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
          requirements.size, memory_type(state_->physical_device, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      check(vkAllocateMemory(state_->device, &allocation, nullptr, &backdrop_memory), "vkAllocateMemory(backdrop)");
      check(vkBindImageMemory(state_->device, backdrop, backdrop_memory, 0), "vkBindImageMemory(backdrop)");
      VkImageViewCreateInfo view = view_info;
      view.image = backdrop;
      check(vkCreateImageView(state_->device, &view, nullptr, &backdrop_view), "vkCreateImageView(backdrop)");
    }

    std::vector<std::uint32_t> raster_pixels;
    for (const Raster& raster : scene.rasters)
      if (raster.rgba) raster_pixels.insert(raster_pixels.end(), raster.rgba->begin(), raster.rgba->end());
    // Custom shader data follows the pixels in the same buffer; the shader
    // finds it through clip_params.y (offset) and .z (count).
    std::vector<std::uint32_t> data_offsets(scene.rectangles.size(), 0);
    for (std::size_t index = 0; index < scene.rectangles.size(); ++index) {
      const Rectangle& rectangle = scene.rectangles[index];
      if (rectangle.shader.empty() || rectangle.shader_data.empty()) continue;
      data_offsets[index] = static_cast<std::uint32_t>(raster_pixels.size());
      for (float value : rectangle.shader_data) raster_pixels.push_back(std::bit_cast<std::uint32_t>(value));
    }
    // Image masks follow them; the raster shader finds its own through the
    // spare fourth components of its effect and colorize constants.
    std::vector<std::uint32_t> mask_offsets(scene.rasters.size(), 0);
    for (std::size_t index = 0; index < scene.rasters.size(); ++index) {
      if (scene.rasters[index].mask.empty()) continue;
      mask_offsets[index] = static_cast<std::uint32_t>(raster_pixels.size());
      for (float value : scene.rasters[index].mask) raster_pixels.push_back(std::bit_cast<std::uint32_t>(value));
    }
    if (raster_pixels.empty()) raster_pixels.push_back(0);
    const VkDeviceSize raster_bytes = raster_pixels.size() * sizeof(std::uint32_t);
    const VkBufferCreateInfo raster_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
        raster_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    check(vkCreateBuffer(state_->device, &raster_info, nullptr, &raster_buffer),
          "vkCreateBuffer(raster)");
    vkGetBufferMemoryRequirements(state_->device, raster_buffer, &requirements);
    const VkMemoryAllocateInfo raster_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
        requirements.size, memory_type(state_->physical_device, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check(vkAllocateMemory(state_->device, &raster_allocation, nullptr, &raster_memory),
          "vkAllocateMemory(raster)");
    check(vkBindBufferMemory(state_->device, raster_buffer, raster_memory, 0),
          "vkBindBufferMemory(raster)");
    void* raster_mapping = nullptr;
    check(vkMapMemory(state_->device, raster_memory, 0, raster_bytes, 0, &raster_mapping),
          "vkMapMemory(raster)");
    std::memcpy(raster_mapping, raster_pixels.data(), static_cast<std::size_t>(raster_bytes));
    vkUnmapMemory(state_->device, raster_memory);

    const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    const VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                nullptr, 0, 1, pool_sizes.size(), pool_sizes.data()};
    check(vkCreateDescriptorPool(state_->device, &pool_info, nullptr, &descriptor_pool),
          "vkCreateDescriptorPool");
    const VkDescriptorSetAllocateInfo descriptor_allocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool, 1,
        &state_->descriptor_layout};
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(state_->device, &descriptor_allocation, &descriptor),
          "vkAllocateDescriptorSets");
    const VkDescriptorImageInfo descriptor_image{VK_NULL_HANDLE, image_view,
                                                  VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo descriptor_buffer{raster_buffer, 0, raster_bytes};
    const VkDescriptorImageInfo descriptor_backdrop{VK_NULL_HANDLE,
        needs_backdrop ? backdrop_view : image_view, VK_IMAGE_LAYOUT_GENERAL};
    const std::array<VkWriteDescriptorSet, 3> writes{{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor, 0, 0, 1,
         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &descriptor_image, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor, 1, 0, 1,
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &descriptor_buffer, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor, 2, 0, 1,
          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &descriptor_backdrop, nullptr, nullptr}}};
    vkUpdateDescriptorSets(state_->device, writes.size(), writes.data(), 0, nullptr);

    const VkCommandBufferAllocateInfo command_allocation{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, state_->command_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    check(vkAllocateCommandBuffers(state_->device, &command_allocation, &command),
          "vkAllocateCommandBuffers");
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                  VK_QUEUE_FAMILY_IGNORED, image,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkClearColorValue clear{{0.0F, 0.0F, 0.0F, 0.0F}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, state_->pipeline_layout, 0,
                             1, &descriptor, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state_->pipeline);
    bool backdrop_initialized = false;
    // Text and images draw in scene order: each raster right after the
    // rectangle that carries it, so a later panel covers earlier text.
    std::vector<std::uint32_t> raster_offsets(scene.rasters.size());
    std::vector<std::vector<std::size_t>> carried(scene.rectangles.size());
    {
      std::uint32_t offset = 0;
      for (std::size_t raster_index = 0; raster_index < scene.rasters.size(); ++raster_index) {
        raster_offsets[raster_index] = offset;
        offset += static_cast<std::uint32_t>(scene.rasters[raster_index].size());
        carried.at(scene.rasters[raster_index].carrier).push_back(raster_index);
      }
    }
    const auto draw_raster = [&](std::size_t raster_index) {
      const Raster& raster = scene.rasters[raster_index];
      const Rectangle& carrier = scene.rectangles.at(raster.carrier);
      if (!carrier.raster_visible) return;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state_->raster_pipeline);
      const float raster_x = raster.pixel_snap ? std::round(carrier.x) : carrier.x;
      const float raster_y = raster.pixel_snap ? std::round(carrier.y) : carrier.y;
      const RasterPushConstants push{
          {raster_x, raster_y, carrier.width, carrier.height},
          {static_cast<float>(raster_offsets[raster_index]), static_cast<float>(raster.width),
            static_cast<float>(raster.height), raster.use_tint ? 1.0F : 0.0F},
          carrier.color, carrier.clip,
           {carrier.radius, carrier.clip_radius, carrier.color[3],
            raster.alpha_mask ? 1.0F : 0.0F},
          {raster.effect.saturation, raster.effect.hue_rotate * 3.14159265F / 180.0F, raster.effect.colorize_amount,
           static_cast<float>(raster.mask.size() / 2)},
          {raster.effect.colorize[0], raster.effect.colorize[1], raster.effect.colorize[2],
           static_cast<float>(mask_offsets[raster_index])}};
      vkCmdPushConstants(command, state_->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(push), &push);
      vkCmdDispatch(command,
                    (static_cast<std::uint32_t>(std::ceil(carrier.width)) + 7) / 8,
                    (static_cast<std::uint32_t>(std::ceil(carrier.height)) + 7) / 8, 1);
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
    };
    for (std::size_t rectangle_index = 0; rectangle_index < scene.rectangles.size(); ++rectangle_index) {
      const Rectangle& rectangle = scene.rectangles[rectangle_index];
      // A carrier is usually invisible itself; its rasters still draw here.
      const auto draw_carried = [&] {
        for (std::size_t raster_index : carried[rectangle_index]) draw_raster(raster_index);
      };
      if (!rectangle.visible) {
        draw_carried();
        continue;
      }
      if (reads_backdrop[rectangle_index]) {
        std::array<VkImageMemoryBarrier, 2> copies{{barrier, barrier}};
        copies[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        copies[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copies[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        copies[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copies[1].image = backdrop;
        copies[1].srcAccessMask = backdrop_initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        copies[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copies[1].oldLayout = backdrop_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        copies[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, copies.size(), copies.data());
        const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {scene.width, scene.height, 1}};
        vkCmdCopyImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       backdrop, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        for (auto& copy : copies) {
          std::swap(copy.oldLayout, copy.newLayout);
          copy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        copies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        copies[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, copies.size(), copies.data());
        backdrop_initialized = true;
      }
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, rectangle_pipelines[rectangle_index]);
      auto shader_params = rectangle.shader_params;
      if (rectangle.shader.empty()) {
        for (int corner = 0; corner < 4; ++corner)
          shader_params[corner] = rectangle.corner_radii[corner] < 0.0F
              ? rectangle.radius : rectangle.corner_radii[corner];
      }
      const PushConstants push{{rectangle.x, rectangle.y, rectangle.width, rectangle.height},
                               {rectangle.radius, rectangle.border_width,
                                static_cast<float>(rectangle.gradient),
                                rectangle.exact_coverage ? 1.0F : 0.0F},
                               rectangle.color, rectangle.color2, rectangle.border_color,
                                rectangle.clip,
                                {rectangle.clip_radius, static_cast<float>(data_offsets[rectangle_index]),
                                 static_cast<float>(rectangle.shader_data.size()), 0.0F},
                                shader_params};
      vkCmdPushConstants(command, state_->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(push), &push);
      vkCmdDispatch(command,
                    (static_cast<std::uint32_t>(std::ceil(rectangle.width)) + 7) / 8,
                    (static_cast<std::uint32_t>(std::ceil(rectangle.height)) + 7) / 8, 1);
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
      draw_carried();
    }
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = state_->queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1,
                              &command, 0, nullptr};
    check(vkQueueSubmit(state_->queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(state_->queue), "vkQueueWaitIdle");

    const auto get_modifier = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
        vkGetDeviceProcAddr(state_->device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    const auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(state_->device, "vkGetMemoryFdKHR"));
    if (get_modifier == nullptr || get_fd == nullptr)
      throw std::runtime_error("Vulkan DMA-BUF export functions are unavailable");
    VkImageDrmFormatModifierPropertiesEXT modifier{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT, nullptr, 0};
    check(get_modifier(state_->device, image, &modifier),
          "vkGetImageDrmFormatModifierPropertiesEXT");
    VkImageSubresource subresource{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(state_->device, image, &subresource, &layout);
    VkMemoryGetFdInfoKHR fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr,
                                 image_memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int dma_fd = -1;
    check(get_fd(state_->device, &fd_info, &dma_fd), "vkGetMemoryFdKHR");
    Frame frame{scene.width, scene.height, dma_fd,
                static_cast<std::uint32_t>(layout.rowPitch), modifier.drmFormatModifier};
    cleanup();
    return frame;
  } catch (...) {
    cleanup();
    throw;
  }
}

Frame::~Frame() {
  if (fd >= 0) close(fd);
}

Frame::Frame(std::uint32_t frame_width, std::uint32_t frame_height, int frame_fd,
             std::uint32_t frame_stride, std::uint64_t frame_modifier)
    : width(frame_width), height(frame_height), fd(frame_fd), stride(frame_stride),
      modifier(frame_modifier) {}

Frame::Frame(Frame&& other) noexcept
    : width(other.width), height(other.height), fd(other.fd), stride(other.stride),
      modifier(other.modifier) {
  other.fd = -1;
}

Frame& Frame::operator=(Frame&& other) noexcept {
  if (this == &other) return *this;
  if (fd >= 0) close(fd);
  width = other.width;
  height = other.height;
  fd = other.fd;
  stride = other.stride;
  modifier = other.modifier;
  other.fd = -1;
  return *this;
}

}  // namespace vw
