// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#ifdef USE_VULKAN

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "band.h"
#include "vk_waterfall.h"

#include "../shaders/wf_power_spv.h"
#include "../shaders/wf_downsample_spv.h"
#include "../shaders/wf_expand_spv.h"

static VkInstance       g_instance;
static VkPhysicalDevice g_pdev;
static VkDevice         g_dev;
static uint32_t         g_queue_family = UINT32_MAX;
static VkQueue          g_queue;

static VkDescriptorSetLayout g_power_ds_layout;
static VkPipelineLayout      g_power_pipe_layout;
static VkPipeline            g_power_pipeline;

static VkDescriptorSetLayout g_down_ds_layout;
static VkPipelineLayout      g_down_pipe_layout;
static VkPipeline            g_down_pipeline;

static VkDescriptorSetLayout g_expand_ds_layout;
static VkPipelineLayout      g_expand_pipe_layout;
static VkPipeline            g_expand_pipeline;

static VkCommandPool    g_cmd_pool;
static VkFence          g_shared_fence;

static int g_vk_ready;

int  vk_waterfall_is_ready(void)           { return g_vk_ready; }
VkPhysicalDevice vk_waterfall_get_pdev(void)       { return g_pdev; }
VkDevice         vk_waterfall_get_device(void)     { return g_dev; }
VkQueue          vk_waterfall_get_queue(void)      { return g_queue; }
VkCommandPool    vk_waterfall_get_cmd_pool(void)   { return g_cmd_pool; }
VkFence          vk_waterfall_get_fence(void)      { return g_shared_fence; }
uint32_t         vk_waterfall_get_queue_family(void) { return g_queue_family; }

struct VkWfBand {
    VkBuffer       fft_buf;
    VkDeviceMemory fft_mem;
    void          *fft_ptr;
    VkDeviceSize   fft_size;

    VkBuffer       pwr_buf[WF_MAX_ZOOMS];
    VkDeviceMemory pwr_mem[WF_MAX_ZOOMS];
    void          *pwr_ptr[WF_MAX_ZOOMS];
    VkDeviceSize   pwr_size[WF_MAX_ZOOMS];
    int            n_levels;

    VkDescriptorPool  desc_pool;
    VkDescriptorSet   power_ds;
    VkDescriptorSet   down_ds[WF_MAX_ZOOMS];

    VkCommandBuffer   cmd;
    VkFence           fence;
};

static uint32_t find_memory_type(uint32_t filter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g_pdev, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

static VkShaderModule create_shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = code,
    };
    VkShaderModule m;
    if (vkCreateShaderModule(g_dev, &ci, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static int create_compute_pipeline(const uint32_t *spv, size_t spv_size,
                                   uint32_t push_size,
                                   VkDescriptorSetLayout *out_ds_layout,
                                   VkPipelineLayout *out_pipe_layout,
                                   VkPipeline *out_pipeline)
{
    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = bindings,
    };
    if (vkCreateDescriptorSetLayout(g_dev, &dslci, NULL, out_ds_layout) != VK_SUCCESS)
        return -1;

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = push_size,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = out_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pcr,
    };
    if (vkCreatePipelineLayout(g_dev, &plci, NULL, out_pipe_layout) != VK_SUCCESS)
        return -1;

    VkShaderModule sm = create_shader(spv, spv_size);
    if (sm == VK_NULL_HANDLE) return -1;

    VkComputePipelineCreateInfo cpci = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm,
            .pName  = "main",
        },
        .layout = *out_pipe_layout,
    };
    VkResult r = vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpci, NULL, out_pipeline);
    vkDestroyShaderModule(g_dev, sm, NULL);
    return r == VK_SUCCESS ? 0 : -1;
}

static int alloc_buffer(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, void **ptr)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    if (vkCreateBuffer(g_dev, &bci, NULL, buf) != VK_SUCCESS)
        return -1;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, *buf, &req);
    uint32_t mt = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        vkDestroyBuffer(g_dev, *buf, NULL);
        return -1;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(g_dev, &mai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(g_dev, *buf, NULL);
        return -1;
    }
    vkBindBufferMemory(g_dev, *buf, *mem, 0);
    vkMapMemory(g_dev, *mem, 0, size, 0, ptr);
    return 0;
}

