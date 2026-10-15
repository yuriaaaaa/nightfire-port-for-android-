// Os adaptadores de vídeo pelo Vulkan (só do port; render/gpu_adapters_vulkan.h). O índice é a posição em
// vkEnumeratePhysicalDevices; "integrado" é o VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU (no Android, todas as GPUs dos
// celulares), a memória dedicada a soma dos heaps DEVICE_LOCAL das placas dedicadas.

#include "render/gpu_adapters_vulkan.h"

#include <algorithm>
#include <cstring>

#include "render/gpu_adapters.h"

namespace nf {

AdapterInfo vulkan_adapter_info(VkPhysicalDevice device, int index) {
    AdapterInfo a;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(device, &p);
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(device, &m);
    a.index = index;
    a.name = p.deviceName;
    a.vendor_id = p.vendorID;
    a.device_id = p.deviceID;
    a.integrated = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    a.uma_known = true;
    a.software = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    uint64_t local = 0, shared = 0;
    for (uint32_t i = 0; i < m.memoryHeapCount; ++i) {
        if (m.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            local += m.memoryHeaps[i].size;
        } else {
            shared += m.memoryHeaps[i].size;
        }
    }
    if (a.integrated || a.software) {
        a.shared_memory = local + shared;
    } else {
        a.dedicated_vram = local;
        a.shared_memory = shared;
    }
    // O registryID do Metal e o LUID do DXGI: aqui o par vendor/device, só para reconhecer o mesmo adaptador.
    a.luid_low = p.deviceID;
    a.luid_high = int32_t(p.vendorID);
    a.outputs = false;
    return a;
}

void vulkan_rank_adapters(std::vector<AdapterInfo>& list) {
    std::vector<size_t> order(list.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    const auto kind = [&](size_t i, bool power) {
        const AdapterInfo& a = list[i];
        if (a.software) return 2;
        if (power) return a.integrated ? 0 : 1;
        return a.integrated ? 1 : 0;
    };
    for (const bool power : {true, false}) {
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return kind(x, power) < kind(y, power); });
        for (size_t r = 0; r < order.size(); ++r) (power ? list[order[r]].power_rank : list[order[r]].perf_rank) = int(r);
    }
}

std::vector<AdapterInfo> enumerate_adapters(bool) {
    std::vector<AdapterInfo> out;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Nightfire PC";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) return out;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n > 0) vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (uint32_t i = 0; i < n; ++i) out.push_back(vulkan_adapter_info(devs[i], int(i)));
    vulkan_rank_adapters(out);
    vkDestroyInstance(inst, nullptr);
    return out;
}

}  // namespace nf
