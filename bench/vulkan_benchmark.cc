#include "vulkan_benchmark.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

uint32_t Align(uint32_t a, uint32_t b) { return (a + b - 1) / b * b; }

constexpr auto timestamp_count = 15;
constexpr uint64_t kDefaultFenceTimeoutMs = 30000;
constexpr uint32_t kWorkgroupSize = 256;

bool DetailedProfilingEnabled() { return std::getenv("VRDX_PROFILE") != nullptr; }

std::string RequestedSubgroupMode() {
  const char* value = std::getenv("VRDX_SUBGROUP_SIZE");
  return value == nullptr ? "auto" : value;
}

uint64_t FenceTimeoutNs() {
  const char* value = std::getenv("VRDX_FENCE_TIMEOUT_MS");
  if (value == nullptr) return kDefaultFenceTimeoutMs * 1000000;

  char* end = nullptr;
  unsigned long long timeout_ms = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || timeout_ms == 0 ||
      timeout_ms > std::numeric_limits<uint64_t>::max() / 1000000) {
    throw std::runtime_error("VRDX_FENCE_TIMEOUT_MS must be a positive integer");
  }
  return static_cast<uint64_t>(timeout_ms) * 1000000;
}

void WaitForFence(VkDevice device, VkFence fence, const char* stage) {
  VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, FenceTimeoutNs());
  if (result == VK_SUCCESS) return;
  if (result == VK_TIMEOUT) {
    throw std::runtime_error(std::string("Vulkan fence timeout during ") + stage);
  }
  throw std::runtime_error(std::string("vkWaitForFences failed during ") + stage +
                           " with VkResult " + std::to_string(result));
}

void FillStageTimes(const std::vector<uint64_t>& timestamps, float timestamp_period,
                    BenchmarkBase::Results* result) {
  auto elapsed = [&](int begin, int end) {
    return static_cast<uint64_t>((timestamps[end] - timestamps[begin]) * timestamp_period);
  };

  result->transfer_time = elapsed(0, 1);
  for (int pass = 0; pass < 4; ++pass) {
    int base = 1 + 3 * pass;
    result->upsweep_time += elapsed(base, base + 1);
    result->spine_time += elapsed(base + 1, base + 2);
    result->downsweep_time += elapsed(base + 2, base + 3);
  }
  result->finalize_time = elapsed(13, 14);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
              VkDebugUtilsMessageTypeFlagsEXT messageType,
              const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
  std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl << std::endl;

  return VK_FALSE;
}

VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
                                      const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator,
                                      VkDebugUtilsMessengerEXT* pDebugMessenger) {
  auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
      instance, "vkCreateDebugUtilsMessengerEXT");
  if (func != nullptr) {
    return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
  } else {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                   const VkAllocationCallbacks* pAllocator) {
  auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
      instance, "vkDestroyDebugUtilsMessengerEXT");
  if (func != nullptr) {
    func(instance, debugMessenger, pAllocator);
  }
}

}  // namespace

std::string VulkanBenchmark::LibraryVersion() const {
  return "v" + std::to_string(VRDX_VERSION_MAJOR) + "." +
               std::to_string(VRDX_VERSION_MINOR) + "." +
               std::to_string(VRDX_VERSION_PATCH);
}

