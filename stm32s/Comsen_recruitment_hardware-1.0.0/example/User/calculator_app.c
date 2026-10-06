/* ============================================================================
 * calculator_app.c —— 你自己的应用层（替换 lib/libcalculator_app.a，同名同签名）
 *
 * 【契约】main.c 只会调用 calculator_app.h 里声明的这 6 个函数：
 *     calculator_app_init()         在调度器启动之前被调用
 *     calculator_heartbeat_task()   心跳（用来闪 PC13 的 LED）
 *     calculator_key_task()         读触摸按键
 *     calculator_lcd_task()         刷新 LCD（唯一允许碰 LCD 的任务）
 *     calculator_controller_task()  控制逻辑（按键、串口数据的汇合处）
 *     calculator_compute_task()     重计算（栈最大 2048 B）
 *   少写一个，链接就会报 undefined reference to `calculator_xxx_task'。
 *
 * 【纪律】除了这 6 个函数，本文件其它函数/变量全部 static，
 *         避免将来和静态库里的符号撞名。
 * ========================================================================== */

#include "calculator_app.h"   /* 保证与 main.c 看到的函数签名完全一致 */
#include "main.h"             /* HAL_GPIO_TogglePin / GPIOC / GPIO_PIN_13 */
#include "cmsis_os.h"         /* osDelay、栈高水位查询等（FreeRTOS 的封装） */
#include "lcd1602.h"          /* 显示库接口 */
#include "usb_device.h"       /* USBD_STATE_CONFIGURED 等 */
#include "usbd_cdc_if.h"      /* CDC_Transmit_FS / CDC_RxTake */
#include <string.h>           /* 不用也行，可删 */
#include "ttp229.h"           /* 物理按键原始读取（继续用库） */
#include "touch_filter.h"     /* 你的 touch_filter.c 的接口（同名） */
#include "calculator_engine.h" /* D6：求值器接口（你的 calculator_engine.c 实现） */

extern USBD_HandleTypeDef hUsbDeviceFS;   /* 定义在 usb_device.c */

/* ------------------------------- 可调参数 -------------------------------- */
#define APP_KEY_PERIOD_MS    10U    /* 触摸扫描周期：库约定 10ms 一次 */
#define APP_CTRL_PERIOD_MS   10U    /* 控制任务周期 */
#define APP_LCD_PERIOD_MS   100U    /* 刷新周期：LCD 库内部用 HAL_Delay 逐字节写，别设太小 */
#define APP_HB_PERIOD_MS    500U    /* 心跳灯：每 500ms 翻转一次 = 每秒闪 1 下 */
#define APP_SPLASH_MS      1500U    /* 欢迎页停留时长，之后自动切到主界面 */

/* -------------------------------- 共享状态 ------------------------------- */
/* 【FreeRTOS 学习点】5 个任务共享一份数据，规则只有两条：
 *   1) 每个字段只有一个"写者"任务；
 *   2) 读快照时用 app_lock()/app_unlock() 包住"拷贝那几个字节"的一瞬间，
 *      临界区里绝不做慢操作（LCD、延时、串口都是慢操作）。       */
typedef struct
{
  char             line1[16];   /* LCD 快照第 1 行：定长 16，不含结束符 */
  char             line2[16];   /* LCD 快照第 2 行 */
  volatile uint8_t dirty;       /* 有新内容置 1，lcdTask 显示完清 0 */

  uint8_t          key_fifo[16];/* 按键事件队列：keyTask 写、controllerTask 读 */
  volatile uint8_t key_head;
  volatile uint8_t key_tail;
} app_state_t;

static app_state_t s_app;       /* 放全局 .bss 区，不要放任务栈上！ */

/* -------------------------------- 小工具 --------------------------------- */

/* 超短临界区的开启/恢复：关中断 → 干那一两微秒的活 → 恢复原状态。
 * 【为什么不用 taskENTER_CRITICAL】那个 API 只有调度器启动之后才能正常配对；
 * 本文件的 app_display 在"调度器启动前"（calculator_app_init 里）也会被调用，
 * 所以用 PRIMASK 保存/恢复这招——不挑时机，而且和 usbd_cdc_if.c 里
 * CDC_TakeExportRequest_FS 的写法完全一致。 */
