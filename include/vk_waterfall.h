// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifndef VK_WATERFALL_H
#define VK_WATERFALL_H

#include <stdint.h>
#ifdef USE_VULKAN
#include <vulkan/vulkan.h>
#endif

struct BandState;

int  vk_waterfall_init_global(void);
void vk_waterfall_shutdown_global(void);
int  vk_waterfall_is_ready(void);

#ifdef USE_VULKAN
VkPhysicalDevice vk_waterfall_get_pdev(void);
VkDevice         vk_waterfall_get_device(void);
VkQueue          vk_waterfall_get_queue(void);
VkCommandPool    vk_waterfall_get_cmd_pool(void);
VkFence          vk_waterfall_get_fence(void);
uint32_t         vk_waterfall_get_queue_family(void);
#endif

int  vk_waterfall_prepare_band(struct BandState *b);
void vk_waterfall_destroy_band(struct BandState *b);

void vk_waterfall_build_pyramid(struct BandState *b, const float *fft_src,
                                int fft_N, int top_px, int dc_xor);

#endif
