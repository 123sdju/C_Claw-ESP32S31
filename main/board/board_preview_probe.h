#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** @brief 固定 240×240 预览帧的 900 点采样诊断，仅比较采样字节及平均颜色。 */
typedef struct {
    uint32_t hash; /**< 采样字节的 FNV 风格散列，按 uint32_t 模运算更新。 */
    uint32_t rgb; /**< 平均 R、G、B 各自归一化为 0 到 255，打包为 0x00RRGGBB。 */
} board_preview_probe_t;

/**
 * @brief 每隔八个像素采样一次小端 RGB565，计算散列和平均颜色。
 * @param[in] data 非 NULL 帧借用，至少覆盖 240 行，整个读取期不可被生产者修改。
 * @param[in] stride 源行跨度，单位为字节，至少 480，帧存储须覆盖全部采样偏移。
 * @return 按值采样结果，不分配或借用输出资源。
 * @note 只读取每行有效像素，行尾填充不参与采样，散列相同仅说明采样值相同。
 */
static inline board_preview_probe_t board_preview_probe(const uint8_t *data, size_t stride)
{
    uint32_t hash = 2166136261U, r = 0, g = 0, b = 0;
    for (size_t y = 4; y < 240; y += 8) {
        for (size_t x = 4; x < 240; x += 8) {
            const uint8_t *p = data + y * stride + x * 2;
            uint16_t c = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
            hash = (hash ^ p[0]) * 16777619U;
            hash = (hash ^ p[1]) * 16777619U;
            r += (c >> 11) & 31U;
            g += (c >> 5) & 63U;
            b += c & 31U;
        }
    }
    board_preview_probe_t result = {
        .hash = hash,
        .rgb = ((r * 255U / (900U * 31U)) << 16) |
               ((g * 255U / (900U * 63U)) << 8) |
               (b * 255U / (900U * 31U)),
    };
    return result;
}

/**
 * @brief 以红蓝均值减绿色分量度量偏粉程度。
 * @param[in] rgb 0x00RRGGBB 平均颜色，高八位忽略。
 * @return -255 到 255 的诊断分值。
 */
static inline int board_preview_pink_score(uint32_t rgb)
{
    return ((int)((rgb >> 16) & 255U) + (int)(rgb & 255U)) / 2 - (int)((rgb >> 8) & 255U);
}

/**
 * @brief 判断本帧偏粉分值是否至少 48 且相对前帧增加至少 48。
 * @param[in] before 前帧的平均 RGB 值。
 * @param[in] after 本帧的平均 RGB 值。
 * @return 两个阈值均达到为 true，否则 false。
 * @note 场景或光照变化同样可能触发，该结果只供诊断记录。
 */
static inline bool board_preview_pink_jump(uint32_t before, uint32_t after)
{
    int score = board_preview_pink_score(after);
    return score >= 48 && score - board_preview_pink_score(before) >= 48;
}
