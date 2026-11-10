#include "../nvn/swapchain_guard.h"
#include <stdio.h>
#include <string.h>

struct mock_cleanup {
    int views, swapchains, surfaces;
    uint32_t view_mask;
};

static void destroy_view(void *p, uint32_t index) {
    struct mock_cleanup *m = p;
    m->views++;
    m->view_mask |= 1u << index;
}
static void destroy_swapchain(void *p) { ((struct mock_cleanup *)p)->swapchains++; }
static void destroy_surface(void *p) { ((struct mock_cleanup *)p)->surfaces++; }

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "ok" : "FAIL", s); } while (0)

static struct swapchain_guard_cleanup cleanup_for(struct mock_cleanup *m) {
    return (struct swapchain_guard_cleanup) { destroy_view, destroy_swapchain, destroy_surface, m };
}

int main(void) {
    struct mock_cleanup m;
    enum swapchain_guard_failure f;

    memset(&m, 0, sizeof m);
    f = swapchain_guard_validate(0, 0, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, 0, VK_SUCCESS, cleanup_for(&m));
    CHECK(f == SWAPCHAIN_GUARD_NO_FORMATS && m.surfaces == 1 && !m.swapchains, "zero surface formats fails closed and destroys surface");

    memset(&m, 0, sizeof m);
    f = swapchain_guard_validate(1, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 0, 0, VK_SUCCESS, cleanup_for(&m));
    CHECK(f == SWAPCHAIN_GUARD_UNSUPPORTED_USAGE && m.surfaces == 1 && !m.swapchains, "unsupported usage flags fail closed");

    memset(&m, 0, sizeof m);
    f = swapchain_guard_validate(1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 0, 0, VK_SUCCESS, cleanup_for(&m));
    CHECK(f == SWAPCHAIN_GUARD_NO_IMAGES && m.swapchains == 1 && m.surfaces == 1, "zero swapchain images destroy swapchain and surface");

    memset(&m, 0, sizeof m);
    f = swapchain_guard_validate(1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 3, 2, VK_ERROR_INITIALIZATION_FAILED, cleanup_for(&m));
    CHECK(f == SWAPCHAIN_GUARD_IMAGE_VIEW && m.views == 2 && m.view_mask == 3 && m.swapchains == 1 && m.surfaces == 1,
          "image-view failure destroys all previously created views and parents");

    memset(&m, 0, sizeof m);
    f = swapchain_guard_validate(2, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 3, 3, VK_SUCCESS, cleanup_for(&m));
    CHECK(f == SWAPCHAIN_GUARD_OK && !m.views && !m.swapchains && !m.surfaces, "valid swapchain setup remains live");

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
