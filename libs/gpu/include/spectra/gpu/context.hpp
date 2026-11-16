// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan.h>

namespace spectra::gpu {

// Vendor-neutral Vulkan compute context. Everything runs through storage
// buffers of 32-bit floats and compute pipelines; nothing here depends on a
// particular GPU vendor. Device-specific tuning is keyed off DeviceInfo.
struct DeviceInfo {
    std::string name, driver;
    uint32_t vendor_id = 0, device_id = 0, api_version = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool unified_memory = false;   // device-local memory is host-visible (integrated GPUs, CPU drivers)
    uint64_t heap_bytes = 0;       // size of the largest device-local heap
    uint64_t budget_bytes = 0;     // VK_EXT_memory_budget for that heap (0 when unavailable)
    uint64_t max_buffer_bytes = 0; // maxStorageBufferRange
    bool is_software() const { return type == VK_PHYSICAL_DEVICE_TYPE_CPU; }
    bool is_intel() const { return vendor_id == 0x8086; }
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t bytes = 0;
    void* mapped = nullptr;  // non-null when host-visible
};

struct ContextOptions {
    // Substring of the device name to prefer, or an index ("0", "1", ...).
    // Empty: discrete > integrated > virtual > CPU. Env SPECTRA_GPU overrides.
    std::string device;
    bool validation = false;  // enable the Khronos validation layer when installed
};

// Generic push constants shared by every kernel.
struct PushConstants {
    uint32_t u[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float f[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

// Thrown by Context::flush() when the cancel flag was raised: the caller
// abandons the render (a newer one is waiting).
struct Cancelled : std::exception {
    const char* what() const noexcept override { return "render cancelled"; }
};

// How dispatches are grouped into queue submissions. One submit per dispatch is
// the slowest but simplest; batching removes most of the per-submit overhead.
// A batch is bounded by work (pixels touched) so that a single submission stays
// short: that keeps cancellation latency low and stays far from the operating
// system's GPU watchdog (about 2 s on Windows).
struct BatchOptions {
    uint32_t max_dispatches = 48;             // 1 = submit after every dispatch
    uint64_t max_work_pixels = 48ull << 20;   // sum over the batch of pixels touched per dispatch
};

class Context {
public:
    static std::vector<DeviceInfo> enumerate();
    explicit Context(const ContextOptions& opt = {});
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const DeviceInfo& info() const { return info_; }
    VkDevice device() const { return device_; }

    // Buffers. Sizes are in floats (4 bytes each).
    Buffer create(size_t n_floats);
    void destroy(Buffer& b);
    void upload(const Buffer& b, const float* data, size_t n_floats, size_t offset_floats = 0);
    void download(const Buffer& b, float* data, size_t n_floats, size_t offset_floats = 0);
    uint64_t allocated_bytes() const { return allocated_; }
    uint64_t peak_allocated_bytes() const { return peak_; }
    void reset_peak() { peak_ = allocated_; }
    // Current VK_EXT_memory_budget value for the main heap (falls back to the heap size).
    uint64_t query_budget_bytes() const;

    // Small host-visible buffer for kernel parameters: written directly, never
    // through a staging copy, so creating one does not interrupt a batch.
    Buffer create_params(size_t n_floats);

    // Persistent buffers: release() keeps a buffer for the next acquire() of the
    // same size instead of returning it to the driver, so a steady stream of
    // renders at one size allocates nothing. The pool is bounded by
    // set_pool_limit(); trim_pool() empties it.
    Buffer acquire(size_t n_floats);
    void release(Buffer& b);
    void set_pool_limit(uint64_t bytes) { pool_limit_ = bytes; }
    void trim_pool();
    uint64_t pooled_bytes() const { return pooled_; }

    // Kernels are identified by name; SPIR-V is embedded at build time.
    static constexpr int kMaxBindings = 8;
    // Record a dispatch of `kernel` over groups (gx, gy, 1). Unused bindings may
    // be null. Dispatches are submitted in batches (see BatchOptions); reading a
    // buffer back, or flush(), submits what is pending and waits for it.
    // work_pixels: pixels this dispatch touches (0: gx * gy * 256).
    void dispatch(const std::string& kernel, const std::vector<const Buffer*>& buffers,
                  const PushConstants& pc, uint32_t gx, uint32_t gy, uint64_t work_pixels = 0);
    void flush();
    void set_batching(const BatchOptions& b) { flush(); batch_ = b; }
    const BatchOptions& batching() const { return batch_; }
    // While set and true, flush() throws Cancelled (after the device is idle).
    void set_cancel_flag(const std::atomic<bool>* flag) { cancel_ = flag; }
    uint64_t submit_count() const { return submits_; }
    uint64_t dispatch_count() const { return dispatches_; }
    // Compiles every kernel now instead of on first use (first-preview latency).
    void warm_up();
    // Accumulated wall time per kernel since the last reset (milliseconds).
    const std::map<std::string, double>& kernel_ms() const { return kernel_ms_; }
    void reset_timings() { kernel_ms_.clear(); }

private:
    struct Pipeline {
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    Pipeline& pipeline(const std::string& kernel);
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want) const;

    DeviceInfo info_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kMaxBatch = 256;
    std::vector<VkDescriptorSet> desc_sets_;
    BatchOptions batch_;
    bool recording_ = false;
    uint32_t pending_ = 0;
    uint64_t pending_work_ = 0, submits_ = 0, dispatches_ = 0;
    std::unordered_set<VkBuffer> pending_buffers_;
    const std::atomic<bool>* cancel_ = nullptr;
    std::multimap<size_t, Buffer> free_;
    uint64_t pooled_ = 0, pool_limit_ = 0;
    std::map<std::string, Pipeline> pipelines_;
    std::map<std::string, double> kernel_ms_;
    Buffer dummy_, staging_;
    uint32_t heap_index_ = 0;
    bool has_budget_ = false;
    uint64_t allocated_ = 0, peak_ = 0;
    void submit_and_wait();
    void ensure_staging(size_t bytes);
    Buffer create_raw(size_t bytes, bool host_visible);
};

// SPIR-V lookup (generated at build time from shaders/*.comp).
struct ShaderBlob {
    const uint32_t* words;
    size_t count;
};
ShaderBlob find_shader(const std::string& name);

}  // namespace spectra::gpu