static uint32_t app_lock(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void app_unlock(uint32_t primask)
{
  if (primask == 0U)
  {
    __enable_irq();
  }
}

/* 数一串字符的长度，最多数到 16（不想依赖 strlen 就这么写） */
static uint8_t app_len16(const char *s)
{
  uint8_t n = 0U;
  while (n < 16U && s[n] != '\0')
  {
    n++;
  }
  return n;
}

/* 把字符串填成"恰好 16 字符"：不足补空格。
 * 【铁律 1】lcd1602_write_lines 的两行形参必须真的指向 16 字节，
 *           短了会读越界 → 屏幕花屏。 */
static void app_fill16(char out[16], const char *s)
{
  uint8_t len = app_len16(s);
  for (uint8_t i = 0U; i < 16U; i++)
  {
    out[i] = (i < len) ? s[i] : ' ';
  }
}

/* 更新"显示快照"。跨任务写共享数据的标准姿势，照抄即可。 */
static void app_display(const char *l1, const char *l2)
{
  char     t1[16], t2[16];
  uint32_t pm;

  app_fill16(t1, l1);
  app_fill16(t2, l2);

  pm = app_lock();
  for (uint8_t i = 0U; i < 16U; i++)
  {
    s_app.line1[i] = t1[i];
    s_app.line2[i] = t2[i];
  }
  s_app.dirty = 1U;
  app_unlock(pm);
}

/* ------------------------------- D4：按键事件 ----------------------------- */

static touch_filter_t s_filter;    /* 滤波状态（放全局，不放任务栈上） */

/* 按键事件队列：单生产者（keyTask）/ 单消费者（controllerTask）：
 * 一个写 head 一个写 tail，天然不需要加锁。 */
static void app_key_push(uint8_t ev)
{
  uint8_t next = (uint8_t)((s_app.key_head + 1U) & 0x0FU);

  if (next == s_app.key_tail)
  {
    return;                      /* 队满：丢事件（正常操作不会满） */
  }
  s_app.key_fifo[s_app.key_head] = ev;
  s_app.key_head = next;
}

static uint8_t app_key_pop(uint8_t *ev)
{
  if (s_app.key_head == s_app.key_tail)
  {
    return 0U;                   /* 队空 */
  }
  *ev = s_app.key_fifo[s_app.key_tail];
  s_app.key_tail = (uint8_t)((s_app.key_tail + 1U) & 0x0FU);
  return 1U;
}

/* ------------------------------- D3：串口收发 ------------------------------ */

/* 显示窗口：最近收到的 13 个字符（滚动效果），屏幕第一行 = "PC:" + 这 13 个 */
static char    s_pc_tail[13];
static uint8_t s_pc_any;         /* 是否收到过任何字符 */

/* 向电脑发送（回显用）。规矩背下来：
 *   1) 只有 controllerTask 调用它（单写者，下面静态缓冲不用抢锁）；
 *   2) USB 没枚举好就直接放弃；
 *   3) CDC 发送是"借用指针"的：数据要一直有效到发完，
 *      所以先拷进 s_tx_buf 再发。 */
static uint8_t s_tx_buf[128];

static void app_usb_send(const uint8_t *data, uint16_t len)
{
  USBD_CDC_HandleTypeDef *hcdc;

  if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
  {
    return;                              /* 电脑还没把这个设备准备就绪，别发 */
  }
  if (len > (uint16_t)sizeof(s_tx_buf))
  {
    len = (uint16_t)sizeof(s_tx_buf);
  }

  hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;

  /* 等上一次发完（最多 50ms），再把数据拷进发送缓冲 */
  for (uint8_t t = 0U; (t < 50U) && (hcdc != NULL) && (hcdc->TxState != 0U); t++)
  {
    osDelay(1U);
  }
  for (uint16_t i = 0U; i < len; i++)
  {
    s_tx_buf[i] = data[i];
  }

  /* 发送；忙就等 1ms 重试（最多 50 次） */
  for (uint8_t t = 0U; t < 50U; t++)
  {
    if (CDC_Transmit_FS(s_tx_buf, len) != USBD_BUSY)
    {
      return;
    }
    osDelay(1U);
  }
  /* 一直忙：通常出现在拔线之后。插回 USB 即恢复正常。 */
}

/* 每个控制周期调用一次：取走 USB 数据 → 滚进窗口 → 刷屏 */
static void app_usb_poll(void)
{
  uint8_t  buf[64];
  uint16_t n = CDC_RxTake(buf, (uint16_t)sizeof(buf));
  char     t[17];

  if (n == 0U)
  {
    return;
  }

  app_usb_send(buf, n);                  /* 回显：电脑端立刻能看到，调试神器 */

  for (uint16_t i = 0U; i < n; i++)
  {
    /* 把 buf[i] 滚进 s_pc_tail（滚动窗口）：
     * 先把 s_pc_tail[0..11] 左移一位，新字符放到 s_pc_tail[12]。
     * '\r'/'\n'（回车换行）在屏幕上显示成空格（1602 没有换行概念）。 */
    for (uint8_t k = 0U; k < 12U; k++)
    {
      s_pc_tail[k] = s_pc_tail[k + 1U];
    }
    s_pc_tail[12] = (buf[i] == '\r' || buf[i] == '\n') ? ' ' : (char)buf[i];
    s_pc_any = 1U;
  }

  /* 组装第一行 "PC:" + 13 字符窗口 */
  t[0] = 'P'; t[1] = 'C'; t[2] = ':';
  for (uint8_t k = 0U; k < 13U; k++)
  {
    t[3U + k] = s_pc_tail[k];
  }
  t[16] = '\0';

  app_display(t, (s_pc_any != 0U) ? "PC > LCD" : "send text...");
}

/* ------------------------------- D5：输入组织 ------------------------------ */

/* 物理键 T0..T29 → 输入字符；0xFF = 暂不映射（按下无反应）。
 * 下标 = D4 试触实测的实物编号；值 = 键位设计表（6 行 × 5 列，2026-10-06 定格）：
 *
 *         列1          列2           列3          列4           列5
 *   行1   T0 SHIFT     T1 BACK       T2 MODE      T3 ↑          T4 OK
 *   行2   T5 (         T6 )          T7 ←         T8 ↓          T9 →
 *   行3   T10 7(π)     T11 8(∠)      T12 9(i)     T13 DEL       T14 AC
 *   行4   T15 4(e)     T16 5(log)    T17 6(ln)    T18 *(x^y)    T19 ÷(√)
 *   行5   T20 1(sin)   T21 2(cos)    T22 3(tan)   T23 +         T24 -
 *   行6   T25 0        T26 .         T27 x10^x    T28 FMT(ANS)  T29 EXE
 *
 * 已启用：数字 / 小数点 / * ÷ + - / 括号 / AC / 撤销(BACK) / 删除(DEL) / 求值(EXE)；
 * SHIFT 已接为"修饰状态键"（按下后第二行提示 SHIFT ACTIVE，第一行保持当前内容）。
 * 预留未启用（0xFF）：MODE、方向键、OK、x10^x、FMT(ANS)——属任务 5 冲刺与自选功能。
 * SHIFT 副功能设计先记在这：7→π、8→∠、9→i、4→e、5→log、6→ln、*→x^y、÷→√、
 * 1→sin、2→cos、3→tan、FMT→ANS，待求值器支持后再接。 */
#define APP_CH_AC    0x01U   /* AC：全清 */
#define APP_CH_UNDO  0x08U   /* BACK：撤销（回滚到上一编辑步骤，深度 8） */
#define APP_CH_DEL   0x7FU   /* DEL：删除最后一个字符 */
#define APP_CH_EQ    0x0DU   /* =：求值（EXE 键） */
#define APP_CH_SHIFT 0x02U   /* SHIFT：修饰状态键（副功能待 D7 接入） */

static const uint8_t s_key_map[30] =
{
  /* T0          T1          T2     T3     T4     T5     T6     T7     T8     T9    */
    APP_CH_SHIFT, APP_CH_UNDO, 0xFFU, 0xFFU, 0xFFU, '(',   ')',   0xFFU, 0xFFU, 0xFFU,
  /* T10    T11    T12    T13        T14        T15    T16    T17    T18    T19 */
    '7',   '8',   '9',   APP_CH_DEL, APP_CH_AC, '4',   '5',   '6',   '*',   '/',
  /* T20    T21    T22    T23    T24    T25    T26    T27    T28    T29 */
    '1',   '2',   '3',   '+',   '-',   '0',   '.',   0xFFU, 0xFFU, APP_CH_EQ,
};

static char    s_expr[32];       /* 输入的表达式（C 字符串） */
static uint8_t s_expr_len;
static uint8_t s_just_eval;      /* 刚按过 = ：下一次按数字要开新算式 */
static uint8_t s_shift_active;   /* SHIFT 修饰态：下一次按键消费掉它（副功能待 D7） */

/* 撤销栈（BACK 键）：以"式子"为粒度——只在这些时刻记快照：
 *   ① 从"空 / 刚求值(结果)"状态开始输入第一个字符之前（记下式子起点的状态）；
 *   ② 按 '=' 求值前（记下整个式子）；
 *   ③ 按 AC 清空前（记下被清的式子）。
 * 输入中途的字符不产生快照——所以 BACK 是"整个式子一次回滚"，
 * 不是退格删字符（删单个字符请用 DEL 键）。
 * 满栈丢最旧的，保留最近 APP_UNDO_DEPTH 个式子状态。 */
#define APP_UNDO_DEPTH 8U
static char    s_undo[APP_UNDO_DEPTH][32];
static uint8_t s_undo_len[APP_UNDO_DEPTH];
static uint8_t s_undo_je[APP_UNDO_DEPTH];    /* 快照时的"刚求值"标志 */
static uint8_t s_undo_count;

/* 压入当前表达式快照（含结尾 '\0' 与 just_eval 标志） */
static void app_undo_push(void)
{
  if (s_undo_count >= APP_UNDO_DEPTH)      /* 满：整体左移，丢最旧一层 */
  {
    for (uint8_t i = 0U; (i + 1U) < APP_UNDO_DEPTH; i++)
    {
      for (uint8_t k = 0U; k <= s_undo_len[i + 1U]; k++)
      {
        s_undo[i][k] = s_undo[i + 1U][k];
      }
      s_undo_len[i] = s_undo_len[i + 1U];
      s_undo_je[i]  = s_undo_je[i + 1U];
    }
    s_undo_count = APP_UNDO_DEPTH - 1U;
  }
  for (uint8_t k = 0U; k <= s_expr_len; k++)
  {
    s_undo[s_undo_count][k] = s_expr[k];
  }
  s_undo_len[s_undo_count] = s_expr_len;
  s_undo_je[s_undo_count]  = s_just_eval;
  s_undo_count++;
}

/* 弹栈恢复（连"刚求值"标志一起还原）；栈空返回 0（无可回滚） */
static uint8_t app_undo_pop(void)
{
  if (s_undo_count == 0U)
  {
    return 0U;
  }
  s_undo_count--;
  s_expr_len = s_undo_len[s_undo_count];
  for (uint8_t k = 0U; k <= s_expr_len; k++)
  {
    s_expr[k] = s_undo[s_undo_count][k];
  }
  s_just_eval = s_undo_je[s_undo_count];
  return 1U;
}

static void app_calc_evaluate(void);   /* 前置声明：app_calc_input 的求值分支要调用它 */

/* 显示表达式（第二行提示文字由调用者给出）：
 * 超过 16 字符时显示"末尾 15 字符"，最左边放 '>' 提示被截断 */
static void app_calc_show_hint(const char *hint)
{
  char    t[17];
  uint8_t start;
  uint8_t j = 0U;

  if (s_expr_len == 0U)
  {
    app_display("0", hint);          /* 第一行"0" + 调用者的提示文字 */
    return;
  }

  start = (s_expr_len > 16U) ? (uint8_t)(s_expr_len - 15U) : 0U;
  if (start > 0U)
  {
    t[j++] = '>';
  }
  for (uint8_t k = start; k < s_expr_len; k++)
  {
    t[j++] = s_expr[k];
  }
  t[j] = '\0';

  app_display(t, hint);
}

/* 主界面空闲态的花样提示（每次进入空闲态时随机选一条） */
static const char *const s_fun_hints[] =
{
  "Hi! (^_^)",
  "I'm awake! :D",
  "Awaiting input_",
  "Hello, world! :)",
  "Let's do math! >",
  "Go Go Go! >_<",
};
#define APP_FUN_HINT_COUNT (sizeof(s_fun_hints) / sizeof(s_fun_hints[0]))

static uint32_t s_fun_seed;      /* 随机种子：0 = 还没初始化（首次用 tick 垫入） */

/* 返回一条随机彩蛋提示（xorshift32——短小够用，不当密码学随机用） */
static const char *app_fun_hint(void)
{
  if (s_fun_seed == 0U)
  {
    s_fun_seed = osKernelGetTickCount() | 1U;   /* 非零种子；xorshift 从非零出发永不为 0 */
  }
  s_fun_seed ^= (s_fun_seed << 13);
  s_fun_seed ^= (s_fun_seed >> 17);
  s_fun_seed ^= (s_fun_seed << 5);
  return s_fun_hints[s_fun_seed % (uint32_t)APP_FUN_HINT_COUNT];
}

/* 常规刷新：第二行提示按状态自动选（空闲随机彩蛋 / 输入中 EXE to evaluate） */
static void app_calc_show(void)
{
  app_calc_show_hint((s_expr_len == 0U) ? app_fun_hint() : "EXE to evaluate");
}

/* 所有输入（触摸键、以后想加的串口命令）都从这里进 */
static void app_calc_input(uint8_t c)
{
  /* SHIFT 是一次性修饰键：它之后的第一个按键就把它消费掉（副功能待 D7 接入）。
   * 先消费再处理本次按键——本次按键若还是 SHIFT，会在下面重新激活。 */
  if (s_shift_active != 0U)
  {
    s_shift_active = 0U;
  }

  /* 0) SHIFT：进入修饰态——第一行保持当前表达式，第二行提示状态 */
  if (c == APP_CH_SHIFT)
  {
    s_shift_active = 1U;
    app_calc_show_hint("SHIFT ACTIVE");
    return;
  }

  /* 1) AC 全清（清之前记撤销点，BACK 可恢复到清空前） */
  if (c == APP_CH_AC)
  {
    if (s_expr_len > 0U)
    {
      app_undo_push();
    }
    s_expr_len = 0U;
    s_expr[0] = '\0';
    s_just_eval = 0U;
    app_calc_show();
    return;
  }

  /* 2) BACK：撤销——整个式子回滚到上一个式子状态（栈空则无反应） */
  if (c == APP_CH_UNDO)
  {
    if (app_undo_pop() != 0U)          /* 连 just_eval 一起还原 */
    {
      app_calc_show();
    }
    return;
  }

  /* 3) DEL：删除最后一个字符（空了就不动）。
   * DEL 是对当前式子的编辑、不产生撤销点——BACK 只按"式子"粒度回滚。 */
  if (c == APP_CH_DEL)
  {
    if (s_expr_len > 0U)
    {
      s_expr_len--;
      s_expr[s_expr_len] = '\0';
    }
    app_calc_show();
    return;
  }

  /* 4) = 求值（求值成功时由 app_calc_evaluate 记撤销点并回填） */
  if (c == APP_CH_EQ)
  {
    app_calc_evaluate();
    return;
  }

  /* 5) 若本次按键要"开启一段新的输入"（当前为空，或刚求过值），
   *    先记下式子的起点状态——这是 BACK 的"上一个式子"回滚目标。
   *    输入中途的字符不记快照（否则 BACK 就退化成按字符删了）。 */
  if ((s_expr_len == 0U) || (s_just_eval != 0U))
  {
    app_undo_push();
  }

  /* 6) 刚求过值（= 后）的状态机：
   *    按数字/小数点 → 清空、开新算式；
   *    按运算符     → 保留结果字符串，在它上面继续算；
   *    两种情况都离开"刚求值"状态——否则后续数字会被误当成新算式清掉。 */
  if (s_just_eval != 0U)
  {
    if (((c >= '0') && (c <= '9')) || (c == '.'))
    {
      s_expr_len = 0U;
      s_expr[0] = '\0';
    }
    s_just_eval = 0U;
  }

  /* 7) 追加字符（给 '\0' 留一个位置） */
  if (s_expr_len < (uint8_t)(sizeof(s_expr) - 1U))
  {
    s_expr[s_expr_len] = (char)c;
    s_expr_len++;
    s_expr[s_expr_len] = '\0';
  }

  /* 8) 刷新显示 */
  app_calc_show();
}

/* 把 float 变成短字符串：整数部分 + 两位小数（四舍五入）。
 * 例：7 → "7.00"；-2.5 → "-2.50"；超范围/NaN → "Error" */
static void app_format_float(float v, char *out, uint8_t out_size)
{
  char     tmp[16];
  uint8_t  n = 0U;
  uint8_t  j = 0U;
  uint8_t  neg = 0U;
  uint32_t ip;
  uint32_t fp;

  if (!(v > -1.0e9f && v < 1.0e9f))     /* NaN 和无穷也走这里 */
  {
    const char *e = "Error";
    uint8_t k = 0U;
    while (e[k] != '\0' && (uint8_t)(k + 1U) < out_size)
    {
      out[k] = e[k];
      k++;
    }
    out[k] = '\0';
    return;
  }

  if (v < 0.0f)
  {
    neg = 1U;
    v = -v;
  }

  ip = (uint32_t)v;                                   /* 整数部分 */
  fp = (uint32_t)(((v - (float)ip) * 100.0f) + 0.5f); /* 小数部分，四舍五入 */
  if (fp >= 100U)                                     /* 进位：0.999.. → 1.00 */
  {
    ip++;
    fp -= 100U;
  }

  /* 整数部分倒着生成（% 10 取末位） */
  do
  {
    tmp[n++] = (char)('0' + (ip % 10U));
    ip /= 10U;
  } while (ip > 0U && n < 12U);

  if (neg && (j + 1U) < out_size)
  {
    out[j++] = '-';
  }
  while (n > 0U && (j + 1U) < out_size)
  {
    out[j++] = tmp[--n];               /* 反转回来 */
  }
  if ((j + 1U) < out_size) { out[j++] = '.'; }
  if ((j + 1U) < out_size) { out[j++] = (char)('0' + (fp / 10U)); }
  if ((j + 1U) < out_size) { out[j++] = (char)('0' + (fp % 10U)); }
  out[j] = '\0';
}

/* '='：求值 → 显示 → 结果回填（供继续运算），模仿卡西欧的连续计算手感。
 * 【为什么在控制任务里直接算】s_expr 只有控制任务一个写者，
 * 不会出现两个任务同时改一个缓冲的竞态；求值器很小，栈够用。 */
static void app_calc_evaluate(void)
{
  calc_complex_t res;
  calc_status_t  st;
  char    r[20];
  char    line1[17];
  uint8_t rlen;

  if (s_expr_len == 0U)
  {
    return;                              /* 空表达式：当没按过 */
  }

  st = calculator_evaluate(s_expr, CALC_ANGLE_DEG, 0U, (calc_complex_t){0}, &res);

  if (st == CALC_OK)
  {
    app_undo_push();      /* 记撤销点：按 BACK 可回到求值前的表达式 */
    app_format_float(res.real, r, (uint8_t)sizeof(r));
    rlen = app_len16(r);

    /* 显示行："=" + 结果，例如 "=7.00" */
    line1[0] = '=';
    for (uint8_t i = 0U; i < rlen; i++)
    {
      line1[1U + i] = r[i];
    }
    line1[1U + rlen] = '\0';

    /* 回填：s_expr 变成结果字符串（含结尾 '\0'），按运算符可接着算；
     * just_eval=1 → 下一次按数字则开新算式（卡西欧式行为） */
    for (uint8_t i = 0U; i <= rlen; i++)
    {
      s_expr[i] = r[i];
    }
    s_expr_len = rlen;
    s_just_eval = 1U;

    app_display(line1, "BACK=undo AC=clr");
  }
  else if (st == CALC_DIV_ZERO)
  {
    app_display(s_expr, "! div by 0");
  }
  else
  {
    app_display(s_expr, "! syntax");
  }
}

/* ============================== 6 个契约函数 ============================== */

/* 在调度器启动之前被调用：只能做"初始化和硬件自检"，不能用 osDelay！ */
void calculator_app_init(void)
{
  lcd1602_init();                          /* LCD 初始化（内部用 HAL_Delay，没问题） */
  touch_filter_init(&s_filter);
  app_display("MY  MINI  CASIO", "SOEI  2607  LYH"); /* TODO(你写)：改成你自己的欢迎语 */
}

/* 心跳任务：证明调度器活着、你的程序没被玩死 */
void calculator_heartbeat_task(void)
{
  for (;;)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    osDelay(APP_HB_PERIOD_MS);             /* 阻塞式延时：延时期间让出 CPU */
  }
}

