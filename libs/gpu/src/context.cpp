// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/gpu/context.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace spectra::gpu {

namespace {

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan error in ") + what + ": " + std::to_string(r));
}

VkInstance make_instance(bool validation) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "spectra";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    std::vector<const char*> layers;
    if (validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props)
            if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    VkInstance inst;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS) throw std::runtime_error("no Vulkan runtime available (vkCreateInstance failed: " + std::to_string(r) + ")");
    return inst;
}

bool has_extension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, ext.data());
    for (auto& e : ext)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

DeviceInfo describe(VkPhysicalDevice pd, uint32_t* heap_index = nullptr) {
    DeviceInfo d;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    d.name = p.deviceName;
    d.vendor_id = p.vendorID;
    d.device_id = p.deviceID;
    d.api_version = p.apiVersion;
    d.type = p.deviceType;
    d.max_buffer_bytes = p.limits.maxStorageBufferRange;
    d.driver = std::to_string(VK_VERSION_MAJOR(p.driverVersion)) + "." + std::to_string(VK_VERSION_MINOR(p.driverVersion)) +
               "." + std::to_string(VK_VERSION_PATCH(p.driverVersion));
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t best = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > d.heap_bytes) {
            d.heap_bytes = mp.memoryHeaps[i].size;
            best = i;
        }
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (mp.memoryTypes[i].heapIndex == best && (f & want) == want) d.unified_memory = true;
    }
    if (heap_index) *heap_index = best;
    return d;
}

int rank(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}

}  // namespace

std::vector<DeviceInfo> Context::enumerate() {
    VkInstance inst = make_instance(false);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    std::vector<DeviceInfo> out;
    for (auto pd : pds) out.push_back(describe(pd));
    vkDestroyInstance(inst, nullptr);
    return out;
}

Context::Context(const ContextOptions& opt) {
    instance_ = make_instance(opt.validation);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (n == 0) throw std::runtime_error("no Vulkan device found");
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    std::string want = opt.device;
    if (const char* env = std::getenv("SPECTRA_GPU")) want = env;
    int best = -1, best_rank = -1;
    for (uint32_t i = 0; i < n; ++i) {
        DeviceInfo d = describe(pds[i]);
        int r = rank(d.type);
        if (!want.empty()) {
            bool is_index = want.find_first_not_of("0123456789") == std::string::npos;
            if (is_index ? std::stoul(want) == i : d.name.find(want) != std::string::npos) r += 100;
        }
        if (r > best_rank) {
            best_rank = r;
            best = static_cast<int>(i);
        }
    }
    physical_ = pds[static_cast<size_t>(best)];
    info_ = describe(physical_, &heap_index_);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queue_family_ = i;
            found = true;
            break;
        }
    if (!found) throw std::runtime_error("the Vulkan device has no compute queue");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<const char*> exts;
    has_budget_ = has_extension(physical_, "VK_EXT_memory_budget");
    if (has_budget_) exts.push_back("VK_EXT_memory_budget");
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    info_.budget_bytes = query_budget_bytes();

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &cai, &cmd_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device_, &fci, nullptr, &fence_), "vkCreateFence");

    std::vector<VkDescriptorSetLayoutBinding> bindings(kMaxBindings);
    for (int i = 0; i < kMaxBindings; ++i) {
        bindings[i] = {};
        bindings[i].binding = static_cast<uint32_t>(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = kMaxBindings;
    lci.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device_, &plci, nullptr, &layout_), "vkCreatePipelineLayout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBindings};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    check(vkCreateDescriptorPool(device_, &dpci, nullptr, &desc_pool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = desc_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    check(vkAllocateDescriptorSets(device_, &dai, &desc_set_), "vkAllocateDescriptorSets");
    dummy_ = create(4);
}

Context::~Context() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (auto& kv : pipelines_) {
        vkDestroyPipeline(device_, kv.second.pipeline, nullptr);
        vkDestroyShaderModule(device_, kv.second.module, nullptr);
    }
    destroy(dummy_);
    destroy(staging_);
    vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
}

uint64_t Context::query_budget_bytes() const {
    if (!has_budget_) return info_.heap_bytes;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    mp.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(physical_, &mp);
    return budget.heapBudget[heap_index_] ? budget.heapBudget[heap_index_] : info_.heap_bytes;
}

uint32_t Context::find_memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

