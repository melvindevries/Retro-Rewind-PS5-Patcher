// Porpoise Retro Rewind - DualSense input. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace pad
{
enum Button : unsigned { up = 1, down = 2, left = 4, right = 8, cross = 16, circle = 32, square = 64,
                         triangle = 128, l1 = 256, r1 = 512, options = 1024 };
bool init();
// Buttons newly pressed since the last call; up/down/l1/r1 repeat while held. Call once per frame.
unsigned poll();
}
