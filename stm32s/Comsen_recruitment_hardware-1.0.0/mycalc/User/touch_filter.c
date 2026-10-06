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

/* 每 10ms 调用一次。D5 语义升级：只在"新按下"的瞬间返回那个键位，
 * 按住不放不会重复触发；必须松手才能触发下一个键。 */
uint32_t touch_filter_update(touch_filter_t *filter, uint32_t raw_bitmap)
{
  uint32_t stable = 0U;

  /* 第 1+2 步：逐位计数并合成 stable_bitmap。
   * 本函数每 10ms 调一次：同一位连续 KEY_STABLE_SAMPLES 次读到 1 才认定"稳定按下"
   * （3 次 × 10ms = 30ms 去抖）；中途读到 0 就清零重新数。 */
  for (uint8_t i = 0U; i < 30U; i++)
  {
    if (((raw_bitmap >> i) & 1UL) != 0UL)
    {
      if (filter->counters[i] < KEY_STABLE_SAMPLES)
      {
        filter->counters[i]++;
      }
    }
    else
    {
      filter->counters[i] = 0U;
    }

    if (filter->counters[i] >= KEY_STABLE_SAMPLES)
    {
      stable |= (1UL << i);
    }
  }
  filter->stable_bitmap = stable;

  /* 第 3 步：单键锁定（边沿触发——D5 升级）。
   * wait_all_released 是个小状态机：
   *   - 报过一个键后就"锁上"，锁定期间无论读到什么都返回 0；
   *   - 等所有手指离开（stable 变 0）才重新武装，允许触发下一个键。
   * 手感 = 真实计算器：按住不放只触发一次，必须松手才能按下一个键。 */
  if (filter->wait_all_released != 0U)
  {
    if (stable == 0UL)
    {
      filter->wait_all_released = 0U;      /* 手都离开了：重新武装 */
    }
    return 0UL;                            /* 锁定期间（含解锁这一刻）不报键 */
  }

  if (stable != 0UL)
  {
    filter->wait_all_released = 1U;        /* 报键前先锁上 */
    return (uint32_t)(1UL << __builtin_ctz(stable));
  }

  return 0UL;
}