Buffer Context::create_raw(size_t bytes, bool host_visible) {
    Buffer b;
    b.bytes = std::max<size_t>(bytes, 16);
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = b.bytes;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &ci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    bool mapped = false;
    if (host_visible || info_.unified_memory) {
        type = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | host);
        if (type == UINT32_MAX && host_visible) type = find_memory_type(req.memoryTypeBits, host);
        mapped = type != UINT32_MAX;
    }
    if (type == UINT32_MAX) type = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = find_memory_type(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkResult r = vkAllocateMemory(device_, &ai, nullptr, &b.memory);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(device_, b.buffer, nullptr);
        throw std::runtime_error("out of GPU memory allocating " + std::to_string(b.bytes >> 20) + " MiB");
    }
    check(vkBindBufferMemory(device_, b.buffer, b.memory, 0), "vkBindBufferMemory");
    if (mapped) check(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    allocated_ += b.bytes;
    peak_ = std::max(peak_, allocated_);
    return b;
}

Buffer Context::create(size_t n_floats) { return create_raw(n_floats * sizeof(float), false); }

void Context::destroy(Buffer& b) {
    if (!b.buffer) return;
    if (b.mapped) vkUnmapMemory(device_, b.memory);
    vkDestroyBuffer(device_, b.buffer, nullptr);
    vkFreeMemory(device_, b.memory, nullptr);
    allocated_ -= b.bytes;
    b = Buffer{};
}

void Context::ensure_staging(size_t bytes) {
    if (staging_.bytes >= bytes) return;
    destroy(staging_);
    staging_ = create_raw(bytes, true);
}

void Context::submit_and_wait() {
    check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    check(vkResetFences(device_, 1, &fence_), "vkResetFences");
    check(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit");
    VkResult r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    if (r == VK_ERROR_DEVICE_LOST) throw std::runtime_error("Vulkan device lost");
    check(r, "vkWaitForFences");
}

void Context::upload(const Buffer& b, const float* data, size_t n, size_t offset) {
    size_t bytes = n * sizeof(float), off = offset * sizeof(float);
    if (b.mapped) {
        std::memcpy(static_cast<char*>(b.mapped) + off, data, bytes);
        return;
    }
    // Chunked through a staging buffer to bound the extra memory.
    const size_t chunk = size_t(64) << 20;
    for (size_t done = 0; done < bytes; done += chunk) {
        size_t len = std::min(chunk, bytes - done);
        ensure_staging(len);
        std::memcpy(staging_.mapped, reinterpret_cast<const char*>(data) + done, len);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
        VkBufferCopy region{0, off + done, len};
        vkCmdCopyBuffer(cmd_, staging_.buffer, b.buffer, 1, &region);
        submit_and_wait();
    }
}

void Context::download(const Buffer& b, float* data, size_t n, size_t offset) {
    size_t bytes = n * sizeof(float), off = offset * sizeof(float);
    if (b.mapped) {
        std::memcpy(data, static_cast<const char*>(b.mapped) + off, bytes);
        return;
    }
    const size_t chunk = size_t(64) << 20;
    for (size_t done = 0; done < bytes; done += chunk) {
        size_t len = std::min(chunk, bytes - done);
        ensure_staging(len);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
        VkBufferCopy region{off + done, 0, len};
        vkCmdCopyBuffer(cmd_, b.buffer, staging_.buffer, 1, &region);
        submit_and_wait();
        std::memcpy(reinterpret_cast<char*>(data) + done, staging_.mapped, len);
    }
}

Context::Pipeline& Context::pipeline(const std::string& kernel) {
    auto it = pipelines_.find(kernel);
    if (it != pipelines_.end()) return it->second;
    ShaderBlob blob = find_shader(kernel);
    if (!blob.words) throw std::runtime_error("unknown GPU kernel '" + kernel + "'");
    Pipeline p;
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = blob.count * sizeof(uint32_t);
    mci.pCode = blob.words;
    check(vkCreateShaderModule(device_, &mci, nullptr, &p.module), "vkCreateShaderModule");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = p.module;
    ci.stage.pName = "main";
    ci.layout = layout_;
    check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline), "vkCreateComputePipelines");
    return pipelines_[kernel] = p;
}

void Context::dispatch(const std::string& kernel, const std::vector<const Buffer*>& buffers,
                       const PushConstants& pc, uint32_t gx, uint32_t gy) {
    auto t0 = std::chrono::steady_clock::now();
    Pipeline& p = pipeline(kernel);
    VkDescriptorBufferInfo infos[kMaxBindings];
    VkWriteDescriptorSet writes[kMaxBindings];
    for (int i = 0; i < kMaxBindings; ++i) {
        const Buffer* b = static_cast<size_t>(i) < buffers.size() && buffers[static_cast<size_t>(i)] ? buffers[static_cast<size_t>(i)] : &dummy_;
        infos[i] = {b->buffer, 0, VK_WHOLE_SIZE};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = desc_set_;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, kMaxBindings, writes, 0, nullptr);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &desc_set_, 0, nullptr);
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &pc);
    vkCmdDispatch(cmd_, gx, gy, 1);
    submit_and_wait();
    kernel_ms_[kernel] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace spectra::gpu
