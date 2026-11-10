#pragma once

#include <stdint.h>
#include <vulkan/vulkan_core.h>

enum swapchain_guard_failure {
    SWAPCHAIN_GUARD_OK = 0,
    SWAPCHAIN_GUARD_NO_FORMATS,
    SWAPCHAIN_GUARD_UNSUPPORTED_USAGE,
    SWAPCHAIN_GUARD_NO_IMAGES,
    SWAPCHAIN_GUARD_IMAGE_VIEW
};

struct swapchain_guard_cleanup {
    void (*destroy_view)(void *ctx, uint32_t index);
    void (*destroy_swapchain)(void *ctx);
    void (*destroy_surface)(void *ctx);
    void *ctx;
};

static enum swapchain_guard_failure swapchain_guard_validate(
        uint32_t format_count, VkImageUsageFlags supported_usage, VkImageUsageFlags required_usage,
        uint32_t image_count, uint32_t created_view_count, VkResult image_view_result,
        struct swapchain_guard_cleanup cleanup) {
    if (!format_count) {
        if (cleanup.destroy_surface) cleanup.destroy_surface(cleanup.ctx);
        return SWAPCHAIN_GUARD_NO_FORMATS;
    }
    if ((supported_usage & required_usage) != required_usage) {
        if (cleanup.destroy_surface) cleanup.destroy_surface(cleanup.ctx);
        return SWAPCHAIN_GUARD_UNSUPPORTED_USAGE;
    }
    if (!image_count) {
        if (cleanup.destroy_swapchain) cleanup.destroy_swapchain(cleanup.ctx);
        if (cleanup.destroy_surface) cleanup.destroy_surface(cleanup.ctx);
        return SWAPCHAIN_GUARD_NO_IMAGES;
    }
    if (image_view_result) {
        if (cleanup.destroy_view)
            for (uint32_t i = 0; i < created_view_count; i++) cleanup.destroy_view(cleanup.ctx, i);
        if (cleanup.destroy_swapchain) cleanup.destroy_swapchain(cleanup.ctx);
        if (cleanup.destroy_surface) cleanup.destroy_surface(cleanup.ctx);
        return SWAPCHAIN_GUARD_IMAGE_VIEW;
    }
    return SWAPCHAIN_GUARD_OK;
}
