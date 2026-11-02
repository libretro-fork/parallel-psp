/* unittest/TestVulkanVideoSurface.cpp on the core's own build: a decoded video frame stays a
 * Vulkan image until the guest touches its memory, through the fault path wherever deferral
 * is supported (Windows, Linux, macOS with 4 KiB pages). Needs a Vulkan device; lavapipe will do. */
#include <cstdio>

bool TestVulkanVideoSurface();

int main() {
	const bool ok = TestVulkanVideoSurface();
	printf("vkvideotest: %s\n", ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
