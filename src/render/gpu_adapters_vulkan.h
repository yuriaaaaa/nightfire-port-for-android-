#pragma once
// Os adaptadores de vídeo pelo Vulkan (só do port; o Android e o Linux com o SDL, docs/android.md): a ficha de um
// VkPhysicalDevice no formato do AdapterInfo, comum ao enumerate_adapters (gpu_adapters_vulkan.cpp) e ao renderizador
// (renderer_vulkan.cpp). O índice é a posição em vkEnumeratePhysicalDevices.

#include <vector>

#include <vulkan/vulkan.h>

#include "platform/gpu_adapter.h"

namespace nf {

AdapterInfo vulkan_adapter_info(VkPhysicalDevice device, int index);
// As ordens de menor consumo (integrados primeiro) e de alto desempenho (dedicados primeiro), como o DXGI dá.
void vulkan_rank_adapters(std::vector<AdapterInfo>& list);

}  // namespace nf
