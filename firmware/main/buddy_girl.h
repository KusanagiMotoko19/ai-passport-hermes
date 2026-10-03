#pragma once

#include <stdint.h>

/* Nous Girl（Hermes 的黑白小姑娘头像）—— 由 assets/nous-girl-white.svg 生成。
 * 工具：scratch/svg2gray.py（4 倍超采样 + even-odd + 4 级灰度）。
 * 每像素 2 bit，存的是**调色板索引**（0=近黑 3=暗灰 2=中灰 1=米白），高位先。 */

#define BUDDY_GIRL_W 180
#define BUDDY_GIRL_H 180
#define BUDDY_GIRL_STRIDE 45

extern const uint8_t buddy_girl_bits[BUDDY_GIRL_H * BUDDY_GIRL_STRIDE];
