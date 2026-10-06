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

  volatile uint8_t cur_on;      /* 编辑光标：1=显示（硬件光标，lcd1602_write_frame） */
  volatile uint8_t cur_row;     /* 光标行 0/1 */
  volatile uint8_t cur_col;     /* 光标列 0~15 */

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

/* 更新"显示快照"（带编辑光标参数）。跨任务写共享数据的标准姿势，照抄即可。 */
static void app_display_ex(const char *l1, const char *l2,
                           uint8_t cur_on, uint8_t cur_row, uint8_t cur_col)
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
  s_app.cur_on  = cur_on;
  s_app.cur_row = cur_row;
  s_app.cur_col = cur_col;
  s_app.dirty = 1U;
  app_unlock(pm);
}

/* 不带光标版（菜单 / 结果 / 错误 / 欢迎页等"特殊视图"用） */
static void app_display(const char *l1, const char *l2)
{
  app_display_ex(l1, l2, 0U, 0U, 0U);
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
 * MODE = 设置菜单（COMP/CMPLX、DEG/RAD 全局设置，OK 确认退出）；x10^x(T27) = 科学计数记号的 'E'；
 * SHIFT = 修饰状态键：按后下一键出副功能（见 app_shift_alt）——7→π、4→e、5→log(、
 * 6→ln(、1→sin(、2→cos(、3→tan(、÷→sqrt(；8→∠、9→i、*→x^y 属复数/幂运算，暂未支持。
 * FMT(T28) = 显示精度切换（2 / 4 / 6 位小数）；方向键 = 编辑光标移动；
 * OK(T4) = MODE 菜单的确认/退出键（仅菜单内有效，菜单外按下无动作）。 */
#define APP_CH_AC    0x01U   /* AC：全清 */
#define APP_CH_UNDO  0x08U   /* BACK：撤销（式子粒度回滚，深度 8） */
#define APP_CH_DEL   0x7FU   /* DEL：删除光标左侧一个字符 */
#define APP_CH_EQ    0x0DU   /* =：求值（EXE 键） */
#define APP_CH_SHIFT 0x02U   /* SHIFT：修饰状态键（副功能见 app_shift_alt） */
#define APP_CH_MODE  0x04U   /* MODE：设置菜单（COMP/CMPLX、DEG/RAD） */
#define APP_CH_FMT   0x06U   /* FMT：循环切换显示精度（2 / 4 / 6 位小数） */
#define APP_CH_UP    0x11U   /* ↑：光标上移一行（-16 字符） */
#define APP_CH_DOWN  0x12U   /* ↓：光标下移一行（+16 字符） */
#define APP_CH_LEFT  0x13U   /* ←：光标左移一字符 */
#define APP_CH_RIGHT 0x14U   /* →：光标右移一字符 */
#define APP_CH_OK    0x15U   /* OK：MODE 菜单的确认/退出键（仅菜单内有效） */

static const uint8_t s_key_map[30] =
{
  /* T0          T1          T2          T3        T4     T5     T6     T7         T8         T9          */
    APP_CH_SHIFT, APP_CH_UNDO, APP_CH_MODE, APP_CH_UP, APP_CH_OK, '(',  ')',  APP_CH_LEFT, APP_CH_DOWN, APP_CH_RIGHT,
  /* T10    T11    T12    T13        T14        T15    T16    T17    T18    T19 */
    '7',   '8',   '9',   APP_CH_DEL, APP_CH_AC, '4',   '5',   '6',   '*',   '/',
  /* T20    T21    T22    T23    T24    T25    T26    T27    T28    T29 */
    '1',   '2',   '3',   '+',   '-',   '0',   '.',   'E',   APP_CH_FMT, APP_CH_EQ,
};

static char    s_expr[32];       /* 输入的表达式（C 字符串） */
static uint8_t s_expr_len;
static uint8_t s_just_eval;      /* 刚按过 = ：下一次按数字要开新算式 */
static uint8_t s_shift_active;   /* SHIFT 修饰态：下一次按键消费掉它 */
static uint8_t s_cmplx_mode;     /* 计算模式：0=COMP，1=CMPLX（MODE 菜单里切换） */
static uint8_t s_angle_rad;      /* 角度制：0=DEG（角度），1=RAD（弧度）；全局设置 */
static uint8_t s_mode_menu;      /* 1 = 正在 MODE 设置菜单里 */
static uint8_t s_frac_digits = 2U;  /* FMT：结果显示的小数位数（2 / 4 / 6） */
static uint8_t s_cursor;            /* 编辑光标：表达式内索引（0 ~ s_expr_len，≤31） */

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

/* SHIFT 副功能：激活态下按这些"主功能字符键" → 输入功能记号（多字符）而不是主字符。
 * 返回 NULL = 该键没有副功能（按主功能处理）。 */
static const char *app_shift_alt(uint8_t c)
{
  switch (c)
  {
    case '7': return "pi";
    case '4': return "e";
    case '5': return "log(";
    case '6': return "ln(";
    case '1': return "sin(";
    case '2': return "cos(";
    case '3': return "tan(";
    case '/': return "sqrt(";
    case '9': return (s_cmplx_mode != 0U) ? "i" : NULL;   /* 虚数单位：仅 COMPLX 模式 */
    case '8': return (s_cmplx_mode != 0U) ? "@" : NULL;   /* 极坐标 a@θ：仅 COMPLX 模式 */
    default:  return NULL;      /* *→x^y 幂运算暂未支持 */
  }
}

/* MODE 设置菜单：第一行是菜单项，第二行显示当前两项设置 + "[OK]"退出提示。
 * 菜单态下：按 1 切换 COMP/CMPLX，按 2 切换 DEG/RAD，按 OK 确认退出；其它键无反应。 */
static void app_mode_menu_show(void)
{
  char t[17];
  uint8_t j = 0U;
  const char *m = (s_cmplx_mode != 0U) ? "CMPLX" : "COMP";
  const char *a = (s_angle_rad != 0U) ? "RAD" : "DEG";
  const char *o = " [OK]";

  for (uint8_t i = 0U; m[i] != '\0'; i++)
  {
    t[j++] = m[i];
  }
  t[j++] = ' ';
  for (uint8_t i = 0U; a[i] != '\0'; i++)
  {
    t[j++] = a[i];
  }
  for (uint8_t i = 0U; o[i] != '\0'; i++)
  {
    t[j++] = o[i];
  }
  t[j] = '\0';

  app_display("1:CMPLX 2:ANGLE", t);
}

/* 编辑视图显示：表达式分两行铺满屏（每行 16 字符，最长 31 字符正好放下），
 * 硬件光标跟随编辑位置——按方向键移动时刷新这条路径即可"实时跟着动"。
 * 第二行没被表达式占满（len ≤ 16）时，显示调用者给的提示文字（彩蛋 / EXE to evaluate 等）。 */
static void app_calc_show_hint(const char *hint)
{
  char l1[16];
  char l2[16];

  for (uint8_t k = 0U; k < 16U; k++)
  {
    l1[k] = (k < s_expr_len) ? s_expr[k] : ' ';
    l2[k] = ((uint8_t)(16U + k) < s_expr_len) ? s_expr[16U + k] : ' ';
  }

  if (s_expr_len <= 16U)
  {
    app_fill16(l2, hint);            /* 第二行空着 → 放提示文字 */
  }
  if (s_expr_len == 0U)
  {
    l1[0] = '0';                     /* 空闲主界面第一行显示 "0" */
  }

  app_display_ex(l1, l2, 1U,
                 (s_cursor >= 16U) ? 1U : 0U,
                 (uint8_t)(s_cursor % 16U));
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

/* 向表达式缓冲追加一个字符（单字符与函数序列共用）：
 *   - 处于"空 / 刚求值"状态时的第一击 = 一段新输入的开始 → 记撤销点；
 *   - 刚求值状态下：运算符 / E 接着结果写，其它字符开新算式；
 *   - 满 31 字符后丢弃（给 '\0' 留位）。 */
static void app_calc_put(uint8_t c)
{
  if ((s_expr_len == 0U) || (s_just_eval != 0U))
  {
    app_undo_push();
  }

  if (s_just_eval != 0U)
  {
    if (!((c == '+') || (c == '-') || (c == '*') || (c == '/') || (c == 'E')))
    {
      s_expr_len = 0U;
      s_expr[0] = '\0';
      s_cursor = 0U;                 /* 开新算式：光标回开头 */
    }
    s_just_eval = 0U;
  }

  if (s_expr_len < (uint8_t)(sizeof(s_expr) - 1U))
  {
    /* 在光标处插入：光标之后的字符整体右移一格，光标随输入前进 */
    for (uint8_t i = s_expr_len; i > s_cursor; i--)
    {
      s_expr[i] = s_expr[i - 1U];
    }
    s_expr[s_cursor] = (char)c;
    s_expr_len++;
    s_cursor++;
    s_expr[s_expr_len] = '\0';
  }
}

/* 追加一串字符（函数记号 "sin(" 等），整串算一个输入动作 */
static void app_calc_put_str(const char *s)
{
  for (uint8_t i = 0U; s[i] != '\0'; i++)
  {
    app_calc_put((uint8_t)s[i]);
  }
}

/* 所有输入（触摸键、以后想加的串口命令）都从这里进 */
static void app_calc_input(uint8_t c)
{
  /* 0) MODE 菜单态：按 1 / 2 切换设置（改完留在菜单里继续调），按 OK 确认退出；
   *    其它键一律无反应——退出只能走 OK（再按 MODE 也不退出），防误触。 */
  if (s_mode_menu != 0U)
  {
    if (c == '1')
    {
      s_cmplx_mode ^= 1U;
      app_mode_menu_show();
    }
    else if (c == '2')
    {
      s_angle_rad ^= 1U;
      app_mode_menu_show();
    }
    else if (c == APP_CH_OK)
    {
      s_mode_menu = 0U;
      app_calc_show();
    }
    return;
  }

  /* 0.5) OK：MODE 菜单的确认键；菜单外按下不产生任何动作。
   *      刻意放在 SHIFT 消费之前——SHIFT+OK 不吞 SHIFT 修饰态（OK 视为不存在）。 */
  if (c == APP_CH_OK)
  {
    return;
  }

  /* 1) SHIFT 是一次性修饰键：它之后的第一个按键就消费它；
   *    被消费的键若有副功能（app_shift_alt），插入功能记号并结束。 */
  if (s_shift_active != 0U)
  {
    const char *alt;

    s_shift_active = 0U;
    alt = app_shift_alt(c);
    if (alt != NULL)
    {
      app_calc_put_str(alt);
      app_calc_show();
      return;
    }
    /* 该键没有副功能：落回下面按主功能处理 */
  }

  /* 2) SHIFT：进入修饰态——第一行保持当前表达式，第二行提示状态 */
  if (c == APP_CH_SHIFT)
  {
    s_shift_active = 1U;
    app_calc_show_hint("SHIFT ACTIVE");
    return;
  }

  /* 3) MODE：进入设置菜单 */
  if (c == APP_CH_MODE)
  {
    s_mode_menu = 1U;
    app_mode_menu_show();
    return;
  }

  /* 3.5) FMT：循环切换显示精度 2 → 4 → 6 → 2，第二行提示当前设置 */
  if (c == APP_CH_FMT)
  {
    char t[8];

    s_frac_digits = (s_frac_digits == 2U) ? 4U : ((s_frac_digits == 4U) ? 6U : 2U);
    t[0] = 'F'; t[1] = 'I'; t[2] = 'X'; t[3] = ':'; t[4] = ' ';
    t[5] = (char)('0' + s_frac_digits);
    t[6] = '\0';
    app_calc_show_hint(t);
    return;
  }

  /* 4) AC 全清（清之前记撤销点，BACK 可恢复到清空前） */
  if (c == APP_CH_AC)
  {
    if (s_expr_len > 0U)
    {
      app_undo_push();
    }
    s_expr_len = 0U;
    s_expr[0] = '\0';
    s_just_eval = 0U;
    s_cursor = 0U;                     /* 清空后光标回开头 */
    app_calc_show();
    return;
  }

  /* 5) BACK：撤销——整个式子回滚到上一个式子状态（栈空则无反应） */
  if (c == APP_CH_UNDO)
  {
    if (app_undo_pop() != 0U)          /* 连 just_eval 一起还原 */
    {
      s_cursor = s_expr_len;           /* 快照没存光标：回滚后保守地放末尾 */
      app_calc_show();
    }
    return;
  }

  /* 6) DEL：删除光标左侧那个字符（像退格；光标在开头就不动）。
   * DEL 是对当前式子的编辑、不产生撤销点——BACK 只按"式子"粒度回滚。 */
  if (c == APP_CH_DEL)
  {
    if (s_cursor > 0U)
    {
      for (uint8_t i = s_cursor - 1U; i < s_expr_len; i++)
      {
        s_expr[i] = s_expr[i + 1U];    /* 光标之后的字符整体左移一格 */
      }
      s_expr_len--;
      s_cursor--;
      s_expr[s_expr_len] = '\0';
    }
    app_calc_show();
    return;
  }

  /* 6.5) 方向键：移动编辑光标，立即刷新（光标随显示快照到屏上）。
   *   ←/→：移动一字符；↑/↓：跨行（每行 16 字符；↓ 只在下行有内容时允许） */
  if (c == APP_CH_LEFT)
  {
    if (s_cursor > 0U)
    {
      s_cursor--;
    }
    app_calc_show();
    return;
  }
  if (c == APP_CH_RIGHT)
  {
    if (s_cursor < s_expr_len)
    {
      s_cursor++;
    }
    app_calc_show();
    return;
  }
  if (c == APP_CH_UP)
  {
    if (s_cursor >= 16U)
    {
      s_cursor -= 16U;
    }
    app_calc_show();
    return;
  }
  if (c == APP_CH_DOWN)
  {
    if ((uint8_t)(s_cursor + 16U) <= s_expr_len)
    {
      s_cursor += 16U;
    }
    app_calc_show();
    return;
  }

  /* 7) = 求值（求值成功时由 app_calc_evaluate 记撤销点并回填） */
  if (c == APP_CH_EQ)
  {
    app_calc_evaluate();
    return;
  }

  /* 8) 追加字符：put 内部负责"新输入段记撤销点"与 just_eval 状态机 */
  app_calc_put(c);

  /* 9) 刷新显示 */
  app_calc_show();
}

/* 把 float 变成短字符串：整数部分 + frac 位小数（四舍五入；frac ∈ {2,4,6}）。
 * 例（frac=2）：7 → "7.00"；-2.5 → "-2.50"；超范围/NaN → "Error" */
static void app_format_float(float v, char *out, uint8_t out_size, uint8_t frac)
{
  char     tmp[16];
  uint8_t  n = 0U;
  uint8_t  j = 0U;
  uint8_t  neg = 0U;
  uint32_t ip;
  uint32_t fp;
  uint32_t scale = 1U;
  uint32_t div;

  for (uint8_t i = 0U; i < frac; i++)
  {
    scale *= 10U;                          /* 10^frac */
  }

  /* 显示分辨率以下的浮点残渣归零（如 sin(30)+cos(60)-1 的 1e-8 噪声），
   * 免得上屏出现 "-0.00" 这类噪点 */
  if ((v > -(0.5f / (float)scale)) && (v < (0.5f / (float)scale)))
  {
    v = 0.0f;
  }

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

  ip = (uint32_t)v;                                      /* 整数部分 */
  fp = (uint32_t)(((v - (float)ip) * (float)scale) + 0.5f); /* 小数部分，四舍五入 */
  if (fp >= scale)                                       /* 进位：0.999.. → 1.00 */
  {
    ip++;
    fp -= scale;
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
  div = scale / 10U;
  for (uint8_t i = 0U; i < frac; i++)
  {
    if ((j + 1U) < out_size) { out[j++] = (char)('0' + (fp / div) % 10U); }
    div /= 10U;
  }
  out[j] = '\0';
}

/* 把结果（实数或复数）变成显示字符串，返回长度。
 * 复数格式："a+bi" / "a-bi"；某个分量近似为 0 时省略（2@90 → "2.00i"）。
 * 近似阈值 = 半个显示单位，和 app_format_float 的归零口径一致。 */
static uint8_t app_format_value(const calc_complex_t *v, char *r, uint8_t size)
{
  float   eps = 0.5f;
  uint8_t j = 0U;
  uint8_t show_im;

  for (uint8_t i = 0U; i < s_frac_digits; i++)
  {
    eps /= 10.0f;                      /* 0.5 / 10^frac */
  }
  show_im = !((v->imag > -eps) && (v->imag < eps));  /* 虚部≈0 → 按实数显示 */

  /* 实部：仅当它有存在感时输出 */
  if ((show_im == 0U) || !((v->real > -eps) && (v->real < eps)))
  {
    app_format_float(v->real, r, size, s_frac_digits);
    while (r[j] != '\0')
    {
      j++;
    }
  }
  else
  {
    r[0] = '\0';                       /* 实部≈0 且虚部非零：只显示虚部 */
  }

  if (show_im != 0U)
  {
    char  t[20];
    float ai = v->imag;
    char  sc = '+';

    if (ai < 0.0f)
    {
      sc = '-';
      ai = -ai;
    }
    app_format_float(ai, t, (uint8_t)sizeof(t), s_frac_digits);
    if ((j + 1U) < size) { r[j++] = sc; }
    for (uint8_t i = 0U; t[i] != '\0' && (j + 1U) < size; i++)
    {
      r[j++] = t[i];
    }
    if ((j + 1U) < size) { r[j++] = 'i'; }
  }
  r[j] = '\0';
  return j;
}

/* '='：求值 → 显示 → 结果回填（供继续运算），模仿卡西欧的连续计算手感。
 * 【为什么在控制任务里直接算】s_expr 只有控制任务一个写者，
 * 不会出现两个任务同时改一个缓冲的竞态；求值器很小，栈够用。 */
static void app_calc_evaluate(void)
{
  calc_complex_t res;
  calc_status_t  st;
  char    r[48];
  char    line1[17];
  uint8_t rlen;
  uint8_t fill;

  if (s_expr_len == 0U)
  {
    return;                              /* 空表达式：当没按过 */
  }

  st = calculator_evaluate(s_expr,
                           (s_angle_rad != 0U) ? CALC_ANGLE_RAD : CALC_ANGLE_DEG,
                           s_cmplx_mode,
                           (calc_complex_t){0},
                           &res);

  if (st == CALC_OK)
  {
    app_undo_push();      /* 记撤销点：按 BACK 可回到求值前的表达式 */
    rlen = app_format_value(&res, r, (uint8_t)sizeof(r));

    /* 显示行："=" + 结果（例 "=7.00"、"=-5.00+10.00i"）；
     * 结果超过 15 字符时显示尾部（保住虚部信息） */
    if (rlen <= 15U)
    {
      line1[0] = '=';
      for (uint8_t i = 0U; i < rlen; i++)
      {
        line1[1U + i] = r[i];
      }
      line1[1U + rlen] = '\0';
    }
    else
    {
      uint8_t off = (uint8_t)(rlen - 15U);

      for (uint8_t i = 0U; i < 15U; i++)
      {
        line1[i] = r[off + i];
      }
      line1[15] = '\0';
    }

    /* 回填：s_expr 变成结果字符串——复数带隐式乘法记号（"10.00i"），
     * 引擎可直接再解析；超长结果按缓冲上限截断。
     * just_eval=1 → 下一次按数字则开新算式（卡西欧式行为） */
    fill = (rlen > (uint8_t)(sizeof(s_expr) - 1U)) ? (uint8_t)(sizeof(s_expr) - 1U) : rlen;
    for (uint8_t i = 0U; i < fill; i++)
    {
      s_expr[i] = r[i];
    }
    s_expr[fill] = '\0';
    s_expr_len = fill;
    s_cursor = fill;      /* 结果显示/回填后，光标回到末尾 */
    s_just_eval = 1U;

    app_display(line1, "BACK=undo AC=clr");
  }
  else if (st == CALC_DIV_ZERO)
  {
    app_display(s_expr, "! div by 0");
  }
  else if (st == CALC_DOMAIN)
  {
    app_display(s_expr, "! domain");
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
  char     l1[16], l2[16];
  uint8_t  cur_on, cur_row, cur_col;

  for (;;)
  {
    if (s_app.dirty != 0U)
    {
      /* 用最短的临界区把快照（含光标位置）拷出来 */
      uint32_t pm = app_lock();
      for (uint8_t i = 0U; i < 16U; i++)
      {
        l1[i] = s_app.line1[i];
        l2[i] = s_app.line2[i];
      }
      cur_on  = s_app.cur_on;
      cur_row = s_app.cur_row;
      cur_col = s_app.cur_col;
      s_app.dirty = 0U;
      app_unlock(pm);

      /* 慢操作放在临界区外面；带硬件光标（编辑视图显示，方向键移动即这里跟进） */
      lcd1602_write_frame(l1, l2, cur_on, cur_row, cur_col);
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