#include <kumo/core/log.h>

#include <GLFW/glfw3.h>

int main() {
    kumo::logInfo("kumo viewer {}", KUMO_VERSION_STRING);
    kumo::logInfo("GLFW {}", glfwGetVersionString());
    return 0;
}
