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
void notify(const char *message);
} // namespace gfx
