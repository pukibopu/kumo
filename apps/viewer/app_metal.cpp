#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <kumo/core/log.h>

// Throwaway pre-RHI code: proves the window + Metal + present loop before the
// RHI lands in M2, which replaces this file entirely.

void* attachMetalLayer(GLFWwindow* window);

namespace {

void updateDrawableSize(GLFWwindow* window, CA::MetalLayer* layer) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    CGSize current = layer->drawableSize();
    if (current.width != width || current.height != height) {
        layer->setDrawableSize(CGSizeMake(width, height));
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

    NS::SharedPtr<MTL::Device> device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        kumo::logError("no Metal device available");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    NS::SharedPtr<MTL::CommandQueue> queue = NS::TransferPtr(device->newCommandQueue());
    kumo::logInfo("Metal device: {}", device->name()->utf8String());

    // Owned by the content view; stays alive as long as the window does.
    auto* layer = static_cast<CA::MetalLayer*>(attachMetalLayer(window));
    layer->setDevice(device.get());
    layer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    updateDrawableSize(window, layer);

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

        updateDrawableSize(window, layer);

        CA::MetalDrawable* drawable = layer->nextDrawable();
        if (drawable) {
            MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            MTL::RenderPassColorAttachmentDescriptor* color = pass->colorAttachments()->object(0);
            color->setTexture(drawable->texture());
            color->setLoadAction(MTL::LoadActionClear);
            color->setStoreAction(MTL::StoreActionStore);
            color->setClearColor(MTL::ClearColor::Make(0.0, 0.45, 0.5, 1.0));

            MTL::CommandBuffer* commands = queue->commandBuffer();
            MTL::RenderCommandEncoder* encoder = commands->renderCommandEncoder(pass);
            encoder->endEncoding();
            commands->presentDrawable(drawable);
            commands->commit();
        }

        pool->release();

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
