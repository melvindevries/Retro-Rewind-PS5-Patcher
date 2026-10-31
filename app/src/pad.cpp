// Porpoise Retro Rewind - DualSense input. Raw layout and call sequence follow the boilerplate pad module.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad.h"
#include <cstdint>

extern "C"
{
    int sceUserServiceInitialize(const void *);
    int sceUserServiceGetInitialUser(std::int32_t *);
    int scePadInit();
    int scePadOpen(std::int32_t, std::int32_t, std::int32_t, const void *);
    int scePadGetHandle(std::int32_t, std::int32_t, std::int32_t);
    int scePadRead(std::int32_t, void *, std::int32_t);
}

namespace pad
{
namespace
{
constexpr int sample_size = 120, sample_count = 32;
constexpr std::uint32_t intercepted = 0x80000000u;
int g_handle = -1;
alignas(16) unsigned char g_samples[sample_count][sample_size];
unsigned g_held = 0;
int g_age = 0;

unsigned translate(std::uint32_t raw, unsigned lx, unsigned ly)
{
    unsigned b = 0;
    if (raw & 0x0010) b |= up;
    if (raw & 0x0040) b |= down;
    if (raw & 0x0080) b |= left;
    if (raw & 0x0020) b |= right;
    if (raw & 0x4000) b |= cross;
    if (raw & 0x2000) b |= circle;
    if (raw & 0x8000) b |= square;
    if (raw & 0x1000) b |= triangle;
    if (raw & 0x0400) b |= l1;
    if (raw & 0x0800) b |= r1;
    if (raw & 0x0008) b |= options;
    if (ly < 68) b |= up;
    if (ly > 188) b |= down;
    if (lx < 68) b |= left;
    if (lx > 188) b |= right;
    return b;
}
} // namespace

bool init()
{
    (void)sceUserServiceInitialize(nullptr);
    std::int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0 || user < 0 || scePadInit() < 0) return false;
    g_handle = scePadOpen(user, 0, 0, nullptr);
    if (g_handle < 0) g_handle = scePadGetHandle(user, 0, 0);
    return g_handle >= 0;
}

unsigned poll()
{
    if (g_handle < 0) return 0;
    const int n = scePadRead(g_handle, g_samples, sample_count);
    if (n <= 0) return 0;
    const unsigned char *s = g_samples[n - 1]; // newest sample
    const std::uint32_t raw = s[0] | s[1] << 8 | s[2] << 16 | static_cast<std::uint32_t>(s[3]) << 24;
    unsigned now = (raw & intercepted) ? 0 : translate(raw, s[4], s[5]);
    unsigned pressed = now & ~g_held;
    constexpr unsigned repeating = up | down | l1 | r1;
    if (now & repeating)
    {
        if (pressed & repeating) g_age = 0;
        else if (++g_age > 24 && (g_age % 5) == 0) pressed |= now & repeating;
    }
    else g_age = 0;
    g_held = now;
    return pressed;
}
} // namespace pad
