// Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).

#include "fft_backend.h"
#include "dsp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_requested_backend = FFT_BACKEND_FFTW;
static int g_active_backend    = FFT_BACKEND_FFTW;

#ifdef USE_VULKAN
#include <vulkan/vulkan.h>

#define VKFFT_BACKEND 0
#include <vkFFT.h>

#include "vk_waterfall.h"

static int g_vkfft_ready;
static int g_compiler_initialized;

static VkPhysicalDevice g_vk_pdev;
static VkDevice         g_vk_dev;
static VkQueue          g_vk_queue;
static VkCommandPool    g_vk_cmd_pool;
static VkFence          g_vk_fence;

typedef struct VkFFTBandPlan {
    VkFFTApplication app;
    VkBuffer         buf;
    VkDeviceMemory   mem;
    void            *ptr;
    VkDeviceSize     buf_bytes;
    size_t           in_bytes;
    size_t           out_bytes;
    int              is_r2c;
} VkFFTBandPlan;

typedef struct VkFFTDemodPlan {
    VkFFTApplication app;
    VkBuffer         buf;
    VkDeviceMemory   mem;
    void            *ptr;
    VkDeviceSize     buf_bytes;
    size_t           in_bytes;
    size_t           out_bytes;
    int              is_c2r;
} VkFFTDemodPlan;

static uint32_t find_memory_type(VkPhysicalDevice pdev, uint32_t type_filter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

static int alloc_mapped_buffer(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, void **ptr)
{
    VkDevice dev = vk_waterfall_get_device();
    VkPhysicalDevice pdev = vk_waterfall_get_pdev();

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bci, NULL, buf) != VK_SUCCESS) return -1;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *buf, &mr);

    uint32_t mi = find_memory_type(pdev, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi == UINT32_MAX) {
        vkDestroyBuffer(dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return -1;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mi,
    };
    if (vkAllocateMemory(dev, &mai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return -1;
    }
    vkBindBufferMemory(dev, *buf, *mem, 0);
    vkMapMemory(dev, *mem, 0, size, 0, ptr);
    return 0;
}

static void free_mapped_buffer(VkBuffer *buf, VkDeviceMemory *mem, void **ptr)
{
    VkDevice dev = vk_waterfall_get_device();
    if (*ptr && *mem) { vkUnmapMemory(dev, *mem); *ptr = NULL; }
    if (*buf)  { vkDestroyBuffer(dev, *buf, NULL); *buf = VK_NULL_HANDLE; }
    if (*mem)  { vkFreeMemory(dev, *mem, NULL); *mem = VK_NULL_HANDLE; }
}

static int create_vkfft_plan(VkFFTBandPlan *p, size_t fft_len, int r2c, int inverse)
{
    memset(p, 0, sizeof(*p));
    p->is_r2c = r2c;

    if (r2c) {
        p->in_bytes  = sizeof(float) * fft_len;
        p->out_bytes = sizeof(float) * 2 * (fft_len / 2 + 1);
    } else {
        p->in_bytes  = sizeof(float) * 2 * fft_len;
        p->out_bytes = sizeof(float) * 2 * fft_len;
    }
    p->buf_bytes = (p->in_bytes > p->out_bytes ? p->in_bytes : p->out_bytes);

    if (alloc_mapped_buffer(p->buf_bytes, &p->buf, &p->mem, &p->ptr) != 0)
        return -1;

    VkFFTConfiguration cfg = VKFFT_ZERO_INIT;
    cfg.FFTdim = 1;
    cfg.size[0] = (uint64_t)fft_len;
    cfg.physicalDevice = &g_vk_pdev;
    cfg.device = &g_vk_dev;
    cfg.queue = &g_vk_queue;
    cfg.commandPool = &g_vk_cmd_pool;
    cfg.fence = &g_vk_fence;
    if (!g_compiler_initialized) {
        cfg.isCompilerInitialized = 0;
        g_compiler_initialized = 1;
    } else {
        cfg.isCompilerInitialized = 1;
    }

    if (r2c)
        cfg.performR2C = 1;

    if (inverse)
        cfg.makeInversePlanOnly = 1;
    else
        cfg.makeForwardPlanOnly = 1;

    cfg.bufferNum = 1;
    uint64_t buf_sz = (uint64_t)p->buf_bytes;
    cfg.bufferSize = &buf_sz;
    cfg.buffer = &p->buf;

    VkFFTResult res = initializeVkFFT(&p->app, cfg);
    if (res != VKFFT_SUCCESS) {
        fprintf(stderr, "vkfft: initializeVkFFT failed (err=%d) for len=%zu r2c=%d inv=%d\n",
                (int)res, fft_len, r2c, inverse);
        free_mapped_buffer(&p->buf, &p->mem, &p->ptr);
        return -1;
    }

    return 0;
}

static void destroy_vkfft_plan(VkFFTBandPlan *p)
{
    if (!p) return;
    deleteVkFFT(&p->app);
    free_mapped_buffer(&p->buf, &p->mem, &p->ptr);
}

static int execute_vkfft(VkFFTBandPlan *p, const void *host_in, size_t in_bytes,
                         void *host_out, size_t out_bytes, int inverse)
{
    memcpy(p->ptr, host_in, in_bytes);

    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_vk_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(g_vk_dev, &ai, &cmd) != VK_SUCCESS) return -1;

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkFFTLaunchParams lp = VKFFT_ZERO_INIT;
    lp.commandBuffer = &cmd;
    lp.buffer = &p->buf;

    VkFFTResult res = VkFFTAppend(&p->app, inverse ? 1 : -1, &lp);
    if (res != VKFFT_SUCCESS) {
        vkEndCommandBuffer(cmd);
        vkFreeCommandBuffers(g_vk_dev, g_vk_cmd_pool, 1, &cmd);
        return -1;
    }

    vkEndCommandBuffer(cmd);

    vkResetFences(g_vk_dev, 1, &g_vk_fence);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    if (vkQueueSubmit(g_vk_queue, 1, &si, g_vk_fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_vk_dev, g_vk_cmd_pool, 1, &cmd);
        return -1;
    }
    vkWaitForFences(g_vk_dev, 1, &g_vk_fence, VK_TRUE, UINT64_MAX);

    vkFreeCommandBuffers(g_vk_dev, g_vk_cmd_pool, 1, &cmd);

    memcpy(host_out, p->ptr, out_bytes);
    return 0;
}

