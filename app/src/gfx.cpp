// Porpoise Retro Rewind - CPU renderer and VideoOut presentation.
// VideoOut/direct-memory setup and the tiled-offset function follow the ps5-native-app-boilerplate
// demo renderer (BlackBearReloaded, GPL-3.0-or-later). Glyphs: DejaVu Sans Mono (see font_data.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx.h"
#include "font_data.h"
#include <cstddef>
#include <cstring>
#ifdef GFX_HOST
#include <cstdio>
#endif

extern "C"
{
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t *);
    int sceKernelMapDirectMemory(void **, std::size_t, int, int, std::int64_t, std::size_t);
    int sceKernelSendNotificationRequest(std::uint32_t, void *, std::size_t, int);
    int sceKernelUsleep(std::uint32_t);
    int sceSystemServiceHideSplashScreen();
    int sceVideoOutOpen(std::int32_t, std::int32_t, std::int32_t, const void *);
    int sceVideoOutSetFlipRate(std::int32_t, std::int32_t);
    int sceVideoOutSubmitFlip(std::int32_t, std::int32_t, std::uint32_t, std::int64_t);
    int sceVideoOutWaitVblank(std::int32_t);
}

namespace
{
struct VideoBuffer { void *data, *metadata, *r0, *r1; };
struct VideoAttribute { std::uint8_t reserved[80]; };
}
extern "C" void sceVideoOutSetBufferAttribute2(VideoAttribute *, std::uint64_t, std::uint32_t, std::uint32_t,
                                               std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t);
extern "C" int sceVideoOutRegisterBuffers2(std::int32_t, std::int32_t, std::int32_t, VideoBuffer *, std::int32_t,
                                           VideoAttribute *, std::int32_t, void *);

namespace gfx
{
namespace
{
constexpr std::size_t frame_bytes = 0x1000000, memory_bytes = frame_bytes * 2, memory_alignment = 0x200000;
std::uint32_t g_lin[W * H];              // linear working frame (cached memory)
std::uint8_t *g_buf[2];
int g_video = -1;
std::int64_t g_flip = 1;
unsigned g_back = 1;
std::uint32_t g_bg[W * H];               // cached static background
std::uint32_t g_xoff[W], g_yoff[H];      // the tiled offset is x-part XOR y-part
struct Notification { std::uint8_t reserved[45]; char message[3075]; } g_note;

void build_tables()
{
    for (unsigned x = 0; x < W; ++x)
        g_xoff[x] = ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^
                    ((x << 9) & 0xa000u);
    for (unsigned y = 0; y < H; ++y)
        g_yoff[y] = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u);
}
void flush(void *p, std::size_t n)
{
    auto *a = static_cast<std::uint8_t *>(p);
    for (const auto *e = a + n; a < e; a += 64) __asm__ volatile("clflush (%0)" : : "r"(a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}
void tile_copy(std::uint8_t *dst)
{
    constexpr unsigned blocks_per_row = (W + 127) >> 7;
    for (unsigned y = 0; y < H; ++y)
    {
        const std::uint32_t *src = g_lin + y * W;
        const std::uint32_t yo = g_yoff[y];
        std::uint8_t *rowbase = dst + (static_cast<std::size_t>(y >> 7) * blocks_per_row << 16);
        for (unsigned x = 0; x < W; ++x)
            *reinterpret_cast<std::uint32_t *>(rowbase + (static_cast<std::size_t>(x >> 7) << 16) + (yo ^ g_xoff[x])) = src[x];
    }
}
} // namespace

void notify(const char *m)
{
    std::strncpy(g_note.message, m, sizeof g_note.message - 1);
    (void)sceKernelSendNotificationRequest(0, &g_note, sizeof g_note, 0);
}

void wait_vblank() { if (g_video >= 0) (void)sceVideoOutWaitVblank(g_video); }

bool init(const char **why)
{
    build_tables();
    (void)sceSystemServiceHideSplashScreen();
    g_video = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (g_video < 0) { *why = "sceVideoOutOpen failed"; return false; }
    const std::size_t pool = sceKernelGetDirectMemorySize();
    if (pool < memory_bytes) { *why = "not enough direct memory"; return false; }
    std::int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(pool), memory_bytes, memory_alignment, 3, &phys) < 0)
    { *why = "direct memory allocation failed"; return false; }
    void *mapped = nullptr;
    if (sceKernelMapDirectMemory(&mapped, memory_bytes, 0x33, 0, phys, memory_alignment) < 0)
    { *why = "direct memory mapping failed"; return false; }
    g_buf[0] = static_cast<std::uint8_t *>(mapped);
    g_buf[1] = g_buf[0] + frame_bytes;
    std::memset(mapped, 0, memory_bytes);
    flush(mapped, memory_bytes);
    VideoBuffer buffers[2] = {{g_buf[0], nullptr, nullptr, nullptr}, {g_buf[1], nullptr, nullptr, nullptr}};
    VideoAttribute attr{};
    (void)sceVideoOutSetFlipRate(g_video, 0);
    sceVideoOutSetBufferAttribute2(&attr, UINT64_C(0x8000000022000000), 0, W, H, 0, 0, 0);
    if (sceVideoOutRegisterBuffers2(g_video, 0, 0, buffers, 2, &attr, 0, nullptr) < 0)
    { *why = "buffer registration failed"; return false; }
    if (sceVideoOutSubmitFlip(g_video, 0, 1, 1) < 0) { *why = "initial flip failed"; return false; }
    (void)sceVideoOutWaitVblank(g_video);
    return true;
}