/* 按键任务：每 10ms 读触摸 → 滤波 → 投递"新按下"事件 */
void calculator_key_task(void)
{
  for (;;)
  {
    uint32_t raw = ttp229_read_physical();                 /* 库：读 30 位位图 */
    uint32_t key = touch_filter_update(&s_filter, raw);    /* 你的：滤波 */

    if (key != 0U)                                /* 只在"新按下"瞬间有值 */
    {
      app_key_push((uint8_t)__builtin_ctz(key));
    }
    osDelay(APP_KEY_PERIOD_MS);
  }
}

/* 显示任务：全工程唯一允许调用 lcd1602_* 的任务（所以它不需要抢锁） */
void calculator_lcd_task(void)
{
  char l1[16], l2[16];

  for (;;)
  {
    if (s_app.dirty != 0U)
    {
      /* 用最短的临界区把快照拷出来 */
      uint32_t pm = app_lock();
      for (uint8_t i = 0U; i < 16U; i++)
      {
        l1[i] = s_app.line1[i];
        l2[i] = s_app.line2[i];
      }
      s_app.dirty = 0U;
      app_unlock(pm);

      lcd1602_write_lines(l1, l2);         /* 慢操作放在临界区外面 */
    }
    osDelay(APP_LCD_PERIOD_MS);
  }
}

