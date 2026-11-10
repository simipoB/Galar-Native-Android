#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint32_t n = 0;
    VkLayerProperties layers[64];
    VkResult r = vkEnumerateInstanceLayerProperties(&n, NULL);
    if (r != VK_SUCCESS || n > 64) {
        fprintf(stderr, "layer enumeration failed: %d (count %u)\n", r, n);
        return 1;
    }
    r = vkEnumerateInstanceLayerProperties(&n, layers);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "layer listing failed: %d\n", r);
        return 1;
    }
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation")) found = 1;
    if (!found) {
        fprintf(stderr, "VK_LAYER_KHRONOS_validation is unavailable\n");
        return 2;
    }
    const char *name = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Galar Native validation smoke", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledLayerCount = 1, .ppEnabledLayerNames = &name };
    VkInstance instance = VK_NULL_HANDLE;
    r = vkCreateInstance(&ci, NULL, &instance);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "validation instance creation failed: %d\n", r);
        return 1;
    }
    vkDestroyInstance(instance, NULL);
    return 0;
}