#endif

const char *fft_backend_name(int backend)
{
    return (backend == FFT_BACKEND_VKFFT) ? "vkfft" : "fftw";
}

void fft_backend_set_requested(int backend)
{
    g_requested_backend = (backend == FFT_BACKEND_VKFFT)
                          ? FFT_BACKEND_VKFFT : FFT_BACKEND_FFTW;
}

int fft_backend_get_active(void)
{
    return g_active_backend;
}

int fft_backend_init_global(void)
{
    g_active_backend = FFT_BACKEND_FFTW;

    if (g_requested_backend != FFT_BACKEND_VKFFT)
        return 0;

#ifndef USE_VULKAN
    fprintf(stderr, "fftbackend=vkfft requested but Vulkan not enabled; falling back to fftw.\n");
    return 0;
#else
    if (!vk_waterfall_is_ready()) {
        fprintf(stderr, "vkfft: Vulkan not initialized; falling back to fftw.\n");
        return 0;
    }

    g_vk_pdev     = vk_waterfall_get_pdev();
    g_vk_dev      = vk_waterfall_get_device();
    g_vk_queue    = vk_waterfall_get_queue();
    g_vk_cmd_pool = vk_waterfall_get_cmd_pool();

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (vkCreateFence(g_vk_dev, &fci, NULL, &g_vk_fence) != VK_SUCCESS) {
        fprintf(stderr, "vkfft: failed to create fence; falling back to fftw.\n");
        return 0;
    }

    g_vkfft_ready = 1;
    g_compiler_initialized = 0;
    g_active_backend = FFT_BACKEND_VKFFT;
    fprintf(stderr, "vkfft: GPU FFT backend enabled (sharing Vulkan device)\n");
    return 0;
#endif
}

