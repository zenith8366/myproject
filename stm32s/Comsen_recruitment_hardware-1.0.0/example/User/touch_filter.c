/* ============================================================================
 * touch_filter.c —— 你自己的按键滤波（替换 lib/libtouch_filter.a）
 *
 * 【契约】函数名和签名必须与 Core/Inc/touch_filter.h 完全一致：
 *     void     touch_filter_init(touch_filter_t *filter);
 *     uint32_t touch_filter_update(touch_filter_t *filter, uint32_t raw_bitmap);
 *   接口约定：每 10ms 调用一次；返回 0 或单个物理键位。
 *   结构体 touch_filter_t 的字段（counters / stable_bitmap /
 *   active_bitmap / wait_all_released）就是留给你用的"草稿纸"。
 * ========================================================================== */

#include "touch_filter.h"

#define KEY_STABLE_SAMPLES 3U    /* 连续 3 次 × 10ms = 30ms 才认定按下 */

void touch_filter_init(touch_filter_t *filter)
{
  for (uint8_t i = 0U; i < 30U; i++)
  {
    filter->counters[i] = 0U;
  }
  filter->stable_bitmap = 0U;
  filter->active_bitmap = 0U;
  filter->wait_all_released = 0U;
}

/* 每 10ms 调用一次。D4 目标：返回"当前稳定按着的单个键位"，没有则返回 0 */
uint32_t touch_filter_update(touch_filter_t *filter, uint32_t raw_bitmap)
{
  /* TODO(你写)（D4 雏形）：
   * 第 1 步 逐位计数 i = 0..29：
   *     raw 这一位置 1 → counters[i] 加一（到 KEY_STABLE_SAMPLES 封顶）；
   *     否则 → counters[i] 清零；
   * 第 2 步 合成 stable_bitmap：把计数达标的位都置进去；
   * 第 3 步 返回：stable_bitmap == 0 → 返回 0；
   *     否则返回"编号最小"的那一位（1UL << __builtin_ctz(stable_bitmap)）。 */
  (void)filter;
  (void)raw_bitmap;
  return 0U;
}