// SPDX-License-Identifier: GPL-3.0-or-later
// Uses the SwanStationPS5 Vulkan presenter with a Vulkan upload path for Ymir's software VDP.

#include "gpu.h"

// Include the presenter so the upload can use the same graphics queue, fences, and swapchain lifecycle.
#include <vk_present.c>

#include <stdarg.h>
#include <unistd.h>

extern int sceKernelDebugOutText(int channel, const char *text);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}

void SwanStationPS5_log(const char *format, ...) {
    char line[512];
    const int prefix = snprintf(line, sizeof(line), "Ymir PS5: ");
    va_list args;
    va_start(args, format);
    vsnprintf(line + prefix, sizeof(line) - prefix - 2, format, args);
    va_end(args);
    strcat(line, "\n");
    fputs(line, stderr);
    sceKernelDebugOutText(0, line);
}

static struct {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkBuffer staging[FRAMES];
    VkDeviceMemory stagingMemory[FRAMES];
    void *mapped[FRAMES];
    VkCommandBuffer commands[FRAMES];
    unsigned width, height;
    bool ready;
} gameUpload;

static void release_upload(void) {
    if (!V.device) {
        return;
    }
    vkDeviceWaitIdle(V.device);
    vkp_set_game_image(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);
    if (gameUpload.commands[0]) {
        vkFreeCommandBuffers(V.device, V.commands, FRAMES, gameUpload.commands);
    }
    for (unsigned i = 0; i < FRAMES; ++i) {
        if (gameUpload.staging[i]) {
            vkDestroyBuffer(V.device, gameUpload.staging[i], NULL);
        }
        if (gameUpload.stagingMemory[i]) {
            vkFreeMemory(V.device, gameUpload.stagingMemory[i], NULL);
        }
    }
    if (gameUpload.view) {
        vkDestroyImageView(V.device, gameUpload.view, NULL);
    }
    if (gameUpload.image) {
        vkDestroyImage(V.device, gameUpload.image, NULL);
    }
    if (gameUpload.memory) {
        vkFreeMemory(V.device, gameUpload.memory, NULL);
    }
    memset(&gameUpload, 0, sizeof(gameUpload));
}

static bool create_upload(unsigned width, unsigned height) {
    VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = (VkExtent3D){width, height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(V.device, &image, NULL, &gameUpload.image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(V.device, gameUpload.image, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gameUpload.memory) ||
        vkBindImageMemory(V.device, gameUpload.image, gameUpload.memory, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = gameUpload.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(V.device, &view, NULL, &gameUpload.view) != VK_SUCCESS) {
        return false;
    }
    for (unsigned i = 0; i < FRAMES; ++i) {
        VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = (VkDeviceSize)width * height * sizeof(uint32_t);
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(V.device, &buffer, NULL, &gameUpload.staging[i]) != VK_SUCCESS) {
            return false;
        }
        vkGetBufferMemoryRequirements(V.device, gameUpload.staging[i], &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                      &gameUpload.stagingMemory[i]) ||
            vkBindBufferMemory(V.device, gameUpload.staging[i], gameUpload.stagingMemory[i], 0) != VK_SUCCESS ||
            vkMapMemory(V.device, gameUpload.stagingMemory[i], 0, VK_WHOLE_SIZE, 0, &gameUpload.mapped[i]) !=
                VK_SUCCESS) {
            return false;
        }
    }
    VkCommandBufferAllocateInfo commands = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commands.commandPool = V.commands;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = FRAMES;
    if (vkAllocateCommandBuffers(V.device, &commands, gameUpload.commands) != VK_SUCCESS) {
        return false;
    }
    gameUpload.width = width;
    gameUpload.height = height;
    return true;
}

static bool stage_game(const uint32_t *pixels) {
    const int frame = V.frame;
    memcpy(gameUpload.mapped[frame], pixels, (size_t)gameUpload.width * gameUpload.height * sizeof(uint32_t));
    VkCommandBuffer cmd = gameUpload.commands[frame];
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        return false;
    }
    barrier(cmd, gameUpload.image,
            gameUpload.ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = (VkExtent3D){gameUpload.width, gameUpload.height, 1};
    vkCmdCopyBufferToImage(cmd, gameUpload.staging[frame], gameUpload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    barrier(cmd, gameUpload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    // The presenter's fence for this slot follows the upload on the same graphics queue.
    if (vkQueueSubmit(V.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }
    gameUpload.ready = true;
    return true;
}

bool ymir_gpu_open(unsigned width, unsigned height, unsigned canvasWidth, unsigned canvasHeight, bool prefer120Hz,
                   char *error, size_t size) {
    vkp_want_size(width, height);
    vkp_want_high_refresh(prefer120Hz);
    if (!vkp_open(canvasWidth, canvasHeight, error, size)) {
        return false;
    }
    if (!vkp_ui_open(error, size)) {
        vkp_close();
        return false;
    }
    return true;
}

void ymir_gpu_close(void) {
    release_upload();
    vkp_close();
}

bool ymir_gpu_resize(unsigned width, unsigned height) {
    char error[256] = {0};
    release_upload();
    if (!vkp_set_mode(width, height, error, sizeof(error))) {
        SwanStationPS5_log("Could not resize Vulkan output: %s", error);
        return false;
    }
    return ymir_gpu_matches(width, height);
}

bool ymir_gpu_matches(unsigned width, unsigned height) {
    return V.extent.width == width && V.extent.height == height;
}

const char *ymir_gpu_description(void) {
    return vkp_describe();
}

double ymir_gpu_refresh(void) {
    return vkp_display_hz();
}

bool ymir_gpu_menu(const uint32_t *pixels) {
    if (!vkp_begin_frame()) {
        return false;
    }
    vkp_set_framegen(0, 60.0, 100.0, false);
    V.core_frames = V.new_frames = V.since_new = V.fg_already = 0;
    vkp_present(pixels, (size_t)V.cw * sizeof(uint32_t));
    return true;
}

bool ymir_gpu_game(const uint32_t *pixels, unsigned width, unsigned height, bool fresh, bool frameTick, float x,
                   float y, float extentWidth, float extentHeight, bool fsr, unsigned generated, double coreHz) {
    if (width && height && (width != gameUpload.width || height != gameUpload.height)) {
        release_upload();
        if (!create_upload(width, height)) {
            release_upload();
            return false;
        }
        fresh = true;
    }
    if (!vkp_begin_frame()) {
        return false;
    }
    if (fresh && !stage_game(pixels)) {
        return false;
    }
    // Ymir's software renderer can finish between two emulation ticks. Count
    // the emulation ticks and the completed, distinct pictures separately.
    V.core_frames += frameTick;
    if (fresh) {
        V.game_image = gameUpload.image;
        V.game_view = gameUpload.view;
        V.game_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ++V.new_frames;
    }

    vkp_set_framegen(generated, coreHz, 100.0, true);
    vkp_set_fsr(fsr);
    vkp_set_game_nearest(true);
    vkp_ui_begin(0xff000000);
    if (gameUpload.ready && width && height) {
        vkp_show_game(x, y, extentWidth, extentHeight, 0.0f, 0, width, height, height);
    }
    vkp_present_ui();
    return true;
}