int vk_waterfall_init_global(void)
{
    g_vk_ready = 0;

    VkApplicationInfo ai = {
        .sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "websdr-waterfall",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
    };
    if (vkCreateInstance(&ici, NULL, &g_instance) != VK_SUCCESS) {
        fprintf(stderr, "vk_waterfall: Vulkan instance creation failed.\n");
        return -1;
    }

    uint32_t dc = 0;
    vkEnumeratePhysicalDevices(g_instance, &dc, NULL);
    if (dc == 0) {
        fprintf(stderr, "vk_waterfall: no Vulkan physical devices were found.\n");
        return -1;
    }
    VkPhysicalDevice *devs = calloc(dc, sizeof(*devs));
    vkEnumeratePhysicalDevices(g_instance, &dc, devs);

    for (uint32_t d = 0; d < dc && g_queue_family == UINT32_MAX; d++) {
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qfc, NULL);
        VkQueueFamilyProperties *qfp = calloc(qfc, sizeof(*qfp));
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qfc, qfp);
        for (uint32_t q = 0; q < qfc; q++) {
            if (qfp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                g_pdev = devs[d];
                g_queue_family = q;
                break;
            }
        }
        free(qfp);
    }
    free(devs);

    if (g_queue_family == UINT32_MAX) {
        fprintf(stderr, "vk_waterfall: no compute-capable queue family was found.\n");
        return -1;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_pdev, &props);
    fprintf(stderr, "vk_waterfall: using device '%s'.\n", props.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos    = &dqci,
    };
    if (vkCreateDevice(g_pdev, &dci, NULL, &g_dev) != VK_SUCCESS) {
        fprintf(stderr, "vk_waterfall: logical device creation failed.\n");
        return -1;
    }
    vkGetDeviceQueue(g_dev, g_queue_family, 0, &g_queue);

    VkCommandPoolCreateInfo cpci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_queue_family,
    };
    if (vkCreateCommandPool(g_dev, &cpci, NULL, &g_cmd_pool) != VK_SUCCESS) {
        fprintf(stderr, "vk_waterfall: command pool creation failed.\n");
        return -1;
    }

    if (create_compute_pipeline((const uint32_t *)wf_power_spv, sizeof(wf_power_spv),
                                12, &g_power_ds_layout, &g_power_pipe_layout,
                                &g_power_pipeline) != 0) {
        fprintf(stderr, "vk_waterfall: power pipeline creation failed.\n");
        return -1;
    }

    if (create_compute_pipeline((const uint32_t *)wf_downsample_spv, sizeof(wf_downsample_spv),
                                4, &g_down_ds_layout, &g_down_pipe_layout,
                                &g_down_pipeline) != 0) {
        fprintf(stderr, "vk_waterfall: downsample pipeline creation failed.\n");
        return -1;
    }

    if (create_compute_pipeline((const uint32_t *)wf_expand_spv, sizeof(wf_expand_spv),
                                4, &g_expand_ds_layout, &g_expand_pipe_layout,
                                &g_expand_pipeline) != 0) {
        fprintf(stderr, "vk_waterfall: expand pipeline creation failed.\n");
        return -1;
    }

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                              .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    if (vkCreateFence(g_dev, &fci, NULL, &g_shared_fence) != VK_SUCCESS)
        g_shared_fence = VK_NULL_HANDLE;

    g_vk_ready = 1;
    fprintf(stderr, "vk_waterfall: global initialization complete.\n");
    return 0;
}

