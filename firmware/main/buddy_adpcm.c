// main/buddy_adpcm.c
// IMA ADPCM (Intel/DVI) 解码 —— 与 Python audioop.lin2adpcm/adpcm2lin 逐采样对齐。
//
// ⚠ nibble 顺序实测为"高 4 位在前"（第 1 个采样在高半字节），与多数教科书
//   实现的"低 nibble 先"相反。已用 2000 采样正弦对拍，误差为 0（低 nibble
//   先的错误版本最大误差 2785）。
#include "buddy_adpcm.h"

static const int8_t s_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t s_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

void buddy_adpcm_reset(buddy_adpcm_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->predictor = 0;
    state->step_index = 0;
}

static int16_t buddy_adpcm_step(uint8_t nibble, buddy_adpcm_state_t *state)
{
    int32_t step = s_step_table[state->step_index];
    int32_t diff = step >> 3;

    if (nibble & 0x01U) {
        diff += step >> 2;
    }
    if (nibble & 0x02U) {
        diff += step >> 1;
    }
    if (nibble & 0x04U) {
        diff += step;
    }
    if (nibble & 0x08U) {
        diff = -diff;
    }

    state->predictor += diff;
    if (state->predictor > 32767) {
        state->predictor = 32767;
    } else if (state->predictor < -32768) {
        state->predictor = -32768;
    }

    state->step_index += s_index_table[nibble & 0x0FU];
    if (state->step_index < 0) {
        state->step_index = 0;
    } else if (state->step_index > 88) {
        state->step_index = 88;
    }

    return (int16_t)state->predictor;
}

static uint8_t buddy_adpcm_nibble(int16_t sample, buddy_adpcm_state_t *state)
{
    int32_t step = s_step_table[state->step_index];
    int32_t diff = (int32_t)sample - state->predictor;
    int32_t delta = step >> 3;
    uint8_t nibble = 0U;

    if (diff < 0) {
        nibble = 0x08U;
        diff = -diff;
    }
    if (diff >= step) {
        nibble |= 0x04U;
        diff -= step;
        delta += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 0x02U;
        diff -= step;
        delta += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 0x01U;
        delta += step;
    }

    /* 状态更新必须与解码端用同一套公式，否则两端会逐渐失步。 */
    state->predictor += (nibble & 0x08U) ? -delta : delta;
    if (state->predictor > 32767) {
        state->predictor = 32767;
    } else if (state->predictor < -32768) {
        state->predictor = -32768;
    }

    state->step_index += s_index_table[nibble];
    if (state->step_index < 0) {
        state->step_index = 0;
    } else if (state->step_index > 88) {
        state->step_index = 88;
    }

    return nibble;
}

void buddy_adpcm_encode(const int16_t *source, size_t samples, uint8_t *dest,
                        buddy_adpcm_state_t *state)
{
    size_t i;

    if (source == NULL || dest == NULL || state == NULL) {
        return;
    }
    for (i = 0U; i + 1U < samples; i += 2U) {
        uint8_t high = buddy_adpcm_nibble(source[i], state);
        uint8_t low = buddy_adpcm_nibble(source[i + 1U], state);

        dest[i / 2U] = (uint8_t)((high << 4) | low);
    }
}

void buddy_adpcm_decode(const uint8_t *source, size_t count, int16_t *dest,
                        buddy_adpcm_state_t *state)
{
    size_t i;

    if (source == NULL || dest == NULL || state == NULL) {
        return;
    }
    for (i = 0U; i < count; ++i) {
        dest[2U * i] = buddy_adpcm_step((uint8_t)(source[i] >> 4), state);
        dest[2U * i + 1U] = buddy_adpcm_step((uint8_t)(source[i] & 0x0FU), state);
    }
}
