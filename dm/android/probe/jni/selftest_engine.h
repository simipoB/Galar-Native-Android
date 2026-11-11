// Galar Native self-tests for the engine's newer paths (included by selftest.c, after its helpers). Synthetic data only: no game files, no keys.
//   heap-speed       the Mali / Xclipse memory pools: CPU speed through the uncached and the cached dma-buf heap, and whether the cached one stays right with cache upkeep
//   pipeline-memory  what one pipeline costs this driver in memory and mappings (the engine keeps at most 800 on Arm's driver and phones under 7.5 GB)
//   descriptor-sets  a descriptor set allocated and written for a draw against one bound again (the reuse on drivers without push descriptors)
//   frame-generation the engine's own frame-generation shaders (gfx/nvn/framegen) on two synthetic frames: they compile, a moving picture lands halfway, a still box stays put
//   phone-hints      the performance hint session, the Android 16 workload hint, thermal status and headroom, the window frame-rate call
#include <dlfcn.h>
#include <math.h>
#include <malloc.h>
#include <time.h>
#include "framegen/fg_spv.h"

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// ---------------- heap-speed ----------------
// The heap route maps the game's memory pools from /dev/dma_heap/system-uncached: the game's own code and the engine's texture uploads then read and write
// uncached memory, slow for the CPU above all when it reads. The cached heap (system) is fast for the CPU but needs cache upkeep around CPU access
// (DMA_BUF_IOCTL_SYNC) unless the GPU snoops the CPU's caches. Measured here: CPU write / read speed through each heap, and on the cached heap whether
// the GPU and the CPU see each other's data with the sync calls and without them
struct st_dmabuf_sync { uint64_t flags; };
#define ST_DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct st_dmabuf_sync)
enum { DS_READ = 1, DS_WRITE = 2, DS_RW = 3, DS_START = 0, DS_END = 4 };
static void dsync(int fd, uint64_t f) { struct st_dmabuf_sync s = { f }; ioctl(fd, ST_DMA_BUF_IOCTL_SYNC, &s); }
static void cpu_speed(void *p, size_t bytes, double *wmbs, double *rmbs) {
    void *tmp = malloc(bytes); if (!tmp) { *wmbs = *rmbs = 0; return; }
    double t0 = now_ms(); for (int r = 0; r < 4; r++) memset(p, r, bytes);
    double t1 = now_ms(); for (int r = 0; r < 4; r++) memcpy(tmp, p, bytes);
    double t2 = now_ms(); free(tmp);
    *wmbs = t1 > t0 ? 4.0 * bytes / 1048576.0 / ((t1 - t0) / 1e3) : 0; *rmbs = t2 > t1 ? 4.0 * bytes / 1048576.0 / ((t2 - t1) / 1e3) : 0;
}
// a dma-buf of size from the heap fd, mapped (anywhere) and imported into the driver. NULL = done
static const char *heap_buf(int hfd, VkDeviceSize size, void **at, int *fd, VkBuffer *b, VkDeviceMemory *m) {
    *at = NULL; *fd = -1; *b = VK_NULL_HANDLE; *m = VK_NULL_HANDLE;
    struct st_heap_alloc d = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(hfd, ST_HEAP_IOCTL_ALLOC, &d) < 0) return "the heap refused the allocation";
    *fd = (int)d.fd;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0); if (p == MAP_FAILED) return "mapping failed"; *at = p;
    if (!s_ext_fd || !s_ext_dmabuf || !p_fdprops) return NULL;   // (the CPU side alone: speed only)
    VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (p_fdprops(s_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, *fd, &fp)) return "vkGetMemoryFdPropertiesKHR failed";
    if (vkCreateBuffer(s_dev, &bi, NULL, b)) { *b = VK_NULL_HANDLE; return "the external buffer was refused"; }
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(s_dev, *b, &rq);
    int t = mem_type(rq.memoryTypeBits & fp.memoryTypeBits, 0, 0); if (t < 0) return "no memory type takes this dma-buf";
    int dupfd = dup(*fd);
    VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dupfd };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size, .memoryTypeIndex = (uint32_t)t };
    if (vkAllocateMemory(s_dev, &ai, NULL, m)) { *m = VK_NULL_HANDLE; close(dupfd); return "the driver refused the dma-buf import"; }
    if (vkBindBufferMemory(s_dev, *b, *m, 0)) return "binding the imported memory failed";
    return NULL;
}
static void heap_free(void *at, int fd, VkBuffer b, VkDeviceMemory m, VkDeviceSize size) {
    if (b) vkDestroyBuffer(s_dev, b, NULL); if (m) vkFreeMemory(s_dev, m, NULL); if (at) munmap(at, size); if (fd >= 0) close(fd);
}
// roundtrip (selftest.c) with the cache upkeep around the CPU's accesses: before writing, after writing, before reading, after reading
static const char *roundtrip_synced(VkBuffer a, VkDeviceSize size, volatile uint32_t *view, int fd) {
    VkDeviceSize half = size / 2; hbuf rb; const char *res = NULL;
    if (hbuf_make(&rb, half, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) return "no readback buffer";
    for (int round = 0; round < 2 && !res; round++) {
        uint32_t salt = 0x7f4a7c15u * (uint32_t)(round + 3), fill = 0xBEEF0000u + (uint32_t)round;
        dsync(fd, DS_START | DS_RW); for (VkDeviceSize i = 0; i < half / 4; i++) view[i] = (uint32_t)i ^ salt; dsync(fd, DS_END | DS_RW);
        VkCommandBuffer cb = cb_begin(); VkBufferCopy cp = { 0, 0, half };
        vkCmdCopyBuffer(cb, a, rb.b, 1, &cp); vkCmdFillBuffer(cb, a, half, half, fill); barrier_host(cb);
        if (cb_submit(cb)) { res = "submission failed"; break; }
        hbuf_sync(&rb, 0); const uint32_t *rp = rb.p;
        for (VkDeviceSize i = 0; !res && i < half / 4; i++) if (rp[i] != ((uint32_t)i ^ salt)) res = "the GPU did not see the CPU's writes";
        dsync(fd, DS_START | DS_RW);
        for (VkDeviceSize i = half / 4; !res && i < size / 4; i++) if (view[i] != fill) res = "the CPU did not see the GPU's writes";
        dsync(fd, DS_END | DS_RW);
    }
    hbuf_free(&rb); return res;
}
static void t_heap_speed(void) {
    const char *N = "heap-speed"; const VkDeviceSize SZ = 32ull << 20, RT = 4ull << 20;
    static const char *heaps[2] = { "/dev/dma_heap/system-uncached", "/dev/dma_heap/system" };
    double w[2] = { 0 }, r[2] = { 0 }; int have[2] = { 0 }; const char *cached_plain = NULL, *cached_sync = NULL; int tried_rt = 0;
    for (int h = 0; h < 2; h++) {
        int hfd = open(heaps[h], O_RDONLY | O_CLOEXEC); if (hfd < 0) { O("  %s: not open to apps (errno %d)\n", heaps[h], errno); continue; }
        void *at; int fd; VkBuffer b; VkDeviceMemory m;
        g_step = h ? "measuring the cached heap" : "measuring the uncached heap";
        const char *e = heap_buf(hfd, SZ, &at, &fd, &b, &m);
        if (at) { if (h) dsync(fd, DS_START | DS_RW); cpu_speed(at, (size_t)SZ, &w[h], &r[h]); if (h) dsync(fd, DS_END | DS_RW); have[h] = 1;
                  O("  %s: CPU writes %.0f MB/s, reads %.0f MB/s%s%s\n", heaps[h], w[h], r[h], e ? " (" : "", e ? e : ""); if (e) O(")\n"); }
        heap_free(at, fd, b, m, SZ);
        if (h == 1 && s_ext_fd && s_ext_dmabuf && p_fdprops) {   // the cached heap imported: right without upkeep, and with it
            g_step = "the cached heap's GPU / CPU round trip";
            if (!(e = heap_buf(hfd, RT, &at, &fd, &b, &m))) { tried_rt = 1; cached_plain = roundtrip(b, RT, (volatile uint32_t *)at); }
            heap_free(at, fd, b, m, RT);
            if (!(e = heap_buf(hfd, RT, &at, &fd, &b, &m))) cached_sync = roundtrip_synced(b, RT, (volatile uint32_t *)at, fd);
            else if (!tried_rt) cached_sync = e;
            heap_free(at, fd, b, m, RT);
        }
        close(hfd);
    }
    if (!have[0] && !have[1]) { R(SKIP, N, "no dma-buf heap opens for an app here"); return; }
    if (!have[0] || !have[1]) { R(INFO, N, "only %s opens: CPU writes %.0f MB/s, reads %.0f MB/s", have[0] ? heaps[0] : heaps[1], have[0] ? w[0] : w[1], have[0] ? r[0] : r[1]); return; }
    double rx = r[0] > 0 ? r[1] / r[0] : 0, wx = w[0] > 0 ? w[1] / w[0] : 0;
    if (!tried_rt) { R(INFO, N, "cached heap: CPU reads %.1fx, writes %.1fx the uncached one's (the driver imports no dma-bufs: the heap route is not used here)", rx, wx); return; }
    if (cached_sync) R(WARN, N, "cached heap: CPU reads %.1fx, writes %.1fx the uncached one's, but it is wrong even with cache upkeep (%s): the engine must stay on the uncached heap here", rx, wx, cached_sync);
    else if (!cached_plain) R(PASS, N, "cached heap: CPU reads %.1fx, writes %.1fx the uncached one's, and right without cache upkeep (the GPU snoops the CPU's caches): the engine can use it as is", rx, wx);
    else R(PASS, N, "cached heap: CPU reads %.1fx, writes %.1fx the uncached one's, right with cache upkeep (without it: %s): the engine can use it with DMA_BUF_IOCTL_SYNC around CPU access", rx, wx, cached_plain);
}

// ---------------- pipeline-memory ----------------
static long rss_kb(void) { FILE *f = fopen("/proc/self/statm", "r"); long a = 0, b = 0; if (f) { if (fscanf(f, "%ld %ld", &a, &b) != 2) b = 0; fclose(f); } return b * (getpagesize() / 1024); }
static int maps_n(void) { FILE *f = fopen("/proc/self/maps", "r"); int n = 0; char ln[512]; while (f && fgets(ln, sizeof ln, f)) n++; if (f) fclose(f); return n; }
static void t_pipe_mem(void) {
    const char *N = "pipeline-memory"; enum { NP = 240 };
    VkFormat dsf = VK_FORMAT_UNDEFINED; static const VkFormat cand[2] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (int i = 0; i < 2 && !dsf; i++) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(s_pd, cand[i], &fp); if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) dsf = cand[i]; }
    if (!dsf) { R(SKIP, N, "no depth / stencil format"); return; }
    VkAttachmentDescription a[2] = {
        { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL },
        { 0, dsf, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL } };
    VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_GENERAL }, dr = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cr, .pDepthStencilAttachment = &dr };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = a, .subpassCount = 1, .pSubpasses = &sp };
    VkRenderPass rp = VK_NULL_HANDLE; VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline *pp = calloc(NP, sizeof *pp);
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof spv_fallback_vert, .pCode = spv_fallback_vert };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    if (!pp || vkCreateRenderPass(s_dev, &rci, NULL, &rp) || vkCreateShaderModule(s_dev, &mci, NULL, &vm)) { R(FAIL, N, "render pass / shaders"); goto out; }
    mci.codeSize = sizeof spv_fallback_frag; mci.pCode = spv_fallback_frag;
    if (vkCreateShaderModule(s_dev, &mci, NULL, &fm) || vkCreatePipelineLayout(s_dev, &pli, NULL, &pl)) { R(FAIL, N, "shaders / layout"); goto out; }
    {   // 240 pipelines, each with other draw state built in (cull 3 x topology 2 x depth test 2 x depth write 2 x stencil 2 x stencil compare 5)
        static const VkCullModeFlags cu[3] = { VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT };
        static const VkCompareOp cmp[5] = { VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL };
        long r0 = rss_kb(); int m0 = maps_n(); struct mallinfo mi0 = mallinfo(); double t0 = now_ms(); int made = 0;
        g_step = "building 240 pipelines";
        for (int i = 0; i < NP; i++) {
            fbs s = { cu[i % 3], VK_FRONT_FACE_COUNTER_CLOCKWISE, (i / 3) % 2 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                      (i / 6) % 2, (i / 12) % 2, (i / 24) % 2, cmp[(i / 48) % 5], VK_STENCIL_OP_KEEP };
            if ((pp[i] = fb_pipe(rp, pl, vm, fm, &s))) made++;
        }
        double t1 = now_ms(); long r1 = rss_kb(); int m1 = maps_n(); struct mallinfo mi1 = mallinfo();
        if (!made) { R(FAIL, N, "no pipeline could be built"); goto out; }
        double per = (double)(r1 - r0) / made, heap = ((double)mi1.uordblks - (double)mi0.uordblks) / 1024.0 / made, mp = (double)(m1 - m0) / made;
        O("  %d pipelines in %.0f ms (%.2f ms each): resident memory +%.1f MB (%.0f KB each), malloc +%.0f KB each, mappings +%d (%.2f each)\n",
          made, t1 - t0, (t1 - t0) / made, (r1 - r0) / 1024.0, per, heap, m1 - m0, mp);
        double game = per * 1500 / 1024.0;   // (Shield builds about 1500 at start)
        if (game > 300) R(WARN, N, "%.0f KB and %.2f mappings per pipeline with tiny shaders (the game's are bigger): ~1500 of them would hold %.0f MB or more; the engine's cap of 800 matters here", per, mp, game);
        else R(PASS, N, "%.0f KB and %.2f mappings per pipeline with tiny shaders (the game's are bigger): ~1500 of them would hold about %.0f MB", per, mp, game);
    }
