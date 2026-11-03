// Host-only stubs so app.cpp and gfx.cpp compile on a PC for the UI preview. SPDX-License-Identifier: GPL-3.0-or-later
#include "../../app/src/elevate.h"
#include "../../app/src/pad.h"
#include "../../app/src/http_sce.h"
#include <cstdint>
extern "C" {
std::size_t sceKernelGetDirectMemorySize() { return 0; }
int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t *) { return -1; }
int sceKernelMapDirectMemory(void **, std::size_t, int, int, std::int64_t, std::size_t) { return -1; }
int sceKernelSendNotificationRequest(std::uint32_t, void *, std::size_t, int) { return 0; }
int sceKernelUsleep(std::uint32_t) { return 0; }
int sceSystemServiceHideSplashScreen() { return 0; }
int sceVideoOutOpen(std::int32_t, std::int32_t, std::int32_t, const void *) { return -1; }
int sceVideoOutSetFlipRate(std::int32_t, std::int32_t) { return 0; }
int sceVideoOutSubmitFlip(std::int32_t, std::int32_t, std::uint32_t, std::int64_t) { return 0; }
int sceVideoOutWaitVblank(std::int32_t) { return 0; }
void sceVideoOutSetBufferAttribute2(void *, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t) {}
int sceVideoOutRegisterBuffers2(std::int32_t, std::int32_t, std::int32_t, void *, std::int32_t, void *, std::int32_t, void *) { return -1; }
int sce_http_start(void) { return -1; }
void sce_http_stop(void) {}
int sce_http_get(void *, rr_ctx *, const char *, const char *, uint8_t **, size_t *, size_t, uint64_t *, int *) { return -1; }
}
namespace elevate { Probe probe() { return {true, ""}; } bool request(char *, unsigned) { return false; } const char *errname(int) { return ""; } bool writable(const char *, char *, unsigned) { return true; } }
namespace pad { bool init() { return false; } unsigned poll() { return 0; } }
void ui_preview(const char *dir);
int main(int argc, char **argv) { ui_preview(argc > 1 ? argv[1] : "."); return 0; }