void fft_backend_shutdown_global(void)
{
#ifdef USE_VULKAN
    if (g_vk_fence && g_vk_dev) {
        vkDeviceWaitIdle(g_vk_dev);
        vkDestroyFence(g_vk_dev, g_vk_fence, NULL);
        g_vk_fence = VK_NULL_HANDLE;
    }
    g_vkfft_ready = 0;
    g_compiler_initialized = 0;
#endif
    g_active_backend = FFT_BACKEND_FFTW;
}

int fft_backend_prepare_band(struct BandState *b, int fftlen, int fftlen2,
                             unsigned int fftw_flags)
{
    (void)fftw_flags;
    b->fft_backend = FFT_BACKEND_FFTW;
    b->vkfft_fwd  = NULL;
    b->vkfft_fwd2 = NULL;

#ifndef USE_VULKAN
    (void)fftlen; (void)fftlen2;
    return -1;
#else
    if (g_active_backend != FFT_BACKEND_VKFFT || !g_vkfft_ready)
        return -1;

    VkFFTBandPlan *pfwd = calloc(1, sizeof(VkFFTBandPlan));
    if (!pfwd) return -1;

    int r2c = b->noniq ? 1 : 0;
    size_t fwd_len  = r2c ? (size_t)(2 * fftlen)  : (size_t)fftlen;
    size_t fwd2_len = r2c ? (size_t)(2 * fftlen2) : (size_t)fftlen2;

    if (create_vkfft_plan(pfwd, fwd_len, r2c, 0) != 0) {
        free(pfwd);
        return -1;
    }

    VkFFTBandPlan *pfwd2 = calloc(1, sizeof(VkFFTBandPlan));
    if (!pfwd2) {
        destroy_vkfft_plan(pfwd); free(pfwd);
        return -1;
    }
    if (create_vkfft_plan(pfwd2, fwd2_len, r2c, 0) != 0) {
        destroy_vkfft_plan(pfwd); free(pfwd);
        free(pfwd2);
        return -1;
    }

    b->vkfft_fwd  = pfwd;
    b->vkfft_fwd2 = pfwd2;
    b->fft_backend = FFT_BACKEND_VKFFT;
    return 0;
#endif
}

void fft_backend_destroy_band(struct BandState *b)
{
#ifdef USE_VULKAN
    if (b->vkfft_fwd) {
        if (g_vk_dev) vkDeviceWaitIdle(g_vk_dev);
        destroy_vkfft_plan((VkFFTBandPlan *)b->vkfft_fwd);
        free(b->vkfft_fwd);
        b->vkfft_fwd = NULL;
    }
    if (b->vkfft_fwd2) {
        destroy_vkfft_plan((VkFFTBandPlan *)b->vkfft_fwd2);
        free(b->vkfft_fwd2);
        b->vkfft_fwd2 = NULL;
    }
#else
    (void)b;
#endif
}

void fft_backend_execute_plan_fwd(struct BandState *b)
{
    if (b->fft_backend != FFT_BACKEND_VKFFT) {
        fftwf_execute(b->plan_fwd);
        return;
    }

#ifdef USE_VULKAN
    VkFFTBandPlan *p = (VkFFTBandPlan *)b->vkfft_fwd;
    if (!p) { fftwf_execute(b->plan_fwd); return; }

    const void *host_in  = b->noniq ? (const void *)b->fft_in_r2c : (const void *)b->fft_in_cx;
    void       *host_out = b->noniq ? (void *)b->fft_out_r2c     : (void *)b->fft_out_cx;

    if (execute_vkfft(p, host_in, p->in_bytes, host_out, p->out_bytes, 0) != 0)
        fftwf_execute(b->plan_fwd);
#endif
}