void vk_waterfall_shutdown_global(void)
{
    if (!g_dev) return;
    vkDeviceWaitIdle(g_dev);
    if (g_power_pipeline)    vkDestroyPipeline(g_dev, g_power_pipeline, NULL);
    if (g_power_pipe_layout) vkDestroyPipelineLayout(g_dev, g_power_pipe_layout, NULL);
    if (g_power_ds_layout)   vkDestroyDescriptorSetLayout(g_dev, g_power_ds_layout, NULL);
    if (g_down_pipeline)      vkDestroyPipeline(g_dev, g_down_pipeline, NULL);
    if (g_down_pipe_layout)   vkDestroyPipelineLayout(g_dev, g_down_pipe_layout, NULL);
    if (g_down_ds_layout)     vkDestroyDescriptorSetLayout(g_dev, g_down_ds_layout, NULL);
    if (g_expand_pipeline)    vkDestroyPipeline(g_dev, g_expand_pipeline, NULL);
    if (g_expand_pipe_layout) vkDestroyPipelineLayout(g_dev, g_expand_pipe_layout, NULL);
    if (g_expand_ds_layout)   vkDestroyDescriptorSetLayout(g_dev, g_expand_ds_layout, NULL);
    if (g_shared_fence)      vkDestroyFence(g_dev, g_shared_fence, NULL);
    if (g_cmd_pool)          vkDestroyCommandPool(g_dev, g_cmd_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    g_vk_ready = 0;
}

int vk_waterfall_prepare_band(struct BandState *b)
{
    if (!g_vk_ready) return -1;

    int mz = b->maxzoom;
    if (mz < 1 || mz >= WF_MAX_ZOOMS) return -1;
    int top = mz - 1;
    int top_px = 1024 << top;

    struct VkWfBand *vb = calloc(1, sizeof(*vb));
    if (!vb) return -1;

    int fft_n = b->fftlen > b->fftlen2 ? b->fftlen : b->fftlen2;
    if (fft_n <= 0) fft_n = b->fftlen;
    vb->fft_size = (VkDeviceSize)fft_n * 2 * sizeof(float);
    if (alloc_buffer(vb->fft_size, &vb->fft_buf, &vb->fft_mem, &vb->fft_ptr) != 0)
        goto fail;

    vb->n_levels = mz + 1;
    if (vb->n_levels > WF_MAX_ZOOMS) vb->n_levels = WF_MAX_ZOOMS;

    for (int z = 0; z < vb->n_levels; z++) {
        int px = 1024 << z;
        vb->pwr_size[z] = (VkDeviceSize)px * sizeof(float);
        if (alloc_buffer(vb->pwr_size[z], &vb->pwr_buf[z], &vb->pwr_mem[z], &vb->pwr_ptr[z]) != 0)
            goto fail;
    }

    int n_ds = 1 + (top > 0 ? top : 0);
    if (mz < WF_MAX_ZOOMS) n_ds++;

    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = (uint32_t)(n_ds * 2),
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = (uint32_t)n_ds,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    if (vkCreateDescriptorPool(g_dev, &dpci, NULL, &vb->desc_pool) != VK_SUCCESS)
        goto fail;

    {
        VkDescriptorSetAllocateInfo dsai = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = vb->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_power_ds_layout,
        };
        if (vkAllocateDescriptorSets(g_dev, &dsai, &vb->power_ds) != VK_SUCCESS)
            goto fail;
        VkDescriptorBufferInfo bi[2] = {
            { .buffer = vb->fft_buf,       .offset = 0, .range = vb->fft_size },
            { .buffer = vb->pwr_buf[top],  .offset = 0, .range = vb->pwr_size[top] },
        };
        VkWriteDescriptorSet ws[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vb->power_ds,
              .dstBinding = 0, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vb->power_ds,
              .dstBinding = 1, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] },
        };
        vkUpdateDescriptorSets(g_dev, 2, ws, 0, NULL);
    }

    int ds_idx = 0;
    if (mz < WF_MAX_ZOOMS && mz > top) {
        VkDescriptorSetAllocateInfo dsai = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = vb->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_expand_ds_layout,
        };
        if (vkAllocateDescriptorSets(g_dev, &dsai, &vb->down_ds[ds_idx]) != VK_SUCCESS)
            goto fail;
        VkDescriptorBufferInfo bi[2] = {
            { .buffer = vb->pwr_buf[top],  .offset = 0, .range = vb->pwr_size[top] },
            { .buffer = vb->pwr_buf[mz],   .offset = 0, .range = vb->pwr_size[mz] },
        };
        VkWriteDescriptorSet ws[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vb->down_ds[ds_idx],
              .dstBinding = 0, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vb->down_ds[ds_idx],
              .dstBinding = 1, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] },
        };
        vkUpdateDescriptorSets(g_dev, 2, ws, 0, NULL);
        ds_idx++;
    }

    for (int z = top - 1; z >= 0; z--) {
        VkDescriptorSetAllocateInfo dsai = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = vb->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_down_ds_layout,
        };
        if (vkAllocateDescriptorSets(g_dev, &dsai, &vb->down_ds[ds_idx]) != VK_SUCCESS)
            goto fail;
        VkDescriptorBufferInfo bi[2] = {
            { .buffer = vb->pwr_buf[z + 1], .offset = 0, .range = vb->pwr_size[z + 1] },
            { .buffer = vb->pwr_buf[z],     .offset = 0, .range = vb->pwr_size[z] },
        };
        VkWriteDescriptorSet ws[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vb->down_ds[ds_idx],
              .dstBinding = 0, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0] },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vb->down_ds[ds_idx],
              .dstBinding = 1, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1] },
        };
        vkUpdateDescriptorSets(g_dev, 2, ws, 0, NULL);
        ds_idx++;
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = g_cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(g_dev, &cbai, &vb->cmd) != VK_SUCCESS)
        goto fail;

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (vkCreateFence(g_dev, &fci, NULL, &vb->fence) != VK_SUCCESS)
        goto fail;

    b->vk_wf = vb;
    fprintf(stderr, "vk_waterfall: band prepared (top_px=%d, levels=%d).\n",
            top_px, vb->n_levels);
    return 0;