out:
    if (pp) { for (int i = 0; i < NP; i++) if (pp[i]) vkDestroyPipeline(s_dev, pp[i], NULL); free(pp); }
    if (pl) vkDestroyPipelineLayout(s_dev, pl, NULL); if (vm) vkDestroyShaderModule(s_dev, vm, NULL); if (fm) vkDestroyShaderModule(s_dev, fm, NULL); if (rp) vkDestroyRenderPass(s_dev, rp, NULL);
}

// ---------------- descriptor-sets ----------------
static void t_desc_cost(void) {
    const char *N = "descriptor-sets"; enum { NS = 2000, NUB = 4, NTX = 4 };
    VkDescriptorSetLayoutBinding b[NUB + NTX];
    for (int i = 0; i < NUB + NTX; i++) b[i] = (VkDescriptorSetLayoutBinding){ (uint32_t)i, i < NUB ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = NUB + NTX, .pBindings = b };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkDescriptorPool dp = VK_NULL_HANDLE; hbuf ub = { 0 };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory im = VK_NULL_HANDLE; VkImageView iv = VK_NULL_HANDLE; VkSampler smp = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(s_dev, &li, NULL, &dsl)) { R(FAIL, N, "set layout"); return; }
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NS * NUB }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NS * NTX } };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = NS, .poolSizeCount = 2, .pPoolSizes = ps };
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 4, 4, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_SAMPLED_BIT };
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR };
    if (vkCreatePipelineLayout(s_dev, &pli, NULL, &pl) || vkCreateDescriptorPool(s_dev, &pci, NULL, &dp) || vkCreateImage(s_dev, &ii, NULL, &img) || !(im = image_mem(img)) ||
        vkCreateSampler(s_dev, &si, NULL, &smp) || hbuf_make(&ub, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "objects"); goto out; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(s_dev, &vi, NULL, &iv)) { R(FAIL, N, "image view"); goto out; }
    {
        VkDescriptorSet *sets = calloc(NS, sizeof *sets); VkDescriptorBufferInfo bi[NUB]; VkDescriptorImageInfo ti[NTX]; VkWriteDescriptorSet w[NUB + NTX];
        for (int k = 0; k < NTX; k++) ti[k] = (VkDescriptorImageInfo){ smp, iv, VK_IMAGE_LAYOUT_GENERAL };
        g_step = "allocating and writing 2000 descriptor sets";
        double t0 = now_ms(); int ok = 0;
        for (int i = 0; sets && i < NS; i++) {   // what a draw costs on the descriptor-set path: one set, its 8 writes (uniform offsets differ from draw to draw)
            VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
            if (vkAllocateDescriptorSets(s_dev, &ai, &sets[i])) break;
            for (int k = 0; k < NUB; k++) bi[k] = (VkDescriptorBufferInfo){ ub.b, (VkDeviceSize)((i * NUB + k) % 256) * 256, 256 };
            for (int k = 0; k < NUB + NTX; k++) w[k] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i], .dstBinding = (uint32_t)k, .descriptorCount = 1,
                .descriptorType = k < NUB ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pBufferInfo = k < NUB ? &bi[k] : NULL, .pImageInfo = k < NUB ? NULL : &ti[k - NUB] };
            vkUpdateDescriptorSets(s_dev, NUB + NTX, w, 0, NULL); ok++;
        }
        double t1 = now_ms();
        VkCommandBuffer cb = cb_begin(); double t2 = now_ms();
        for (int i = 0; cb && ok && i < NS; i++) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[0], 0, NULL);   // a set already written, bound again
        double t3 = now_ms();
        if (cb) { vkEndCommandBuffer(cb); vkFreeCommandBuffers(s_dev, s_cp, 1, &cb); }
        free(sets);
        if (!ok) { R(FAIL, N, "no descriptor set could be allocated"); goto out; }
        double nw = (t1 - t0) * 1000.0 / ok, rb = (t3 - t2) * 1000.0 / NS;
        R(INFO, N, "a new set with 8 writes %.1f us, binding one already written %.2f us (%.0fx): about %.1f ms a frame saved per 1000 draws that repeat a set",
          nw, rb, rb > 0 ? nw / rb : 0, (nw - rb) / 1000.0 * 1000.0);
    }
