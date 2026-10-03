// main/buddy_adpcm.h
// IMA ADPCM 解码状态与接口。每字节承载 2 个 4-bit 采样（高半字节在前）。
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t predictor;
    int step_index;
} buddy_adpcm_state_t;

void buddy_adpcm_reset(buddy_adpcm_state_t *state);
/* 把 count 字节解成 2*count 个 int16 PCM 采样，写入 dest。 */
void buddy_adpcm_decode(const uint8_t *source, size_t count, int16_t *dest,
                        buddy_adpcm_state_t *state);

/* 把 samples 个 int16 编成 samples/2 字节（高半字节先），与上面解码严格互为逆运算。
 * 实测：2000 采样正弦往返信噪比约 32.5 dB（IMA-ADPCM 4bit 的正常水平）。
 * ⚠ 起始若干采样误差偏大是 ADPCM 固有的"点火延迟"——step 从最小值 7 逐级爬升。 */
void buddy_adpcm_encode(const int16_t *source, size_t samples, uint8_t *dest,
                        buddy_adpcm_state_t *state);