void fft_backend_execute_plan_fwd2(struct BandState *b)
{
    if (b->fft_backend != FFT_BACKEND_VKFFT) {
        fftwf_execute(b->plan_fwd2);
        return;
    }

#ifdef USE_VULKAN
    VkFFTBandPlan *p = (VkFFTBandPlan *)b->vkfft_fwd2;
    if (!p) { fftwf_execute(b->plan_fwd2); return; }

    const void *host_in  = b->noniq ? (const void *)b->fft_in2_r2c : (const void *)b->fft_in2;
    void       *host_out = (void *)b->fft_out2;

    if (execute_vkfft(p, host_in, p->in_bytes, host_out, p->out_bytes, 0) != 0)
        fftwf_execute(b->plan_fwd2);
#endif
}

int fft_backend_prepare_demod_ifft(struct DemodTable *dt, int fft_size, int c2r)
{
    dt->fft_backend = FFT_BACKEND_FFTW;
    dt->vkfft_ifft  = NULL;

#ifndef USE_VULKAN
    (void)fft_size; (void)c2r;
    return -1;
#else
    if (g_active_backend != FFT_BACKEND_VKFFT || !g_vkfft_ready)
        return -1;

    VkFFTDemodPlan *dp = calloc(1, sizeof(VkFFTDemodPlan));
    if (!dp) return -1;

    memset(dp, 0, sizeof(*dp));
    dp->is_c2r = c2r ? 1 : 0;

    if (c2r) {
        dp->in_bytes  = sizeof(float) * 2 * (size_t)(fft_size / 2 + 1);
        dp->out_bytes = sizeof(float) * (size_t)fft_size;
    } else {
        dp->in_bytes  = sizeof(float) * 2 * (size_t)fft_size;
        dp->out_bytes = sizeof(float) * 2 * (size_t)fft_size;
    }
    dp->buf_bytes = (dp->in_bytes > dp->out_bytes ? dp->in_bytes : dp->out_bytes);

    if (alloc_mapped_buffer(dp->buf_bytes, &dp->buf, &dp->mem, &dp->ptr) != 0) {
        free(dp);
        return -1;
    }

    VkFFTConfiguration cfg = VKFFT_ZERO_INIT;
    cfg.FFTdim = 1;
    cfg.size[0] = (uint64_t)fft_size;
    cfg.physicalDevice = &g_vk_pdev;
    cfg.device = &g_vk_dev;
    cfg.queue = &g_vk_queue;
    cfg.commandPool = &g_vk_cmd_pool;
    cfg.fence = &g_vk_fence;
    cfg.isCompilerInitialized = g_compiler_initialized ? 1 : 0;
    if (!g_compiler_initialized) g_compiler_initialized = 1;
    if (c2r) cfg.performR2C = 1;
    cfg.makeInversePlanOnly = 1;
    cfg.bufferNum = 1;
    uint64_t buf_sz = (uint64_t)dp->buf_bytes;
    cfg.bufferSize = &buf_sz;
    cfg.buffer = &dp->buf;

    VkFFTResult res = initializeVkFFT(&dp->app, cfg);
    if (res != VKFFT_SUCCESS) {
        fprintf(stderr, "vkfft: demod IFFT init failed (err=%d) size=%d c2r=%d\n",
                (int)res, fft_size, c2r);
        free_mapped_buffer(&dp->buf, &dp->mem, &dp->ptr);
        free(dp);
        return -1;
    }

    dt->vkfft_ifft  = dp;
    dt->fft_backend = FFT_BACKEND_VKFFT;
    return 0;
#endif
}

void fft_backend_destroy_demod_ifft(struct DemodTable *dt)
{
#ifdef USE_VULKAN
    if (dt->vkfft_ifft) {
        if (g_vk_dev) vkDeviceWaitIdle(g_vk_dev);
        VkFFTDemodPlan *dp = (VkFFTDemodPlan *)dt->vkfft_ifft;
        deleteVkFFT(&dp->app);
        free_mapped_buffer(&dp->buf, &dp->mem, &dp->ptr);
        free(dp);
        dt->vkfft_ifft = NULL;
    }
#else
    (void)dt;
#endif
}

void fft_backend_execute_demod_ifft(struct DemodTable *dt)
{
    fftwf_execute(dt->plan);
}