out:
    if (iv) vkDestroyImageView(s_dev, iv, NULL); if (img) vkDestroyImage(s_dev, img, NULL); if (im) vkFreeMemory(s_dev, im, NULL); if (smp) vkDestroySampler(s_dev, smp, NULL);
    hbuf_free(&ub); if (dp) vkDestroyDescriptorPool(s_dev, dp, NULL); if (pl) vkDestroyPipelineLayout(s_dev, pl, NULL); if (dsl) vkDestroyDescriptorSetLayout(s_dev, dsl, NULL);
}

// ---------------- frame-generation ----------------
// The engine's shaders (gfx/nvn/framegen, the same SPIR-V) on two 1280x720 frames: random 16-pixel cells, the second one moved 16 pixels to the right (480 pixels a
// second at 30 fps), and a white box that stays where it is (a HUD). The middle frame should show the cells moved 8 pixels and the box untouched
static uint16_t f2h(float f) { return f >= 1.0f ? 0x3C00 : f <= 0.0f ? 0x0000 : (uint16_t)0x3800; }   // (0, 0.5, 1 only)
static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1f, m = h & 0x3ff, u;
    if (!e) u = s | (m ? 0 : 0); else if (e == 31) u = s | 0x7f800000u | (m << 13); else u = s | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return e ? f : 0.0f;
}
static float fg_src(int frame, int x, int y) {   // the synthetic picture: frame 1 is frame 0 moved 16 pixels right; the box stays
    if (x >= 64 && x < 384 && y >= 32 && y < 96) return 1.0f;
    int sx = x - 16 * frame; if (sx < 0) sx += 4096;
    uint32_t c = (uint32_t)(sx / 16) * 73856093u ^ (uint32_t)(y / 16) * 19349663u; c ^= c >> 13; c *= 0x5bd1e995u; c ^= c >> 15;
    return (c & 1) ? 1.0f : 0.0f;
}
typedef struct { VkImage i; VkDeviceMemory m; VkImageView v; } fgimg;
static int fgimg_make(fgimg *g, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags u) {
    memset(g, 0, sizeof *g);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .usage = u };
    if (vkCreateImage(s_dev, &ci, NULL, &g->i) || !(g->m = image_mem(g->i))) return -1;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g->i, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    return vkCreateImageView(s_dev, &vi, NULL, &g->v) ? -1 : 0;
}
static void fgimg_free(fgimg *g) { if (g->v) vkDestroyImageView(s_dev, g->v, NULL); if (g->i) vkDestroyImage(s_dev, g->i, NULL); if (g->m) vkFreeMemory(s_dev, g->m, NULL); memset(g, 0, sizeof *g); }
static void fg_set(comp *c, VkSampler s, const VkImageView v[4], int storage) {
    VkDescriptorImageInfo ii[4]; VkWriteDescriptorSet w[4];
    for (int b = 0; b < 4; b++) { ii[b] = (VkDescriptorImageInfo){ b == storage ? VK_NULL_HANDLE : s, v[b], VK_IMAGE_LAYOUT_GENERAL };
        w[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = (uint32_t)b, .descriptorCount = 1,
            .descriptorType = b == storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[b] }; }
    vkUpdateDescriptorSets(s_dev, 4, w, 0, NULL);
}
static void t_framegen(void) {
    const char *N = "frame-generation"; const uint32_t W = 1280, H = 720, QW = (W + 3) / 4, QH = (H + 3) / 4, RW = (QW + 3) / 4, RH = (QH + 3) / 4, MW = (QW + 3) / 4, MH = (QH + 3) / 4, CW = (RW + 3) / 4, CH = (RH + 3) / 4;
    const VkFormat F = VK_FORMAT_R16G16B16A16_SFLOAT, U8 = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags S = VK_IMAGE_USAGE_SAMPLED_BIT, T = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ST = VK_IMAGE_USAGE_STORAGE_BIT;
    fgimg h[2], q[2], r[2], mc, m, mid; comp cc = { 0 }, cf = { 0 }, ci = { 0 }; hbuf up = { 0 }, rb = { 0 }; VkSampler smp = VK_NULL_HANDLE; VkQueryPool qp = VK_NULL_HANDLE; VkResult res;
    memset(h, 0, sizeof h); memset(q, 0, sizeof q); memset(r, 0, sizeof r); memset(&mc, 0, sizeof mc); memset(&m, 0, sizeof m); memset(&mid, 0, sizeof mid);
    for (int k = 0; k < 2; k++) if (fgimg_make(&h[k], F, W, H, S | T) || fgimg_make(&q[k], U8, QW, QH, S | T) || fgimg_make(&r[k], U8, RW, RH, S | T)) { R(FAIL, N, "images"); goto out; }
    if (fgimg_make(&mc, F, CW, CH, S | ST) || fgimg_make(&m, F, MW, MH, S | ST) || fgimg_make(&mid, F, W, H, S | T | ST)) { R(FAIL, N, "storage images (RGBA16F)"); goto out; }
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    if (vkCreateSampler(s_dev, &sci, NULL, &smp)) { R(FAIL, N, "sampler"); goto out; }
    VkDescriptorType tm[4] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
    VkDescriptorType ti4[4] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
    g_step = "building the frame-generation pipelines";
    if ((res = comp_make(&cc, fg_motion_spv, sizeof fg_motion_spv / 4, tm, 4, 32)) || (res = comp_make(&cf, fg_motion_spv, sizeof fg_motion_spv / 4, tm, 4, 32)) ||
        (res = comp_make(&ci, fg_interp_spv, sizeof fg_interp_spv / 4, ti4, 4, 24))) { R(FAIL, N, "the driver refused a frame-generation shader (%d): Frame generation stays off on this phone", res); goto out; }
    { VkImageView v1[4] = { r[0].v, r[1].v, mc.v, m.v }, v2[4] = { q[0].v, q[1].v, m.v, mc.v }, v3[4] = { h[0].v, h[1].v, m.v, mid.v };
      fg_set(&cc, smp, v1, 2); fg_set(&cf, smp, v2, 2); fg_set(&ci, smp, v3, 3); }
    // the two frames, uploaded
    VkDeviceSize fb = (VkDeviceSize)W * H * 8;
    if (hbuf_make(&up, fb * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0 || hbuf_make(&rb, fb, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) < 0) { R(FAIL, N, "buffers"); goto out; }
    for (int k = 0; k < 2; k++) { uint16_t *p = (uint16_t *)up.p + (size_t)k * W * H * 4;
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) { uint16_t v = f2h(fg_src(k, (int)x, (int)y)); uint16_t *t = p + ((size_t)y * W + x) * 4; t[0] = t[1] = t[2] = v; t[3] = 0x3C00; } }
    hbuf_sync(&up, 1);
    {
        uint32_t tsb = 0; { uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nq, NULL); VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nq, qf); tsb = qf[s_qf].timestampValidBits; }
        if (tsb && s_props.limits.timestampPeriod > 0) { VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 }; if (vkCreateQueryPool(s_dev, &qi, NULL, &qp)) qp = VK_NULL_HANDLE; }
        VkCommandBuffer cb = cb_begin(); VkImageMemoryBarrier ib[11]; fgimg *all[11] = { &h[0], &h[1], &q[0], &q[1], &r[0], &r[1], &mc, &m, &mid, NULL, NULL }; int na = 9;
        for (int i = 0; i < na; i++) ib[i] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = all[i]->i, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, (uint32_t)na, ib);
        for (int k = 0; k < 2; k++) { VkBufferImageCopy bc = { (VkDeviceSize)k * fb, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
            vkCmdCopyBufferToImage(cb, up.b, h[k].i, VK_IMAGE_LAYOUT_GENERAL, 1, &bc); }
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        if (qp) { vkCmdResetQueryPool(cb, qp, 0, 2); vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0); }
        // the engine's fg_record from here: the quarter and 1/16 copies, coarse then fine motion, the middle frame
        for (int k = 0; k < 2; k++) {
            VkImageBlit b2 = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)W, (int32_t)H, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)QW, (int32_t)QH, 1}} };
            vkCmdBlitImage(cb, h[k].i, VK_IMAGE_LAYOUT_GENERAL, q[k].i, VK_IMAGE_LAYOUT_GENERAL, 1, &b2, VK_FILTER_LINEAR);
        }
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (int k = 0; k < 2; k++) {
            VkImageBlit b3 = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)QW, (int32_t)QH, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)RW, (int32_t)RH, 1}} };
            vkCmdBlitImage(cb, q[k].i, VK_IMAGE_LAYOUT_GENERAL, r[k].i, VK_IMAGE_LAYOUT_GENERAL, 1, &b3, VK_FILTER_LINEAR);
        }
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        struct { int32_t lw, lh, bw, bh, radius; float scale, zbias, use_pred; } pm1 = { (int32_t)RW, (int32_t)RH, (int32_t)CW, (int32_t)CH, 6, (float)W / RW, 0.003f, 0.0f },
                                                                                 pm2 = { (int32_t)QW, (int32_t)QH, (int32_t)MW, (int32_t)MH, 2, (float)W / QW, 0.003f, 1.0f };
        comp *cs[2] = { &cc, &cf }; void *pcs[2] = { &pm1, &pm2 }; uint32_t gx[2] = { (CW + 7) / 8, (MW + 7) / 8 }, gy[2] = { (CH + 7) / 8, (MH + 7) / 8 };
        for (int k = 0; k < 2; k++) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cs[k]->pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cs[k]->pl, 0, 1, &cs[k]->ds, 0, NULL);
            vkCmdPushConstants(cb, cs[k]->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pcs[k]); vkCmdDispatch(cb, gx[k], gy[k], 1);
            barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        struct { int32_t w, h, mw, mh; float thresh, block; } pi = { (int32_t)W, (int32_t)H, (int32_t)MW, (int32_t)MH, 0.06f, (float)W / MW };
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ci.pipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ci.pl, 0, 1, &ci.ds, 0, NULL);
        vkCmdPushConstants(cb, ci.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 24, &pi); vkCmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
        if (qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy bc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        vkCmdCopyImageToBuffer(cb, mid.i, VK_IMAGE_LAYOUT_GENERAL, rb.b, 1, &bc); barrier_host(cb);
        g_step = "running the frame-generation shaders";
        if ((res = cb_submit(cb))) { R(FAIL, N, "the frame-generation work did not finish (%d)", res); goto out; }
    }
    hbuf_sync(&rb, 0);
    {
        const uint16_t *o = rb.p; long box_ok = 0, box_n = 0, mv_ok = 0, mv_n = 0, held = 0;
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
            float v = h2f(o[((size_t)y * W + x) * 4]);
            int in_box = x >= 64 && x < 384 && y >= 32 && y < 96, near_box = x >= 24 && x < 424 && y >= 0 && y < 136;
            if (in_box) { box_n++; if (fabsf(v - 1.0f) < 0.1f) box_ok++; }
            else if (!near_box && x >= 48 && x < W - 48 && y >= 16 && y < H - 16) {   // the cells, away from the box and the edges: frame 0 moved 8 pixels
                mv_n++; float want = fg_src(0, (int)x - 8, (int)y), still = fg_src(0, (int)x, (int)y);
                if (fabsf(v - want) < 0.25f) mv_ok++; else if (fabsf(v - still) < 0.25f) held++;
            }
        }
        double ms = 0; uint64_t ts[2];
        if (qp && !vkGetQueryPoolResults(s_dev, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) && ts[1] > ts[0]) ms = (double)(ts[1] - ts[0]) * s_props.limits.timestampPeriod / 1e6;
        double pb = box_n ? 100.0 * box_ok / box_n : 0, pm = mv_n ? 100.0 * mv_ok / mv_n : 0;
        O("  middle frame: still box %.1f %% unchanged; moving cells %.1f %% where they belong halfway (%.1f %% left where they were)\n", pb, pm, mv_n ? 100.0 * held / mv_n : 0);
        if (ms > 0) O("  GPU time for one middle frame at 1280x720 (copies, motion, interpolation): %.2f ms\n", ms);
        if (pb >= 99 && pm >= 90) R(PASS, N, "the shaders run here and the middle frame is right (box %.1f %%, motion %.1f %%)%s%.2f ms a frame", pb, pm, ms > 0 ? "; " : "", ms);
        else if (pb >= 99 && pm >= 60) R(WARN, N, "the shaders run, but only %.1f %% of the moving picture lands halfway (box %.1f %%): smearing to expect on motion", pm, pb);
        else R(FAIL, N, "the middle frame is wrong here (box %.1f %% unchanged, motion %.1f %%): Frame generation would show artefacts on this GPU", pb, pm);
    }
