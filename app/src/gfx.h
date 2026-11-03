// Porpoise Retro Rewind - CPU renderer and VideoOut presentation (adapted from the boilerplate demo).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace gfx
{
constexpr unsigned W = 1920, H = 1080;
constexpr std::uint32_t rgb(unsigned r, unsigned g, unsigned b)
{
    return 0xff000000u | (b << 16) | (g << 8) | r; // memory order R,G,B,A
}
bool init(const char **why);   // opens VideoOut, maps two tiled buffers
void present();                // tiles the linear frame into the back buffer and flips
void fill(int x, int y, int w, int h, std::uint32_t c);
void frame(int x, int y, int w, int h, int t, std::uint32_t c);
void text(int x, int y, const char *s, int scale, std::uint32_t c, int max_chars = 1000);
int text_w(const char *s, int scale);
void wait_vblank();
// --- drawing helpers for the styled UI (all clip to the screen) ---
void fill_a(int x, int y, int w, int h, std::uint32_t c, int alpha);                 // alpha 0..255
void rrect(int x, int y, int w, int h, int r, std::uint32_t c, int alpha = 255);     // rounded rectangle
void rrect_frame(int x, int y, int w, int h, int r, int t, std::uint32_t c);
void gradient_v(int x, int y, int w, int h, std::uint32_t top, std::uint32_t bottom);
void blit(int x, int y, int w, int h, const std::uint8_t *rgba);                     // straight alpha RGBA
std::uint32_t hsv(int hue, int sat, int val);                                        // hue 0..359, sat/val 0..255
void text_shadow(int x, int y, const char *s, int scale, std::uint32_t c, int max_chars = 1000);
void save_background();      // remember the current frame as the static background
#ifdef GFX_HOST
void dump(const char *ppm_path);   // host preview only
#endif
void restore_background();   // copy the remembered background back (cheap full-screen clear)
void notify(const char *message);
} // namespace gfx