void present()
{
    tile_copy(g_buf[g_back]);
    flush(g_buf[g_back], frame_bytes);
    ++g_flip;
    if (sceVideoOutSubmitFlip(g_video, static_cast<std::int32_t>(g_back), 1, g_flip) < 0)
        (void)sceKernelUsleep(4000);
    g_back ^= 1u;
}

void fill(int x, int y, int w, int h, std::uint32_t c)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > static_cast<int>(W)) x1 = W;
    if (y1 > static_cast<int>(H)) y1 = H;
    for (int j = y; j < y1; ++j)
        for (int i = x; i < x1; ++i) g_lin[j * W + i] = c;
}

void frame(int x, int y, int w, int h, int t, std::uint32_t c)
{
    fill(x, y, w, t, c); fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
}

int text_w(const char *s, int scale) { return static_cast<int>(std::strlen(s)) * FONT_W * scale; }

void text(int x, int y, const char *s, int scale, std::uint32_t c, int max_chars)
{
    const unsigned cr = c & 0xff, cg = (c >> 8) & 0xff, cb = (c >> 16) & 0xff;
    for (int n = 0; *s && n < max_chars; ++s, ++n, x += FONT_W * scale)
    {
        unsigned ch = static_cast<unsigned char>(*s);
        if (ch < 32 || ch > 126) ch = '?';
        if (x + FONT_W * scale < 0 || x >= static_cast<int>(W)) continue;
        const unsigned char *g = font_alpha[ch - 32];
        for (int gy = 0; gy < FONT_H; ++gy)
            for (int gx = 0; gx < FONT_W; ++gx)
            {
                const unsigned a = g[gy * FONT_W + gx];
                if (!a) continue;
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx)
                    {
                        const int px = x + gx * scale + sx, py = y + gy * scale + sy;
                        if (px < 0 || py < 0 || px >= static_cast<int>(W) || py >= static_cast<int>(H)) continue;
                        std::uint32_t &d = g_lin[py * W + px];
                        const unsigned dr = d & 0xff, dg = (d >> 8) & 0xff, db = (d >> 16) & 0xff;
                        d = rgb((cr * a + dr * (255 - a)) / 255, (cg * a + dg * (255 - a)) / 255,
                                (cb * a + db * (255 - a)) / 255);
                    }
            }
    }
}

namespace
{
inline std::uint32_t mix(std::uint32_t d, std::uint32_t c, int a)
{
    const int ia = 255 - a;
    const unsigned r = ((c & 0xff) * a + (d & 0xff) * ia) / 255, g = (((c >> 8) & 0xff) * a + ((d >> 8) & 0xff) * ia) / 255,
                   b = (((c >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia) / 255;
    return rgb(r, g, b);
}
} // namespace

void fill_a(int x, int y, int w, int h, std::uint32_t c, int alpha)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > static_cast<int>(W)) x1 = W;
    if (y1 > static_cast<int>(H)) y1 = H;
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    for (int j = y; j < y1; ++j)
        for (int i = x; i < x1; ++i) g_lin[j * W + i] = mix(g_lin[j * W + i], c, alpha);
}