VulkanBenchmark::VulkanBenchmark() {
  volkInitialize();

  // instance
  VkApplicationInfo application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application_info.pApplicationName = "vk_radix_sort_benchmark";
  application_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 0, 0);
  application_info.pEngineName = "vk_radix_sort";
  application_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 0, 0);
  application_info.apiVersion = VK_API_VERSION_1_3;

  VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  messenger_info.pfnUserCallback = DebugCallback;

  std::vector<const char*> layers = {};
  std::vector<const char*> instance_extensions = {
      VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#ifdef __APPLE__
      VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#endif
  };

  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#ifdef __APPLE__
  instance_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
  instance_info.pNext = &messenger_info;
  instance_info.pApplicationInfo = &application_info;
  instance_info.enabledLayerCount = layers.size();
  instance_info.ppEnabledLayerNames = layers.data();
  instance_info.enabledExtensionCount = instance_extensions.size();
  instance_info.ppEnabledExtensionNames = instance_extensions.data();
  vkCreateInstance(&instance_info, NULL, &instance_);
  volkLoadInstance(instance_);

  CreateDebugUtilsMessengerEXT(instance_, &messenger_info, NULL, &messenger_);

  // physical device
  uint32_t physical_device_count = 0;
  vkEnumeratePhysicalDevices(instance_, &physical_device_count, NULL);
  std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
  vkEnumeratePhysicalDevices(instance_, &physical_device_count, physical_devices.data());
  physical_device_ = physical_devices[0];
  VkPhysicalDeviceProperties physical_device_properties = {};
  vkGetPhysicalDeviceProperties(physical_device_, &physical_device_properties);
  timestamp_period_ = physical_device_properties.limits.timestampPeriod;

  // find graphics queue
  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, NULL);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count,
                                           queue_families.data());

  for (size_t i = 0; i < queue_families.size(); ++i) {
    const auto& queue_family = queue_families[i];
    if ((queue_family.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT &&
        (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
      queue_family_index_ = i;
      break;
    }
  }

  // queues
  std::vector<float> queue_priorities = {
      1.f,
  };
  std::vector<VkDeviceQueueCreateInfo> queue_infos(1);
  queue_infos[0] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_infos[0].queueFamilyIndex = queue_family_index_;
  queue_infos[0].queueCount = queue_priorities.size();
  queue_infos[0].pQueuePriorities = queue_priorities.data();

  uint32_t device_extension_count = 0;
  vkEnumerateDeviceExtensionProperties(physical_device_, NULL, &device_extension_count, NULL);
  std::vector<VkExtensionProperties> available_device_extensions(device_extension_count);
  vkEnumerateDeviceExtensionProperties(physical_device_, NULL, &device_extension_count,
                                       available_device_extensions.data());
  auto has_device_extension = [&](const char* name) {
    return std::any_of(available_device_extensions.begin(), available_device_extensions.end(),
                       [&](const VkExtensionProperties& extension) {
                         return std::strcmp(extension.extensionName, name) == 0;
                       });
  };

  std::vector<const char*> device_extensions = {
#ifdef __APPLE__
      "VK_KHR_portability_subset",
#endif
  };
  std::string subgroup_mode = RequestedSubgroupMode();
  if (has_device_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
    device_extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
  }
  if (subgroup_mode != "native" &&
      has_device_extension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) {
    device_extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
  }

  VkPhysicalDeviceSubgroupSizeControlFeatures subgroup_size_features = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
  VkPhysicalDeviceFeatures2 available_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  available_features.pNext = &subgroup_size_features;
  vkGetPhysicalDeviceFeatures2(physical_device_, &available_features);

  VkPhysicalDeviceSubgroupSizeControlFeatures enabled_subgroup_size_features = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
  if (subgroup_mode != "native") {
    enabled_subgroup_size_features.subgroupSizeControl =
        subgroup_size_features.subgroupSizeControl;
    enabled_subgroup_size_features.computeFullSubgroups =
        subgroup_size_features.computeFullSubgroups;
  }

  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  if (enabled_subgroup_size_features.subgroupSizeControl) {
    device_info.pNext = &enabled_subgroup_size_features;
  }
  device_info.queueCreateInfoCount = queue_infos.size();
  device_info.pQueueCreateInfos = queue_infos.data();
  device_info.enabledExtensionCount = device_extensions.size();
  device_info.ppEnabledExtensionNames = device_extensions.data();
  vkCreateDevice(physical_device_, &device_info, NULL, &device_);
  volkLoadDevice(device_);

  vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);

  // vma
  VmaVulkanFunctions functions = {};
  functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo allocator_info = {};
  allocator_info.physicalDevice = physical_device_;
  allocator_info.device = device_;
  allocator_info.instance = instance_;
  allocator_info.pVulkanFunctions = &functions;
  allocator_info.vulkanApiVersion = application_info.apiVersion;
  vmaCreateAllocator(&allocator_info, &allocator_);

  // commands
  VkCommandPoolCreateInfo command_pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  command_pool_info.flags =
      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  command_pool_info.queueFamilyIndex = queue_family_index_;
  vkCreateCommandPool(device_, &command_pool_info, NULL, &command_pool_);

  VkCommandBufferAllocateInfo command_buffer_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_buffer_info.commandPool = command_pool_;
  command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_info.commandBufferCount = 1;
  vkAllocateCommandBuffers(device_, &command_buffer_info, &command_buffer_);

  // fence
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(device_, &fence_info, NULL, &fence_);

  // timestamp query pool
  VkQueryPoolCreateInfo query_pool_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query_pool_info.queryCount = timestamp_count;
  vkCreateQueryPool(device_, &query_pool_info, NULL, &query_pool_);

  // sorter
  VrdxSorterCreateInfo sorter_info = {};
  sorter_info.physicalDevice = physical_device_;
  sorter_info.device = device_;

  VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size_properties = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
  VkPhysicalDeviceSubgroupProperties subgroup_properties = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties.pNext = &subgroup_properties;
  subgroup_properties.pNext = &subgroup_size_properties;
  vkGetPhysicalDeviceProperties2(physical_device_, &properties);

  const bool can_require_subgroup =
      enabled_subgroup_size_features.subgroupSizeControl &&
      (subgroup_size_properties.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
  uint32_t requested_subgroup_size = 0;
  if (subgroup_mode == "auto") {
    if (can_require_subgroup && subgroup_size_properties.minSubgroupSize <= 32 &&
        subgroup_size_properties.maxSubgroupSize >= 32) {
      requested_subgroup_size = 32;
    }
  } else if (subgroup_mode != "native") {
    char* end = nullptr;
    unsigned long value = std::strtoul(subgroup_mode.c_str(), &end, 10);
    if (end == subgroup_mode.c_str() || *end != '\0' || value > UINT32_MAX) {
      throw std::runtime_error("VRDX_SUBGROUP_SIZE must be auto, native, or a positive integer");
    }
    requested_subgroup_size = static_cast<uint32_t>(value);
    if (!can_require_subgroup ||
        requested_subgroup_size < subgroup_size_properties.minSubgroupSize ||
        requested_subgroup_size > subgroup_size_properties.maxSubgroupSize ||
        requested_subgroup_size < 32 || kWorkgroupSize % requested_subgroup_size != 0) {
      throw std::runtime_error("Requested Vulkan subgroup size is unsupported by this device");
    }
  }

  subgroup_size_ =
      requested_subgroup_size ? requested_subgroup_size : subgroup_properties.subgroupSize;
  required_subgroup_size_ = requested_subgroup_size;
  if (subgroup_size_ < 32 || kWorkgroupSize % subgroup_size_ != 0) {
    throw std::runtime_error("vk_radix_sort requires a compute subgroup size of at least 32");
  }
  sorter_info.requiredSubgroupSize = required_subgroup_size_;
  std::cout << "Vulkan subgroup: native=" << subgroup_properties.subgroupSize
            << " selected=" << subgroup_size_
            << (required_subgroup_size_ ? " (required)" : " (native)") << std::endl;
  vrdxCreateSorter(&sorter_info, &sorter_);
}

VulkanBenchmark::~VulkanBenchmark() {
  vkDeviceWaitIdle(device_);

  if (keys_.buffer) vmaDestroyBuffer(allocator_, keys_.buffer, keys_.allocation);
  if (storage_.buffer) vmaDestroyBuffer(allocator_, storage_.buffer, storage_.allocation);
  if (staging_.buffer) vmaDestroyBuffer(allocator_, staging_.buffer, staging_.allocation);

  vrdxDestroySorter(sorter_);
  vkDestroyQueryPool(device_, query_pool_, NULL);
  vkDestroyFence(device_, fence_, NULL);
  vkDestroyCommandPool(device_, command_pool_, NULL);
  vmaDestroyAllocator(allocator_);
  vkDestroyDevice(device_, NULL);
  DestroyDebugUtilsMessengerEXT(instance_, messenger_, NULL);
  vkDestroyInstance(instance_, NULL);

  volkFinalize();
}

void VulkanBenchmark::Reallocate(Buffer* buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                                 bool mapped) {
  if ((buffer->usage & usage) == usage && buffer->size >= size &&
      (mapped && buffer->map || !mapped && buffer->map == nullptr))
    return;

  if (buffer->allocation) vmaDestroyBuffer(allocator_, buffer->buffer, buffer->allocation);

  VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  VmaAllocationCreateInfo allocation_create_info = {};
  if (mapped) {
    allocation_create_info.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }
  allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO;

  VmaAllocationInfo allocation_info;
  vmaCreateBuffer(allocator_, &buffer_info, &allocation_create_info, &buffer->buffer,
                  &buffer->allocation, &allocation_info);

  buffer->usage = usage;
  buffer->size = size;
  if (mapped) buffer->map = reinterpret_cast<uint8_t*>(allocation_info.pMappedData);
}

bool VulkanBenchmark::RunSubmissionStress(uint32_t runs, uint32_t batch_size) {
  if (runs == 0 || batch_size == 0) {
    throw std::runtime_error("Submission stress runs and batch size must be positive");
  }

  auto begin_command_buffer = [&](VkCommandBuffer command_buffer,
                                  VkCommandBufferUsageFlags flags) {
    vkResetCommandBuffer(command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = flags;
    VkResult result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
      throw std::runtime_error("vkBeginCommandBuffer failed with VkResult " +
                               std::to_string(result));
    }
  };

  auto submit_and_wait = [&](const std::vector<VkCommandBuffer>& command_buffers,
                             const char* stage) {
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = static_cast<uint32_t>(command_buffers.size());
    submit.pCommandBuffers = command_buffers.data();
    VkResult result = vkQueueSubmit(queue_, 1, &submit, fence_);
    if (result != VK_SUCCESS) {
      throw std::runtime_error(std::string("vkQueueSubmit failed during ") + stage +
                               " with VkResult " + std::to_string(result));
    }
    WaitForFence(device_, fence_, stage);
    vkResetFences(device_, 1, &fence_);
  };

  auto chain_barrier = [](VkCommandBuffer command_buffer) {
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
  };

  for (uint32_t run = 0; run < runs; ++run) {
    uint32_t element_count = run % 2 == 0 ? 262145u : 65537u;
    uint32_t inout_size =
        std::max(Align(element_count * static_cast<uint32_t>(sizeof(uint32_t)), 16), 16u);

    std::vector<uint32_t> input_keys(element_count);
    std::vector<uint32_t> input_values(element_count);
    for (uint32_t i = 0; i < element_count; ++i) {
      input_keys[i] = ((i * 2654435761u) ^ (run * 2246822519u)) & 0xffu;
      input_values[i] = i;
    }

    std::vector<uint32_t> expected_keys = input_keys;
    std::sort(expected_keys.begin(), expected_keys.end());
    std::vector<std::pair<uint32_t, uint32_t>> expected_pairs(element_count);
    for (uint32_t i = 0; i < element_count; ++i) {
      expected_pairs[i] = {input_keys[i], input_values[i]};
    }
    std::stable_sort(expected_pairs.begin(), expected_pairs.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    Reallocate(&staging_, 2 * inout_size,
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    Reallocate(&keys_, 2 * inout_size,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VrdxSorterStorageRequirements requirements;
    vrdxGetSorterKeyValueStorageRequirements(sorter_, element_count, &requirements);
    Reallocate(&storage_, requirements.size, requirements.usage);

    auto upload = [&] {
      std::memcpy(staging_.map, input_keys.data(), element_count * sizeof(uint32_t));
      std::memcpy(staging_.map + inout_size, input_values.data(),
                  element_count * sizeof(uint32_t));
      vmaFlushAllocation(allocator_, staging_.allocation, 0, 2 * inout_size);

      begin_command_buffer(command_buffer_, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
      VkBufferCopy copy = {};
      copy.size = 2 * inout_size;
      vkCmdCopyBuffer(command_buffer_, staging_.buffer, keys_.buffer, 1, &copy);
      vkEndCommandBuffer(command_buffer_);
      submit_and_wait({command_buffer_}, "submission stress upload");
    };

    auto read_and_check = [&](bool key_value, const char* mode) {
      begin_command_buffer(command_buffer_, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
      VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                           nullptr);
      VkBufferCopy copy = {};
      copy.size = key_value ? 2 * inout_size : inout_size;
      vkCmdCopyBuffer(command_buffer_, keys_.buffer, staging_.buffer, 1, &copy);
      vkEndCommandBuffer(command_buffer_);
      submit_and_wait({command_buffer_}, "submission stress readback");

      vmaInvalidateAllocation(allocator_, staging_.allocation, 0, copy.size);
      const uint32_t* output_keys = reinterpret_cast<const uint32_t*>(staging_.map);
      for (uint32_t i = 0; i < element_count; ++i) {
        uint32_t expected_key = key_value ? expected_pairs[i].first : expected_keys[i];
        if (output_keys[i] != expected_key) {
          std::cerr << mode << " key mismatch at " << i << std::endl;
          return false;
        }
      }
      if (key_value) {
        const uint32_t* output_values =
            reinterpret_cast<const uint32_t*>(staging_.map + inout_size);
        for (uint32_t i = 0; i < element_count; ++i) {
          if (output_values[i] != expected_pairs[i].second) {
            std::cerr << mode << " value mismatch at " << i << std::endl;
            return false;
          }
        }
      }
      return true;
    };

    auto record_sort = [&](VkCommandBuffer command_buffer, bool key_value,
                           uint32_t repeat_count) {
      begin_command_buffer(command_buffer, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
      chain_barrier(command_buffer);
      for (uint32_t i = 0; i < repeat_count; ++i) {
        if (key_value) {
          vrdxCmdSortKeyValue(command_buffer, sorter_, element_count, keys_.buffer, 0,
                              keys_.buffer, inout_size, storage_.buffer, 0, VK_NULL_HANDLE, 0);
        } else {
          vrdxCmdSort(command_buffer, sorter_, element_count, keys_.buffer, 0,
                      storage_.buffer, 0, VK_NULL_HANDLE, 0);
        }
        if (i + 1 < repeat_count) chain_barrier(command_buffer);
      }
      vkEndCommandBuffer(command_buffer);
    };

    for (bool key_value : {false, true}) {
      const char* sort_name = key_value ? "key-value" : "keys";

      upload();
      record_sort(command_buffer_, key_value, batch_size);
      submit_and_wait({command_buffer_}, "same-command-buffer sort chain");
      if (!read_and_check(key_value, "same-command-buffer")) return false;
      std::cout << "Submission stress run " << run + 1 << "/" << runs << " " << sort_name
                << " same-command-buffer passed" << std::endl;

      upload();
      std::vector<VkCommandBuffer> command_buffers(batch_size);
      VkCommandBufferAllocateInfo allocate_info = {
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      allocate_info.commandPool = command_pool_;
      allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate_info.commandBufferCount = batch_size;
      VkResult result =
          vkAllocateCommandBuffers(device_, &allocate_info, command_buffers.data());
      if (result != VK_SUCCESS) {
        throw std::runtime_error("vkAllocateCommandBuffers failed with VkResult " +
                                 std::to_string(result));
      }
      for (VkCommandBuffer command_buffer : command_buffers) {
        record_sort(command_buffer, key_value, 1);
      }
      submit_and_wait(command_buffers, "multi-command-buffer sort chain");
      vkFreeCommandBuffers(device_, command_pool_, batch_size, command_buffers.data());
      if (!read_and_check(key_value, "multi-command-buffer")) return false;
      std::cout << "Submission stress run " << run + 1 << "/" << runs << " " << sort_name
                << " multi-command-buffer passed" << std::endl;

      upload();
      command_buffers.resize(batch_size);
      result = vkAllocateCommandBuffers(device_, &allocate_info, command_buffers.data());
      if (result != VK_SUCCESS) {
        throw std::runtime_error("vkAllocateCommandBuffers failed with VkResult " +
                                 std::to_string(result));
      }
      for (VkCommandBuffer command_buffer : command_buffers) {
        record_sort(command_buffer, key_value, 1);
      }
      for (uint32_t i = 0; i < batch_size; ++i) {
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffers[i];
        VkFence submit_fence = i + 1 == batch_size ? fence_ : VK_NULL_HANDLE;
        result = vkQueueSubmit(queue_, 1, &submit, submit_fence);
        if (result != VK_SUCCESS) {
          throw std::runtime_error("vkQueueSubmit failed during no-wait chain with VkResult " +
                                   std::to_string(result));
        }
      }
      WaitForFence(device_, fence_, "no-wait multi-submit sort chain");
      vkResetFences(device_, 1, &fence_);
      vkFreeCommandBuffers(device_, command_pool_, batch_size, command_buffers.data());
      if (!read_and_check(key_value, "no-wait-multi-submit")) return false;
      std::cout << "Submission stress run " << run + 1 << "/" << runs << " " << sort_name
                << " no-wait-multi-submit passed" << std::endl;
    }
  }

  std::cout << "Submission stress passed: subgroup=" << subgroup_size_ << " runs=" << runs
            << " batch=" << batch_size << std::endl;
  return true;
}

VulkanBenchmark::Results VulkanBenchmark::Sort(const std::vector<uint32_t>& keys) {
  uint32_t element_count = keys.size();
  uint32_t inout_size = std::max(Align(element_count * sizeof(uint32_t), 16), 16u);
  bool detailed_profiling = DetailedProfilingEnabled();

  Reallocate(&staging_, inout_size,
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  Reallocate(&keys_, inout_size,
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  VrdxSorterStorageRequirements requirements;
  vrdxGetSorterStorageRequirements(sorter_, element_count, &requirements);
  Reallocate(&storage_, requirements.size, requirements.usage);

  std::memcpy(staging_.map, keys.data(), element_count * sizeof(uint32_t));
  vmaFlushAllocation(allocator_, staging_.allocation, 0, inout_size);

  VkCommandBufferBeginInfo command_buffer_begin_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer_, &command_buffer_begin_info);

  vkCmdResetQueryPool(command_buffer_, query_pool_, 0, timestamp_count);

  VkBufferCopy region = {};
  region.srcOffset = 0;
  region.dstOffset = 0;
  region.size = element_count * sizeof(uint32_t);
  if (region.size != 0) vkCmdCopyBuffer(command_buffer_, staging_.buffer, keys_.buffer, 1, &region);

  vkEndCommandBuffer(command_buffer_);

  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer_;
  vkQueueSubmit(queue_, 1, &submit, fence_);
  WaitForFence(device_, fence_, "key upload");
  vkResetFences(device_, 1, &fence_);

  vkBeginCommandBuffer(command_buffer_, &command_buffer_begin_info);

  VkMemoryBarrier sort_before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  sort_before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  sort_before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &sort_before, 0, NULL, 0,
                       NULL);

  if (!detailed_profiling) {
    vkCmdWriteTimestamp(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, query_pool_, 0);
  }
  vrdxCmdSort(command_buffer_, sorter_, element_count, keys_.buffer, 0, storage_.buffer, 0,
              detailed_profiling ? query_pool_ : VK_NULL_HANDLE, 0);
  if (!detailed_profiling) {
    vkCmdWriteTimestamp(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, query_pool_, 1);
  }

  vkEndCommandBuffer(command_buffer_);
  auto cpu_start = std::chrono::steady_clock::now();
  vkQueueSubmit(queue_, 1, &submit, fence_);
  WaitForFence(device_, fence_, "key sort");
  auto cpu_end = std::chrono::steady_clock::now();
  vkResetFences(device_, 1, &fence_);

  vkBeginCommandBuffer(command_buffer_, &command_buffer_begin_info);

  VkMemoryBarrier sort_after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  sort_after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  sort_after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &sort_after, 0, NULL, 0, NULL);

  region.srcOffset = 0;
  region.dstOffset = 0;
  region.size = element_count * sizeof(uint32_t);
  if (region.size != 0) vkCmdCopyBuffer(command_buffer_, keys_.buffer, staging_.buffer, 1, &region);

  vkEndCommandBuffer(command_buffer_);
  vkQueueSubmit(queue_, 1, &submit, fence_);
  WaitForFence(device_, fence_, "key readback");
  vkResetFences(device_, 1, &fence_);

  size_t result_count = detailed_profiling ? timestamp_count : 2;
  std::vector<uint64_t> timestamps(result_count);
  vkGetQueryPoolResults(device_, query_pool_, 0, timestamps.size(),
                        timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
                        VK_QUERY_RESULT_64_BIT);

  Results result;
  result.keys.resize(element_count);
  vmaInvalidateAllocation(allocator_, staging_.allocation, 0, inout_size);
  std::memcpy(result.keys.data(), staging_.map, element_count * sizeof(uint32_t));
  result.total_time =
      static_cast<uint64_t>((timestamps[result_count - 1] - timestamps[0]) * timestamp_period_);
  if (detailed_profiling) FillStageTimes(timestamps, timestamp_period_, &result);
  result.cpu_time = std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_end - cpu_start).count();
  return result;
}

VulkanBenchmark::Results VulkanBenchmark::SortKeyValue(const std::vector<uint32_t>& keys,
                                                       const std::vector<uint32_t>& values) {
  uint32_t element_count = keys.size();
  uint32_t inout_size = std::max(Align(element_count * sizeof(uint32_t), 16), 16u);
  bool detailed_profiling = DetailedProfilingEnabled();

  Reallocate(&staging_, 2 * inout_size + 16,
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  Reallocate(&keys_, 2 * inout_size + 16,
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  VrdxSorterStorageRequirements requirements;
  vrdxGetSorterKeyValueStorageRequirements(sorter_, element_count, &requirements);
  Reallocate(&storage_, requirements.size, requirements.usage);

  std::memcpy(staging_.map, keys.data(), element_count * sizeof(uint32_t));
  std::memcpy(staging_.map + inout_size, values.data(), element_count * sizeof(uint32_t));
  std::memcpy(staging_.map + 2 * inout_size, &element_count, sizeof(uint32_t));
  vmaFlushAllocation(allocator_, staging_.allocation, 0, 2 * inout_size + sizeof(uint32_t));

  VkCommandBufferBeginInfo command_buffer_begin_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_buffer_, &command_buffer_begin_info);

  vkCmdResetQueryPool(command_buffer_, query_pool_, 0, timestamp_count);

  // copy to keys buffer
  VkBufferCopy region = {};
  region.srcOffset = 0;
  region.dstOffset = 0;
  region.size = 2 * inout_size + sizeof(uint32_t);
  vkCmdCopyBuffer(command_buffer_, staging_.buffer, keys_.buffer, 1, &region);

  vkEndCommandBuffer(command_buffer_);

  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer_;
  vkQueueSubmit(queue_, 1, &submit, fence_);
  WaitForFence(device_, fence_, "key-value upload");
  vkResetFences(device_, 1, &fence_);

  // sort
  vkBeginCommandBuffer(command_buffer_, &command_buffer_begin_info);

  VkMemoryBarrier sort_before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  sort_before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  sort_before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                       &sort_before, 0, NULL, 0, NULL);

  if (!detailed_profiling) {
    vkCmdWriteTimestamp(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, query_pool_, 0);
  }
  vrdxCmdSortKeyValueIndirect(command_buffer_, sorter_, element_count, keys_.buffer, 2 * inout_size,
                              keys_.buffer, 0, keys_.buffer, inout_size, storage_.buffer, 0,
                              detailed_profiling ? query_pool_ : VK_NULL_HANDLE, 0);
  if (!detailed_profiling) {
    vkCmdWriteTimestamp(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, query_pool_, 1);
  }

  vkEndCommandBuffer(command_buffer_);
  auto cpu_start = std::chrono::steady_clock::now();
  vkQueueSubmit(queue_, 1, &submit, fence_);
  WaitForFence(device_, fence_, "key-value sort");
  auto cpu_end = std::chrono::steady_clock::now();
  vkResetFences(device_, 1, &fence_);

  // copy back
  vkBeginCommandBuffer(command_buffer_, &command_buffer_begin_info);

  VkMemoryBarrier sort_after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  sort_after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  sort_after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &sort_after, 0, NULL, 0, NULL);

  region.srcOffset = 0;
  region.dstOffset = 0;
  region.size = 2 * inout_size;
  vkCmdCopyBuffer(command_buffer_, keys_.buffer, staging_.buffer, 1, &region);

  vkEndCommandBuffer(command_buffer_);
  vkQueueSubmit(queue_, 1, &submit, fence_);
  WaitForFence(device_, fence_, "key-value readback");
  vkResetFences(device_, 1, &fence_);

  size_t result_count = detailed_profiling ? timestamp_count : 2;
  std::vector<uint64_t> timestamps(result_count);
  vkGetQueryPoolResults(device_, query_pool_, 0, timestamps.size(),
                        timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
                        VK_QUERY_RESULT_64_BIT);

  Results result;
  result.keys.resize(element_count);
  result.values.resize(element_count);
  vmaInvalidateAllocation(allocator_, staging_.allocation, 0, 2 * inout_size);
  std::memcpy(result.keys.data(), staging_.map, element_count * sizeof(uint32_t));
  std::memcpy(result.values.data(), staging_.map + inout_size, element_count * sizeof(uint32_t));
  result.total_time =
      static_cast<uint64_t>((timestamps[result_count - 1] - timestamps[0]) * timestamp_period_);
  if (detailed_profiling) FillStageTimes(timestamps, timestamp_period_, &result);
  result.cpu_time = std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_end - cpu_start).count();
  return result;
}