out:
    if (qp) vkDestroyQueryPool(s_dev, qp, NULL);
    comp_free(&cc); comp_free(&cf); comp_free(&ci); hbuf_free(&up); hbuf_free(&rb); if (smp) vkDestroySampler(s_dev, smp, NULL);
    for (int k = 0; k < 2; k++) { fgimg_free(&h[k]); fgimg_free(&q[k]); fgimg_free(&r[k]); } fgimg_free(&mc); fgimg_free(&m); fgimg_free(&mid);
}

// ---------------- phone-hints ----------------
static void t_phone_hints(void) {
    const char *N = "phone-hints";
    void *a = dlopen("libandroid.so", RTLD_NOW), *nw = dlopen("libnativewindow.so", RTLD_NOW);
    void *(*mgr)(void) = a ? (void *(*)(void))dlsym(a, "APerformanceHint_getManager") : NULL;
    void *(*create)(void *, const int32_t *, size_t, int64_t) = a ? (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(a, "APerformanceHint_createSession") : NULL;
    void (*close_s)(void *) = a ? (void (*)(void *))dlsym(a, "APerformanceHint_closeSession") : NULL;
    int spike = a && dlsym(a, "APerformanceHint_notifyWorkloadSpike"), dur2 = a && dlsym(a, "APerformanceHint_reportActualWorkDuration2");
    int rate2 = nw && dlsym(nw, "ANativeWindow_setFrameRateWithChangeStrategy"), rate1 = nw && dlsym(nw, "ANativeWindow_setFrameRate");
    void *(*th_acq)(void) = a ? (void *(*)(void))dlsym(a, "AThermal_acquireManager") : NULL;
    int (*th_st)(void *) = a ? (int (*)(void *))dlsym(a, "AThermal_getCurrentThermalStatus") : NULL;
    float (*th_hr)(void *, int) = a ? (float (*)(void *, int))dlsym(a, "AThermal_getThermalHeadroom") : NULL;
    int session = 0;
    if (mgr && create) { void *m = mgr(); int32_t tid = gettid(); void *s = m ? create(m, &tid, 1, 33333333) : NULL; session = s != NULL; if (s && close_s) close_s(s); }
    int st = -1; float hr = -1; if (th_acq && th_st) { void *tm = th_acq(); if (tm) { st = th_st(tm); if (th_hr) hr = th_hr(tm, 10); } }
    O("  performance hint session %s; Android 16 workload hint %s; GPU time in the hint (Android 15) %s; window frame rate %s\n", session ? "made" : mgr ? "refused" : "not on this Android",
      spike ? "there" : "not there", dur2 ? "there" : "not there", rate2 ? "with change strategy (Android 12+)" : rate1 ? "plain (Android 11)" : "not there");
    if (st >= 0) O("  thermal status now %d (0 none ... 2 moderate ... 6 shutdown), headroom forecast in 10 s %.2f\n", st, hr);
    if (session) R(PASS, N, "the performance hint session works here: the engine can steer the CPU clocks (target 3/4 of the frame)%s", spike ? ", and tell loads ahead" : "");
    else R(WARN, N, "no performance hint session on this phone: the CPU clocks follow the system alone (more uneven frames)");
}
