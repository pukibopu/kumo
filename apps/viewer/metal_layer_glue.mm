#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <kumo/gpu/metal_interop.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// metal-cpp has no AppKit coverage, so attaching the layer to the NSView is
// the one place that needs Objective-C. The pointer bridges to CA::MetalLayer*.
CA::MetalLayer* attachMetalLayer(GLFWwindow* window) {
    NSWindow* nsWindow = glfwGetCocoaWindow(window);
    CAMetalLayer* layer = [CAMetalLayer layer];
    nsWindow.contentView.layer = layer;
    nsWindow.contentView.wantsLayer = YES;
    return reinterpret_cast<CA::MetalLayer*>((__bridge void*)layer);
}
