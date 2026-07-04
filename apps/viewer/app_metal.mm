#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <kumo/core/log.h>

// Throwaway pre-RHI code: proves the window + Metal + present loop before the
// RHI lands in M2, which replaces this file entirely.

namespace {

void updateDrawableSize(GLFWwindow* window, CAMetalLayer* layer) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    CGSize size = CGSizeMake(width, height);
    if (!CGSizeEqualToSize(layer.drawableSize, size)) {
        layer.drawableSize = size;
    }
}

} // namespace

int runMetalApp(int maxFrames) {
    if (!glfwInit()) {
        kumo::logError("glfwInit failed");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "kumo", nullptr, nullptr);
    if (!window) {
        kumo::logError("window creation failed");
        glfwTerminate();
        return 1;
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        kumo::logError("no Metal device available");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    kumo::logInfo("Metal device: {}", [device.name UTF8String]);

    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

    NSWindow* nsWindow = glfwGetCocoaWindow(window);
    nsWindow.contentView.layer = layer;
    nsWindow.contentView.wantsLayer = YES;
    updateDrawableSize(window, layer);

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        @autoreleasepool {
            updateDrawableSize(window, layer);

            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (drawable) {
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = drawable.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.45, 0.5, 1.0);

                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder =
                    [commands renderCommandEncoderWithDescriptor:pass];
                [encoder endEncoding];
                [commands presentDrawable:drawable];
                [commands commit];
            }
        }

        ++frame;
        if (maxFrames >= 0 && frame >= maxFrames) {
            break;
        }
    }

    kumo::logInfo("rendered {} frames", frame);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
