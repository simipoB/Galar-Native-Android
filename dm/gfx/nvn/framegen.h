// ================= Frame generation: 30 -> 60 on screen (included by nvn_vk.c: its present path, images and pacing) =================
// The game keeps running at 30. For every real frame the GPU makes the frame halfway between it and the one before, and the present path
// shows that one half a frame before the real one (pace_time's display times: 4 refreshes of 8.3 ms per real frame at 120 Hz, the generated
// frame at 2). No game knowledge: the motion is estimated from the two finished pictures, coarse to fine (framegen/fg_motion.comp):
//   1. on 1/16-size copies, 64x64-pixel blocks, +-96 pixels around no motion and around the previous frame's motion (a pan that keeps going is
//      followed past that);
//   2. on quarter-size copies, 16x16-pixel blocks, +-8 pixels around no motion and around step 1's result.
// The middle frame (fg_interp.comp) takes, for each pixel, the best of its own block's motion and its four neighbours' (an object and the
// background next to it keep their own), warps both frames halfway along it and averages them; where the two do not agree (uncovered
// background, smoke, particles) it shows the current frame's side, so that spot moves at 30 fps rather than twice. Whatever does not move (the
// HUD, a still camera) stays exactly as it is. The real frame is shown half a frame later than without it (the cost of any interpolation).
// On: the Frame generation setting (DM_FRAMEGEN=on, switched while playing), at 30 fps on a display whose refresh divides the frame evenly
// (60 / 120 Hz), with 7.5 GB of memory or more (three full-size RGBA16F images, ~50 MB at 1920x1080) unless DM_FRAMEGEN=force.
// DM_FRAMEGEN_RADIUS=<coarse pixels> (6: +-96 full-size pixels), DM_FRAMEGEN_THRESH=<0..1> (0.06)
#include "framegen/fg_spv.h"
volatile int dm_framegen = -1;   // -1: read DM_FRAMEGEN at the first present; 0 off, 1 on, 2 forced (android_main.c switches it from the menu)
enum { FG_H0, FG_H1, FG_Q0, FG_Q1, FG_R0, FG_R1, FG_MC, FG_M, FG_I, FG_N };   // history, quarter, 1/16 (two each), coarse motion, motion, middle frame
static struct {
    int failed, cur, have_prev, have_motion, live; uint32_t w, h, qw, qh, rw, rh, mw, mh, cw_, ch_;
    VkImage img[FG_N]; vmalloc_t mem[FG_N]; VkImageView view[FG_N]; vtex *iv;   // iv: the middle frame as a vtex, for scale_draw
    VkSampler samp; VkDescriptorSetLayout dl_m, dl_i; VkPipelineLayout pl_m, pl_i; VkPipeline pm, pi; VkDescriptorPool dp;
    VkDescriptorSet ds_c[2], ds_m[2], ds_i[2];   // per order of the two real frames: coarse motion, fine motion, middle frame
    uint64_t made, frames;
} FG;
static int fg_setting(void) {
    if (dm_framegen < 0) { const char *e = getenv("DM_FRAMEGEN"); dm_framegen = !e ? 0 : !strcmp(e, "force") ? 2 : (!strcmp(e, "on") || !strcmp(e, "1")) ? 1 : 0; }
    if (dm_framegen == 1) {   // memory: three full-size images; a phone under 7.5 GB keeps its memory for the game
        static int ok = -1; if (ok < 0) { long kb = 0; FILE *f = fopen("/proc/meminfo", "r"); char ln[128]; while (f && fgets(ln, sizeof ln, f)) if (sscanf(ln, "MemTotal: %ld kB", &kb) == 1) break; if (f) fclose(f);
            ok = kb == 0 || kb >= 7500000; if (!ok) VLOG("frame generation: off on this phone (%.1f GB of memory; DM_FRAMEGEN=force to try)", kb / 1048576.0); }
        return ok;
    }
    return dm_framegen == 2;
}
static void fg_free(void) {
    if (FG.iv) { for (int i = 0; i < FG.iv->nviews; i++) vkDestroyImageView(g_dev, FG.iv->views[i].v, NULL); free(FG.iv->views); pthread_mutex_destroy(&FG.iv->vmu); free(FG.iv); FG.iv = NULL; }
    for (int i = 0; i < FG_N; i++) { if (FG.view[i]) vkDestroyImageView(g_dev, FG.view[i], NULL); if (FG.img[i]) { vkDestroyImage(g_dev, FG.img[i], NULL); vk_free(FG.mem[i]); } FG.view[i] = VK_NULL_HANDLE; FG.img[i] = VK_NULL_HANDLE; }
    if (FG.dp) { vkDestroyDescriptorPool(g_dev, FG.dp, NULL); FG.dp = VK_NULL_HANDLE; }
    FG.w = FG.h = 0; FG.have_prev = FG.have_motion = 0;
}
static VkImage fg_image(int i, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags u) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = { w ? w : 1, h ? h : 1, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .usage = u };
    if (vkCreateImage(g_dev, &ci, NULL, &FG.img[i])) return VK_NULL_HANDLE;
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, FG.img[i], &rq); FG.mem[i] = vk_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    vkBindImageMemory(g_dev, FG.img[i], FG.mem[i].b->mem, FG.mem[i].off);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = FG.img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (vkCreateImageView(g_dev, &vi, NULL, &FG.view[i])) return VK_NULL_HANDLE;
    return FG.img[i];
}
static int fg_pipelines(void) {   // once: the sampler, the two layouts and pipelines (the motion pipeline serves both levels)
    if (FG.pm) return 1;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0 };
    if (vkCreateSampler(g_dev, &sci, NULL, &FG.samp)) return 0;
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ST = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding bm[4] = { { 0, CIS, 1, C, NULL }, { 1, CIS, 1, C, NULL }, { 2, ST, 1, C, NULL }, { 3, CIS, 1, C, NULL } };
    VkDescriptorSetLayoutBinding bi[4] = { { 0, CIS, 1, C, NULL }, { 1, CIS, 1, C, NULL }, { 2, CIS, 1, C, NULL }, { 3, ST, 1, C, NULL } };
    VkDescriptorSetLayoutCreateInfo lm = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bm }, li = lm; li.pBindings = bi;
    if (vkCreateDescriptorSetLayout(g_dev, &lm, NULL, &FG.dl_m) || vkCreateDescriptorSetLayout(g_dev, &li, NULL, &FG.dl_i)) return 0;
    VkPushConstantRange pm = { C, 0, 32 }, pi = { C, 0, 24 };
    VkPipelineLayoutCreateInfo plm = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &FG.dl_m, .pushConstantRangeCount = 1, .pPushConstantRanges = &pm },
                               pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &FG.dl_i, .pushConstantRangeCount = 1, .pPushConstantRanges = &pi };
    if (vkCreatePipelineLayout(g_dev, &plm, NULL, &FG.pl_m) || vkCreatePipelineLayout(g_dev, &pli, NULL, &FG.pl_i)) return 0;
    for (int k = 0; k < 2; k++) {
        VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = k ? sizeof fg_interp_spv : sizeof fg_motion_spv, .pCode = k ? fg_interp_spv : fg_motion_spv };
        VkShaderModule m; if (vkCreateShaderModule(g_dev, &mi, NULL, &m)) return 0;
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = C, .module = m, .pName = "main" }, .layout = k ? FG.pl_i : FG.pl_m };
        VkResult r = vkCreateComputePipelines(g_dev, g_pcache, 1, &ci, NULL, k ? &FG.pi : &FG.pm); vkDestroyShaderModule(g_dev, m, NULL);
        if (r) { VLOG("[RED] frame generation: its %s pipeline failed (%d)", k ? "interpolation" : "motion", r); return 0; }
    }
    return 1;
}
static void fg_write(VkDescriptorSet ds, const VkImageView v[4], const int storage) {   // four bindings: samplers, the one at storage a storage image
    VkDescriptorImageInfo im[4]; VkWriteDescriptorSet w[4];
    for (int b = 0; b < 4; b++) {
        im[b] = (VkDescriptorImageInfo){ b == storage ? VK_NULL_HANDLE : FG.samp, v[b], VK_IMAGE_LAYOUT_GENERAL };
        w[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)b, .descriptorCount = 1,
            .descriptorType = b == storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &im[b] };
    }
    vkUpdateDescriptorSets(g_dev, 4, w, 0, NULL);
}
static int fg_resize(uint32_t w, uint32_t h) {   // the images for a w x h picture (the window lock is held; the GPU is waited for before the old ones go)
    if (FG.w == w && FG.h == h && FG.iv) return 1;
    if (FG.w) { pthread_mutex_lock(&g_qmu); vkQueueWaitIdle(g_q); pthread_mutex_unlock(&g_qmu); fg_free(); }
    if (!fg_pipelines()) return 0;
    FG.qw = (w + 3) / 4; FG.qh = (h + 3) / 4; FG.rw = (FG.qw + 3) / 4; FG.rh = (FG.qh + 3) / 4;
    FG.mw = (FG.qw + 3) / 4; FG.mh = (FG.qh + 3) / 4; FG.cw_ = (FG.rw + 3) / 4; FG.ch_ = (FG.rh + 3) / 4;
    const VkFormat F = VK_FORMAT_R16G16B16A16_SFLOAT, U8 = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags S = VK_IMAGE_USAGE_SAMPLED_BIT, T = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ST = VK_IMAGE_USAGE_STORAGE_BIT;
    int ok = fg_image(FG_H0, F, w, h, S | T) && fg_image(FG_H1, F, w, h, S | T)
          && fg_image(FG_Q0, U8, FG.qw, FG.qh, S | T) && fg_image(FG_Q1, U8, FG.qw, FG.qh, S | T)
          && fg_image(FG_R0, U8, FG.rw, FG.rh, S | T) && fg_image(FG_R1, U8, FG.rw, FG.rh, S | T)
          && fg_image(FG_MC, F, FG.cw_, FG.ch_, S | ST) && fg_image(FG_M, F, FG.mw, FG.mh, S | ST) && fg_image(FG_I, F, w, h, S | T | ST);
    if (!ok) { VLOG("[RED] frame generation: its images could not be made (%ux%u)", w, h); fg_free(); return 0; }
    // all in GENERAL from here on, as the engine's own images; the motion starts at zero
    VkCommandBuffer cb = once_begin(); VkImageMemoryBarrier ib[FG_N];
    for (int i = 0; i < FG_N; i++) ib[i] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = FG.img[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, FG_N, ib);
    once_end(cb);
    // the descriptor sets, per order of the two real frames (cur 0 / 1): coarse motion (1/16 copies, predicted by the last fine motion), fine motion
    //   (quarter copies, predicted by the coarse), middle frame
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 18 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 } };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 6, .poolSizeCount = 2, .pPoolSizes = ps };
    if (vkCreateDescriptorPool(g_dev, &pci, NULL, &FG.dp)) { fg_free(); return 0; }
    for (int c = 0; c < 2; c++) {
        VkDescriptorSetLayout ls[3] = { FG.dl_m, FG.dl_m, FG.dl_i }; VkDescriptorSet ds[3];
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = FG.dp, .descriptorSetCount = 3, .pSetLayouts = ls };
        if (vkAllocateDescriptorSets(g_dev, &ai, ds)) { fg_free(); return 0; }
        FG.ds_c[c] = ds[0]; FG.ds_m[c] = ds[1]; FG.ds_i[c] = ds[2];
        int p = 1 - c;
        VkImageView vc[4] = { FG.view[FG_R0 + p], FG.view[FG_R0 + c], FG.view[FG_MC], FG.view[FG_M] };
        VkImageView vm[4] = { FG.view[FG_Q0 + p], FG.view[FG_Q0 + c], FG.view[FG_M], FG.view[FG_MC] };
        VkImageView vi[4] = { FG.view[FG_H0 + p], FG.view[FG_H0 + c], FG.view[FG_M], FG.view[FG_I] };
        fg_write(FG.ds_c[c], vc, 2); fg_write(FG.ds_m[c], vm, 2); fg_write(FG.ds_i[c], vi, 3);
    }
    // the middle frame as a vtex, so the upscaler (scale_draw) draws it like the game's own picture
    vtex *v = calloc(1, sizeof *v); if (!v) { fg_free(); return 0; }
    v->refs = 1; v->img = FG.img[FG_I]; v->fmt = F; v->w = w; v->h = h; v->d = 1; v->levels = 1; v->layers = 1; v->samples = 1; v->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    v->usage = S | T | ST; pthread_mutex_init(&v->vmu, NULL); FG.iv = v;
    FG.w = w; FG.h = h; FG.cur = 0; FG.have_prev = FG.have_motion = 0;
    VLOG("frame generation: on, %ux%u (motion: %ux%u blocks of 64x64, then %ux%u of 16x16)", w, h, FG.cw_, FG.ch_, FG.mw, FG.mh);
    return 1;
}
// Whether this present takes part: the setting, pacing at an even number of refreshes per real frame, a picture the GPU can copy, the images for its size
static int fg_active(vtex *x, uint32_t cw, uint32_t ch) {
    int on = fg_setting() && !FG.failed && g_pace.on && g_pace.k >= 2 && !(g_pace.k & 1);
    if (on) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, x->fmt, &fp); if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(x->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) on = 0; }
    if (on && !fg_resize(cw, ch)) { FG.failed = 1; on = 0; }
    if (!on) FG.have_prev = FG.have_motion = 0;   // (back on later: it starts again from a fresh pair)
    FG.live = on;
    return on;
}
static void fg_motion_pass(VkCommandBuffer cb, VkDescriptorSet ds, uint32_t lw, uint32_t lh, uint32_t bw, uint32_t bh, int radius, float scale, int use_pred) {
    struct { int32_t lw, lh, bw, bh, radius; float scale, zbias, use_pred; } pc = { (int32_t)lw, (int32_t)lh, (int32_t)bw, (int32_t)bh, radius, scale, 0.003f, use_pred ? 1.0f : 0.0f };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, FG.pl_m, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, FG.pl_m, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (bw + 7) / 8, (bh + 7) / 8, 1);
    VkMemoryBarrier cc = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cc, 0, NULL, 0, NULL);
}
// The real frame copied (cropped, turned upright) into the history with its quarter and 1/16 copies, and - with the previous one there - the motion,
//   coarse then fine, and the middle frame. Returns whether the middle frame is ready (FG.iv)
static int fg_record(VkCommandBuffer cb, vtex *x, int32_t cx, int32_t cy, int32_t cw, int32_t ch, int flip) {
    int c = FG.cur, made = 0;
    VkMemoryBarrier tt = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT };
    VkImageBlit b1 = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{cx, flip ? cy + ch : cy, 0}, {cx + cw, flip ? cy : cy + ch, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {cw, ch, 1}} };
    vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, FG.img[FG_H0 + c], VK_IMAGE_LAYOUT_GENERAL, 1, &b1, VK_FILTER_NEAREST);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
    VkImageBlit b2 = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {cw, ch, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)FG.qw, (int32_t)FG.qh, 1}} };
    vkCmdBlitImage(cb, FG.img[FG_H0 + c], VK_IMAGE_LAYOUT_GENERAL, FG.img[FG_Q0 + c], VK_IMAGE_LAYOUT_GENERAL, 1, &b2, VK_FILTER_LINEAR);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
    VkImageBlit b3 = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)FG.qw, (int32_t)FG.qh, 1}}, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, {{0, 0, 0}, {(int32_t)FG.rw, (int32_t)FG.rh, 1}} };
    vkCmdBlitImage(cb, FG.img[FG_Q0 + c], VK_IMAGE_LAYOUT_GENERAL, FG.img[FG_R0 + c], VK_IMAGE_LAYOUT_GENERAL, 1, &b3, VK_FILTER_LINEAR);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
    if (FG.have_prev) {
        static int radius = -1; static float thresh; if (radius < 0) { const char *e = getenv("DM_FRAMEGEN_RADIUS"), *t = getenv("DM_FRAMEGEN_THRESH");
            radius = e ? atoi(e) : 6; if (radius < 1 || radius > 16) radius = 6; thresh = t ? (float)atof(t) : 0.06f; if (thresh <= 0) thresh = 0.06f; }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, FG.pm);
        fg_motion_pass(cb, FG.ds_c[c], FG.rw, FG.rh, FG.cw_, FG.ch_, radius, (float)cw / (float)FG.rw, FG.have_motion);   // coarse: +-radius 1/16 pixels
        fg_motion_pass(cb, FG.ds_m[c], FG.qw, FG.qh, FG.mw, FG.mh, 2, (float)cw / (float)FG.qw, 1);                       // fine: +-2 quarter pixels around it
        struct { int32_t w, h, mw, mh; float thresh, block; } pi = { cw, ch, (int32_t)FG.mw, (int32_t)FG.mh, thresh, (float)cw / (float)FG.mw };
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, FG.pi);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, FG.pl_i, 0, 1, &FG.ds_i[c], 0, NULL);
        vkCmdPushConstants(cb, FG.pl_i, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pi, &pi);
        vkCmdDispatch(cb, ((uint32_t)cw + 7) / 8, ((uint32_t)ch + 7) / 8, 1);
        VkMemoryBarrier cd = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &cd, 0, NULL, 0, NULL);
        made = 1; FG.made++; FG.have_motion = 1;
    }
    FG.cur = 1 - c; FG.have_prev = 1; FG.frames++;
    return made;
}