fail:
    free(vb);
    return -1;
}

void vk_waterfall_destroy_band(struct BandState *b)
{
    struct VkWfBand *vb = b->vk_wf;
    if (!vb) return;
    vkDeviceWaitIdle(g_dev);
    if (vb->fence) vkDestroyFence(g_dev, vb->fence, NULL);
    if (vb->desc_pool) vkDestroyDescriptorPool(g_dev, vb->desc_pool, NULL);
    for (int z = 0; z < vb->n_levels; z++) {
        if (vb->pwr_ptr[z]) vkUnmapMemory(g_dev, vb->pwr_mem[z]);
        if (vb->pwr_buf[z]) vkDestroyBuffer(g_dev, vb->pwr_buf[z], NULL);
        if (vb->pwr_mem[z]) vkFreeMemory(g_dev, vb->pwr_mem[z], NULL);
    }
    if (vb->fft_ptr) vkUnmapMemory(g_dev, vb->fft_mem);
    if (vb->fft_buf) vkDestroyBuffer(g_dev, vb->fft_buf, NULL);
    if (vb->fft_mem) vkFreeMemory(g_dev, vb->fft_mem, NULL);
    free(vb);
    b->vk_wf = NULL;
}

void vk_waterfall_build_pyramid(struct BandState *b, const float *fft_src,
                                int fft_N, int top_px, int dc_xor)
{
    struct VkWfBand *vb = b->vk_wf;
    if (!vb || !g_vk_ready) return;

    int mz = b->maxzoom;
    int top = mz - 1;

    vkWaitForFences(g_dev, 1, &vb->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g_dev, 1, &vb->fence);

    memcpy(vb->fft_ptr, fft_src, (size_t)fft_N * 2 * sizeof(float));

    vkResetCommandBuffer(vb->cmd, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(vb->cmd, &cbbi);

    uint32_t power_pc[3] = { (uint32_t)fft_N, (uint32_t)top_px, (uint32_t)dc_xor };
    vkCmdBindPipeline(vb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_power_pipeline);
    vkCmdBindDescriptorSets(vb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            g_power_pipe_layout, 0, 1, &vb->power_ds, 0, NULL);
    vkCmdPushConstants(vb->cmd, g_power_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, 12, power_pc);
    vkCmdDispatch(vb->cmd, ((uint32_t)top_px + 255) / 256, 1, 1);

    VkMemoryBarrier barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    int ds_idx = 0;

    if (mz < WF_MAX_ZOOMS && mz > top) {
        vkCmdPipelineBarrier(vb->cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, NULL, 0, NULL);

        uint32_t expand_pc = (uint32_t)top_px;
        vkCmdBindPipeline(vb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_expand_pipeline);
        vkCmdBindDescriptorSets(vb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                g_expand_pipe_layout, 0, 1, &vb->down_ds[ds_idx], 0, NULL);
        vkCmdPushConstants(vb->cmd, g_expand_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, 4, &expand_pc);
        vkCmdDispatch(vb->cmd, ((uint32_t)top_px + 255) / 256, 1, 1);
        ds_idx++;
    }

    for (int z = top - 1; z >= 0; z--) {
        vkCmdPipelineBarrier(vb->cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, NULL, 0, NULL);

        uint32_t px = (uint32_t)(1024 << z);
        vkCmdBindPipeline(vb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_down_pipeline);
        vkCmdBindDescriptorSets(vb->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                g_down_pipe_layout, 0, 1, &vb->down_ds[ds_idx], 0, NULL);
        vkCmdPushConstants(vb->cmd, g_down_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, 4, &px);
        vkCmdDispatch(vb->cmd, (px + 255) / 256, 1, 1);
        ds_idx++;
    }

    vkEndCommandBuffer(vb->cmd);

    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &vb->cmd,
    };
    vkQueueSubmit(g_queue, 1, &si, vb->fence);
    vkWaitForFences(g_dev, 1, &vb->fence, VK_TRUE, UINT64_MAX);

    for (int z = 0; z < vb->n_levels; z++) {
        int px = 1024 << z;
        if (b->wf_zoom_power[z] && vb->pwr_ptr[z])
            memcpy(b->wf_zoom_power[z], vb->pwr_ptr[z], (size_t)px * sizeof(float));
    }
}

#endif