void rrect(int x, int y, int w, int h, int r, std::uint32_t c, int alpha)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; ++j)
    {
        const int py = y + j;
        if (py < 0 || py >= static_cast<int>(H)) continue;
        for (int i = 0; i < w; ++i)
        {
            const int px = x + i;
            if (px < 0 || px >= static_cast<int>(W)) continue;
            int cov = 255; // 1 px soft edge on the corners
            const int cx = i < r ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
            const int cy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
            if (cx > 0 && cy > 0)
            {
                const int d2 = cx * cx + cy * cy, R2 = r * r;
                if (d2 > (r + 1) * (r + 1)) continue;
                if (d2 > R2) cov = 255 - 255 * (d2 - R2) / ((r + 1) * (r + 1) - R2);
            }
            const int a = alpha * cov / 255;
            if (a > 0) g_lin[py * W + px] = mix(g_lin[py * W + px], c, a);
        }
    }
}

namespace
{
inline bool in_rr(int i, int j, int w, int h, int r)
{
    if (i < 0 || j < 0 || i >= w || j >= h) return false;
    const int cx = i < r ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
    const int cy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
    return !(cx > 0 && cy > 0) || cx * cx + cy * cy <= r * r;
}
} // namespace

void rrect_frame(int x, int y, int w, int h, int r, int t, std::uint32_t c)
{
    for (int j = 0; j < h; ++j)
    {
        const int py = y + j;
        if (py < 0 || py >= static_cast<int>(H)) continue;
        for (int i = 0; i < w; ++i)
        {
            const int px = x + i;
            if (px < 0 || px >= static_cast<int>(W)) continue;
            if (in_rr(i, j, w, h, r) && !in_rr(i - t, j - t, w - 2 * t, h - 2 * t, r > t ? r - t : 0)) g_lin[py * W + px] = c;
        }
    }
}

void gradient_v(int x, int y, int w, int h, std::uint32_t top, std::uint32_t bottom)
{
    for (int j = 0; j < h; ++j) fill(x, y + j, w, 1, mix(top, bottom, h > 1 ? j * 255 / (h - 1) : 0));
}

void blit(int x, int y, int w, int h, const std::uint8_t *p)
{
    for (int j = 0; j < h; ++j)
    {
        const int py = y + j;
        if (py < 0 || py >= static_cast<int>(H)) continue;
        for (int i = 0; i < w; ++i, p += 4)
        {
            const int px = x + i;
            if (px < 0 || px >= static_cast<int>(W) || !p[3]) continue;
            g_lin[py * W + px] = mix(g_lin[py * W + px], rgb(p[0], p[1], p[2]), p[3]);
        }
    }
}

std::uint32_t hsv(int hue, int sat, int val)
{
    hue %= 360;
    const int region = hue / 60, rem = (hue % 60) * 255 / 60;
    const int p = val * (255 - sat) / 255, q = val * (255 - sat * rem / 255) / 255, t = val * (255 - sat * (255 - rem) / 255) / 255;
    switch (region)
    {
    case 0: return rgb(val, t, p);
    case 1: return rgb(q, val, p);
    case 2: return rgb(p, val, t);
    case 3: return rgb(p, q, val);
    case 4: return rgb(t, p, val);
    default: return rgb(val, p, q);
    }
}

void text_shadow(int x, int y, const char *s, int scale, std::uint32_t c, int max_chars)
{
    text(x + scale, y + scale, s, scale, rgb(0, 0, 0), max_chars);
    text(x, y, s, scale, c, max_chars);
}

void save_background() { std::memcpy(g_bg, g_lin, sizeof g_lin); }
void restore_background() { std::memcpy(g_lin, g_bg, sizeof g_lin); }
#ifdef GFX_HOST
void dump(const char *path)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (unsigned i = 0; i < W * H; ++i)
    {
        const std::uint32_t v = g_lin[i];
        const unsigned char px[3] = {(unsigned char)(v & 255), (unsigned char)((v >> 8) & 255), (unsigned char)((v >> 16) & 255)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}
#endif
} // namespace gfx