/* 控制任务：D3/D4 在这里汇聚"串口数据 + 按键事件" */
void calculator_controller_task(void)
{
  uint8_t  ev;
  uint32_t t0 = osKernelGetTickCount();   /* 开机时刻 ≈ 本任务第一次运行 */
  uint8_t  intro_done = 0U;

  for (;;)
  {
    app_usb_poll();                 /* D3 */

    while (app_key_pop(&ev))        /* D4/D5：按键事件 → 输入 */
    {
      uint8_t ch = s_key_map[ev];
      if (ch != 0xFFU)              /* 0xFF = 该键未映射，忽略 */
      {
        app_calc_input(ch);
      }
    }

    /* 欢迎页展示 APP_SPLASH_MS 后自动进入主界面（"0" + "ready"）。
     * 欢迎期间若有按键，app_calc_input 已刷过屏；这里补刷一次只是重显当前状态，无害。 */
    if ((intro_done == 0U) && ((osKernelGetTickCount() - t0) >= APP_SPLASH_MS))
    {
      intro_done = 1U;
      app_calc_show();
    }

    osDelay(APP_CTRL_PERIOD_MS);
  }
}

/* 计算任务：栈最大（2048 B），高级功能留到这里 */
void calculator_compute_task(void)
{
  for (;;)
  {
    osDelay(20U);
  }
}