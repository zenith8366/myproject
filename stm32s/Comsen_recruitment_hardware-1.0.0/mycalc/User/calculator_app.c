/* ============================================================================
 * calculator_app.c —— 你自己的应用层（替换 lib/libcalculator_app.a，同名同签名）
 *
 * 【契约】main.c 只会调用 calculator_app.h 里声明的这 6 个函数：
 *     calculator_app_init()         在调度器启动之前被调用
 *     calculator_heartbeat_task()   心跳（用来闪 PC13 的 LED）
 *     calculator_key_task()         读触摸按键
 *     calculator_lcd_task()         刷新 LCD（唯一允许碰 LCD 的任务）
 *     calculator_controller_task()  控制逻辑（按键、串口数据的汇合处）
 *     calculator_compute_task()     重计算（栈最大 3328 B，表达式求值在这里跑）
 *   少写一个，链接就会报 undefined reference to `calculator_xxx_task'。
 *
 * 【纪律】除了这 6 个函数，本文件其它函数/变量全部 static，
 *         避免将来和静态库里的符号撞名。
 * ========================================================================== */

#include "calculator_app.h"   /* 保证与 main.c 看到的函数签名完全一致 */
#include "main.h"             /* HAL_GPIO_TogglePin / GPIOC / GPIO_PIN_13 */
#include "cmsis_os.h"         /* osDelay、栈高水位查询等（FreeRTOS 的封装） */
#include "queue.h"            /* StaticQueue_t：静态消息队列的控制块类型 */
#include "lcd1602.h"          /* 显示库接口 */
#include "usb_device.h"       /* USBD_STATE_CONFIGURED 等 */
#include "usbd_cdc_if.h"      /* CDC_Transmit_FS / CDC_RxTake */
#include <string.h>           /* memcmp（显示帧去重） */
#include "ttp229.h"           /* 物理按键原始读取（继续用库） */
#include "touch_filter.h"     /* 你的 touch_filter.c 的接口（同名） */
#include "calculator_engine.h" /* D6：求值器接口（你的 calculator_engine.c 实现） */

extern USBD_HandleTypeDef hUsbDeviceFS;   /* 定义在 usb_device.c */

/* ------------------------------- 可调参数 -------------------------------- */
#define APP_KEY_PERIOD_MS    10U    /* 触摸扫描周期：库约定 10ms 一次 */
#define APP_CTRL_PERIOD_MS   10U    /* 控制任务周期 */
#define APP_HB_PERIOD_MS    500U    /* 心跳灯：每 500ms 翻转一次 = 每秒闪 1 下 */
#define APP_SPLASH_MS      1500U    /* 欢迎页停留时长，之后自动切到主界面 */

/* --------------------------- 显示格式化常量 ------------------------------ */
/* app_format_float / app_format_value 的尺寸与阈值：别再用裸数字 */
#define FMT_INT_DIGITS_MAX  12U     /* 整数部分最多显示位数（缓冲的容量上限） */
#define FMT_INT_TMP_SIZE    16U     /* 整数位倒序生成的临时缓冲（≥ MAX+1） */
#define FMT_VALUE_MAX       48U     /* 单个数值/结果字符串的最大长度（"a±bi" 全形态） */
#define FMT_RESULT_LINE_MAX 15U     /* "=" 结果行最多放 15 个字符（超出取尾部） */
#define FMT_IMAG_TMP_SIZE   20U     /* 虚部分量临时缓冲（含符号/小数点/'\0'） */
#define FMT_ZERO_EPS        0.5f    /* 半个显示单位：以下归零（防 "-0.00" 噪声） */
#define FMT_RANGE_LIMIT     1.0e9f  /* ±量程：超出（含 NaN/Inf）显示 "Error" */
#define LCD_COLS            16U     /* 1602 每行 16 字符 */
#define APP_EXPR_MAX        64U     /* 表达式缓冲大小（可输入 63 字符 + '\0'） */
#define APP_EVAL_TIMEOUT_MS 200U    /* 求值兜底超时：算这么久还没回就放弃（正常 <30ms） */
#define APP_HWM_PERIOD_MS  10000U   /* 栈余量采样周期（调试用，见下） */

/* ---------------------------- 栈余量监测（调试用） ---------------------------- */
/* 每 10s 采一轮各任务的剩余栈（字节，由 osThreadGetStackSpace 从栈高水位算出）。
 * 上板时喂最坏输入（8 层嵌套 / 63 字符长式子）后用调试器按名字读取：
 *   g_stack_hwm = [0]computeTask [1]controllerTask [2]lcdTask [3]KeyTask [4]heartbeat
 * 判据：computeTask 剩余 ≥ ~400B（engine 的上界公式见 CALC_MAX_DEPTH 注释）。 */
volatile uint32_t g_stack_hwm[5];   /* 故意非 static：让调试器按符号名直接查 */

extern osThreadId_t calculatorTaskHandle;   /* main.c 里的任务句柄 */
extern osThreadId_t controllerTaskHandle;
extern osThreadId_t lcdTaskHandle;
extern osThreadId_t KeyTaskHandle;
extern osThreadId_t defaultTaskHandle;

/* FMT 显示精度档位：值 = 小数位数。s_frac_digits 用它，不再出现裸 2/4/6 */
typedef enum { FMT_2 = 2, FMT_4 = 4, FMT_6 = 6 } fmt_digits_t;

/* --------------------------- 任务间通信（RTOS 对象） -------------------------- */
/* 【FreeRTOS 学习点】任务之间不再靠"共享变量 + 关中断互斥"，而是用 RTOS 对象传数据：
 *   keyTask     --消息队列 keyQ-->  controllerTask（按键事件）
 *   controller  --消息队列 lcdQ-->  lcdTask（显示帧；满时丢最旧、最新帧胜出）
 *   controller  --消息队列 reqQ-->  computeTask（求值请求，带完整快照）
 *   computeTask --消息队列 resQ-->  controllerTask（求值结果）
 * 队列自带临界区保护；本文件不再需要任何 PRIMASK 关中断。
 * 每个 static 变量仍然只有一个写者任务——这条纪律在多任务里永远要守住。 */

/* 显示帧：controller 发给 lcdTask 的一条"整屏内容"（定长，可直接进队列） */
typedef struct
{
  char    line1[16];            /* 定长 16，不含结束符 */
  char    line2[16];
  uint8_t cur_on;               /* 编辑光标：1=显示（硬件光标，lcd1602_write_frame） */
  uint8_t cur_row;              /* 光标行 0/1 */
  uint8_t cur_col;              /* 光标列 0~15 */
} lcd_frame_t;

/* 按键事件 */
typedef struct
{
  uint8_t key;                  /* 物理键号 0~29 */
} key_evt_t;

#define APP_KEY_Q_DEPTH 16U
#define APP_LCD_Q_DEPTH 4U

static StaticQueue_t      s_key_q_cb;
static uint32_t           s_key_q_mem[(APP_KEY_Q_DEPTH * sizeof(key_evt_t) + 3U) / 4U];
static osMessageQueueId_t s_key_q;
static const osMessageQueueAttr_t s_key_q_attr =
{
  .name    = "keyQ",
  .cb_mem  = &s_key_q_cb,
  .cb_size = sizeof(s_key_q_cb),
  .mq_mem  = &s_key_q_mem[0],
  .mq_size = sizeof(s_key_q_mem),
};

static StaticQueue_t      s_lcd_q_cb;
static uint32_t           s_lcd_q_mem[(APP_LCD_Q_DEPTH * sizeof(lcd_frame_t) + 3U) / 4U];
static osMessageQueueId_t s_lcd_q;
static const osMessageQueueAttr_t s_lcd_q_attr =
{
  .name    = "lcdQ",
  .cb_mem  = &s_lcd_q_cb,
  .cb_size = sizeof(s_lcd_q_cb),
  .mq_mem  = &s_lcd_q_mem[0],
  .mq_size = sizeof(s_lcd_q_mem),
};

/* 求值请求/结果：controller → computeTask → controller（求值解耦的核心）。
 * 两条消息都带完整快照——computeTask 不读任何 UI 状态、controller 不跑来算数，
 * 两边唯一的耦合就是这两个队列。 */
typedef struct
{
  char    expr[APP_EXPR_MAX];   /* 表达式快照（按下 = 瞬间的完整式子） */
  uint8_t serial;               /* 请求序号（结果配对用；0 保留给"空闲"） */
  uint8_t angle_rad;            /* 角度制快照 */
  uint8_t cmplx;                /* 复数模式快照 */
  uint8_t frac;                 /* FMT 精度档位快照 */
} calc_req_t;

typedef struct
{
  char          text[FMT_VALUE_MAX];  /* 已格式化好的结果串（CALC_OK 时有效） */
  uint8_t       serial;               /* 对应请求的序号 */
  uint8_t       len;                  /* text 的长度 */
  calc_status_t status;               /* 求值状态 */
} calc_res_t;

#define APP_EVAL_Q_DEPTH 2U

static StaticQueue_t      s_req_q_cb;
static uint32_t           s_req_q_mem[(APP_EVAL_Q_DEPTH * sizeof(calc_req_t) + 3U) / 4U];
static osMessageQueueId_t s_req_q;
static const osMessageQueueAttr_t s_req_q_attr =
{
  .name    = "reqQ",
  .cb_mem  = &s_req_q_cb,
  .cb_size = sizeof(s_req_q_cb),
  .mq_mem  = &s_req_q_mem[0],
  .mq_size = sizeof(s_req_q_mem),
};

static StaticQueue_t      s_res_q_cb;
static uint32_t           s_res_q_mem[(APP_EVAL_Q_DEPTH * sizeof(calc_res_t) + 3U) / 4U];
static osMessageQueueId_t s_res_q;
static const osMessageQueueAttr_t s_res_q_attr =
{
  .name    = "resQ",
  .cb_mem  = &s_res_q_cb,
  .cb_size = sizeof(s_res_q_cb),
  .mq_mem  = &s_res_q_mem[0],
  .mq_size = sizeof(s_res_q_mem),
};

/* 一次性创建全部 RTOS 对象（全部静态存储，不占那 1KB 的堆）。
 * 由 calculator_app_init 在调度器启动之前调用——静态创建不依赖调度器。 */
static void app_rtos_init(void)
{
  s_key_q = osMessageQueueNew(APP_KEY_Q_DEPTH, sizeof(key_evt_t), &s_key_q_attr);
  s_lcd_q = osMessageQueueNew(APP_LCD_Q_DEPTH, sizeof(lcd_frame_t), &s_lcd_q_attr);
  s_req_q = osMessageQueueNew(APP_EVAL_Q_DEPTH, sizeof(calc_req_t), &s_req_q_attr);
  s_res_q = osMessageQueueNew(APP_EVAL_Q_DEPTH, sizeof(calc_res_t), &s_res_q_attr);
}

/* 全部 RTOS 对象就绪 ? 1 : 0（init 里校验；失败要停机报错，不能带着坏队列跑） */
static uint8_t app_rtos_ready(void)
{
  return ((s_key_q != NULL) && (s_lcd_q != NULL) &&
          (s_req_q != NULL) && (s_res_q != NULL)) ? 1U : 0U;
}

/* -------------------------------- 小工具 --------------------------------- */

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

/* 发一帧到显示队列。与上一帧完全相同就跳过（去重，省一次 LCD 慢写）；
 * 队列满时先丢最旧的一帧——"最新帧胜出"，屏幕最终必与最新状态一致。 */
static void app_display_post(const lcd_frame_t *f)
{
  static lcd_frame_t s_last;       /* 上一帧（本模块私有） */
  static uint8_t     s_last_valid;

  if ((s_last_valid != 0U) && (memcmp(&s_last, f, sizeof(*f)) == 0))
  {
    return;                        /* 内容没变：不必重绘 */
  }
  s_last = *f;
  s_last_valid = 1U;

  if (osMessageQueuePut(s_lcd_q, f, 0U, 0U) != osOK)
  {
    lcd_frame_t drop;

    (void)osMessageQueueGet(s_lcd_q, &drop, NULL, 0U);   /* 丢最旧 */
    (void)osMessageQueuePut(s_lcd_q, f, 0U, 0U);
  }
}

/* 更新显示（带编辑光标参数）：组帧 → 入队 */
static void app_display_ex(const char *l1, const char *l2,
                           uint8_t cur_on, uint8_t cur_row, uint8_t cur_col)
{
  lcd_frame_t f;

  app_fill16(f.line1, l1);
  app_fill16(f.line2, l2);
  f.cur_on  = cur_on;
  f.cur_row = cur_row;
  f.cur_col = cur_col;

  app_display_post(&f);
}

/* 不带光标版（菜单 / 结果 / 错误等"特殊视图"用） */
static void app_display(const char *l1, const char *l2)
{
  app_display_ex(l1, l2, 0U, 0U, 0U);
}

/* 直接写屏：只在 calculator_app_init（调度器启动前、无并发）里用；
 * 调度器启动之后，LCD 只允许 lcdTask 经 lcdQ 碰。 */
static void app_lcd_direct(const char *l1, const char *l2)
{
  char t1[16], t2[16];

  app_fill16(t1, l1);
  app_fill16(t2, l2);
  lcd1602_write_frame(t1, t2, 0U, 0U, 0U);
}

/* ------------------------------- D4：按键事件 ----------------------------- */

/* 滤波状态：只被 keyTask 读写（init 里清零），不跨任务共享、无需加锁。 */
static touch_filter_t s_filter;

/* 按键事件入队（keyTask 调用；队满丢弃——人手连按不会堆满 16 个） */
static void app_key_push(uint8_t ev)
{
  key_evt_t e;

  e.key = ev;
  (void)osMessageQueuePut(s_key_q, &e, 0U, 0U);
}

/* 按键事件出队（controllerTask 调用）；*ev 取到键号返回 1，队空返回 0 */
static uint8_t app_key_pop(uint8_t *ev)
{
  key_evt_t e;

  if (osMessageQueueGet(s_key_q, &e, NULL, 0U) != osOK)
  {
    return 0U;                     /* 队空（或队列不可用） */
  }
  *ev = e.key;
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
 * 6→ln(、1→sin(、2→cos(、3→tan(、÷→sqrt(、*→x^y 幂运算；8→∠、9→i 仅 COMPLX 模式。
 * FMT(T28) = 显示精度设置（2 / 4 / 6 位小数；FMT 循环切换、OK 退出）；
 * 方向键 = 编辑光标移动；OK(T4) = MODE / FMT 界面的确认退出键（其它场景按下无动作）。 */
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

static char    s_expr[APP_EXPR_MAX]; /* 输入的表达式（C 字符串，容量见顶部 APP_EXPR_MAX） */
static uint8_t s_expr_len;
static uint8_t s_just_eval;      /* 刚按过 = ：下一次按数字要开新算式 */
static uint8_t s_shift_active;   /* SHIFT 修饰态：下一次按键消费掉它 */
static uint8_t s_cmplx_mode;     /* 计算模式：0=COMP，1=CMPLX（MODE 菜单里切换） */
static uint8_t s_angle_rad;      /* 角度制：0=DEG（角度），1=RAD（弧度）；全局设置 */
static uint8_t s_mode_menu;      /* 1 = 正在 MODE 设置菜单里 */
static uint8_t s_fmt_menu;       /* 1 = 正在 FMT 精度设置里（模态：OK 退出） */
static fmt_digits_t s_frac_digits = FMT_2;  /* FMT：结果显示的小数位数（2 / 4 / 6） */
static uint8_t s_cursor;            /* 编辑光标：表达式内索引（0 ~ s_expr_len，≤63） */

/* 求值在途状态（只有 controllerTask 访问，无需加锁）：
 * 求值已异步化——按 = 只发请求，结果由 app_eval_poll 每周期取回。 */
static uint8_t  s_req_seq;          /* 请求序号发生器（0 保留给"空闲"，不发给计算任务） */
static uint8_t  s_pending_seq;      /* 在途请求序号；0 = 没有在途 */
static uint32_t s_edit_seq;         /* 表达式"内容版本号"：内容变化才 ++（光标移动不算） */
static uint32_t s_pending_edit;     /* 提交请求瞬间的版本号快照 */
static uint32_t s_pending_tick;     /* 提交时刻（超时兜底用） */

/* 撤销栈（BACK 键）：以"式子"为粒度——只在这些时刻记快照：
 *   ① 从"空 / 刚求值(结果)"状态开始输入第一个字符之前（记下式子起点的状态）；
 *   ② 按 '=' 求值前（记下整个式子）；
 *   ③ 按 AC 清空前（记下被清的式子）。
 * 输入中途的字符不产生快照——所以 BACK 是"整个式子一次回滚"，
 * 不是退格删字符（删单个字符请用 DEL 键）。
 * 满栈丢最旧的，保留最近 APP_UNDO_DEPTH 个式子状态。 */
#define APP_UNDO_DEPTH 8U
static char    s_undo[APP_UNDO_DEPTH][APP_EXPR_MAX];
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
  s_edit_seq++;                    /* 表达式内容变了（求值在途判定用） */
  return 1U;
}

static void app_eval_submit(void);     /* 前置声明：app_calc_input 的 '=' 分支要调用它 */

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
    case '*': return "^";                                 /* x^y 幂运算 */
    case '9': return (s_cmplx_mode != 0U) ? "i" : NULL;   /* 虚数单位：仅 COMPLX 模式 */
    case '8': return (s_cmplx_mode != 0U) ? "@" : NULL;   /* 极坐标 a@θ：仅 COMPLX 模式 */
    default:  return NULL;
  }
}

/* ---------------- 编辑视图的显示窗口（两行 × 16 的滚动视图） ---------------- */

/* 32 字符显示窗口的起点：跟随光标，保证光标始终可见。
 * 光标 < 32（在前两屏内）→ 窗口从 0 起；否则窗口滚动到"光标钉在第二行末"。
 * 纯函数（无状态），光标一动窗口自然跟着滚；窗口起点 ≤ 32，索引不越界。 */
static uint8_t app_win_start(void)
{
  return (s_cursor >= 32U) ? (uint8_t)(s_cursor - 31U) : 0U;
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

/* FMT：切到下一档显示精度（2 → 4 → 6 → 2）并刷新 "FIX:n [OK]" 提示。
 * 第二行强制显示提示（表达式超过 16 字符时暂盖住第二行，退出后恢复编辑视图），
 * 保证 FMT 模态里"看得见"当前档位与退出方式。 */
static void app_fmt_step(void)
{
  char    l1[16];
  char    l2[16];
  char    t[12];
  uint8_t win = app_win_start();
  uint8_t off;

  s_frac_digits = (s_frac_digits == FMT_2) ? FMT_4 : ((s_frac_digits == FMT_4) ? FMT_6 : FMT_2);
  t[0] = 'F'; t[1] = 'I'; t[2] = 'X'; t[3] = ':'; t[4] = ' ';
  t[5] = (char)('0' + (int)s_frac_digits);
  t[6] = ' '; t[7] = '['; t[8] = 'O'; t[9] = 'K'; t[10] = ']';
  t[11] = '\0';

  for (uint8_t k = 0U; k < 16U; k++)
  {
    uint8_t idx = (uint8_t)(win + k);

    l1[k] = (idx < s_expr_len) ? s_expr[idx] : ' ';
  }
  if (s_expr_len == 0U)
  {
    l1[0] = '0';                       /* 与编辑视图一致：空表达式显示 "0" */
  }
  app_fill16(l2, t);

  off = (uint8_t)(s_cursor - win);
  app_display_ex(l1, l2, 1U,
                 (off >= 16U) ? 1U : 0U,
                 (uint8_t)(off % 16U));
}

/* 编辑视图显示：表达式经 32 字符窗口分两行铺满屏（每行 16），
 * 窗口跟随光标（见 app_win_start）——长式子下满屏滚动、光标钉在可见处。
 * 第二行空着（len ≤ 16）时显示调用者给的提示文字（彩蛋 / EXE to evaluate 等）。 */
static void app_calc_show_hint(const char *hint)
{
  char    l1[16];
  char    l2[16];
  uint8_t win = app_win_start();
  uint8_t off;

  for (uint8_t k = 0U; k < 16U; k++)
  {
    uint8_t i1 = (uint8_t)(win + k);
    uint8_t i2 = (uint8_t)(win + 16U + k);

    l1[k] = (i1 < s_expr_len) ? s_expr[i1] : ' ';
    l2[k] = (i2 < s_expr_len) ? s_expr[i2] : ' ';
  }

  if (s_expr_len <= 16U)
  {
    app_fill16(l2, hint);            /* 第二行空着 → 放提示文字 */
  }
  if (s_expr_len == 0U)
  {
    l1[0] = '0';                     /* 空闲主界面第一行显示 "0" */
  }

  off = (uint8_t)(s_cursor - win);
  app_display_ex(l1, l2, 1U,
                 (off >= 16U) ? 1U : 0U,
                 (uint8_t)(off % 16U));
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
  "Brewing :3",
  "Peliccan town!",
  "Teyvat waits!",
  "Euler is here",
  "Sanity dropping",
  "Blood moon!",
  "Hi,husters!",
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
 *   - 满 APP_EXPR_MAX-1 字符后拒绝（返回 0），调用者据此提示 "! buf full"。 */
static uint8_t app_calc_put(uint8_t c)
{
  if ((s_expr_len == 0U) || (s_just_eval != 0U))
  {
    app_undo_push();
  }

  if (s_just_eval != 0U)
  {
    if (!((c == '+') || (c == '-') || (c == '*') || (c == '/') || (c == 'E') || (c == '^')))
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
    s_edit_seq++;                    /* 表达式内容变了（求值在途判定用） */
    return 1U;
  }
  return 0U;                         /* 缓冲满：拒绝本次输入 */
}

/* 追加一串字符（函数记号 "sin(" 等），整串算一个输入动作 */
static void app_calc_put_str(const char *s)
{
  for (uint8_t i = 0U; s[i] != '\0'; i++)
  {
    (void)app_calc_put((uint8_t)s[i]);
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

  /* 0.2) FMT 精度设置模态：FMT 循环切换 2 → 4 → 6 → 2，按 OK 确认退出；
   *      其它键一律无反应（与 MODE 菜单同款"只能 OK 退出"规则）。 */
  if (s_fmt_menu != 0U)
  {
    if (c == APP_CH_FMT)
    {
      app_fmt_step();
    }
    else if (c == APP_CH_OK)
    {
      s_fmt_menu = 0U;
      app_calc_show();
    }
    return;
  }

  /* 0.5) OK：MODE / FMT 界面的确认键；都不在时按下不产生任何动作。
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

  /* 3.5) FMT：进入精度设置模态（模态内 FMT 循环切换、OK 退出、其它键无反应） */
  if (c == APP_CH_FMT)
  {
    s_fmt_menu = 1U;
    app_fmt_step();
    return;
  }

  /* 4) AC 全清（清之前记撤销点，BACK 可恢复到清空前） */
  if (c == APP_CH_AC)
  {
    if (s_expr_len > 0U)
    {
      app_undo_push();
      s_edit_seq++;                    /* 清空也是内容变化 */
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
      s_edit_seq++;                  /* 表达式内容变了 */
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

  /* 7) = 求值：这里只"发请求"（computeTask 负责算，结果经 resQ 回来、
   *    app_eval_poll 负责应用）——控制任务不再被计算挡路 */
  if (c == APP_CH_EQ)
  {
    app_eval_submit();
    return;
  }

  /* 8) 追加字符：put 内部负责"新输入段记撤销点"与 just_eval 状态机；
   *    缓冲区满会拒绝（返回 0）——明确提示，不再静默丢弃 */
  if (app_calc_put(c) == 0U)
  {
    app_display(s_expr, "! buf full");
    return;
  }

  /* 9) 刷新显示 */
  app_calc_show();
}

/* 把 float 变成短字符串：整数部分 + frac 位小数（四舍五入；frac 取 FMT_2/FMT_4/FMT_6）。
 * 例（frac=2）：7 → "7.00"；-2.5 → "-2.50"；超范围/NaN → "Error" */
static void app_format_float(float v, char *out, uint8_t out_size, fmt_digits_t frac)
{
  char     tmp[FMT_INT_TMP_SIZE];
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
  if ((v > -(FMT_ZERO_EPS / (float)scale)) && (v < (FMT_ZERO_EPS / (float)scale)))
  {
    v = 0.0f;
  }

  if (!(v > -FMT_RANGE_LIMIT && v < FMT_RANGE_LIMIT))   /* NaN 和无穷也走这里 */
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
  } while (ip > 0U && n < FMT_INT_DIGITS_MAX);

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
 * frac：显示精度快照（由消息带入，不读全局——本函数在 computeTask 里跑）。
 * 近似阈值 = 半个显示单位，和 app_format_float 的归零口径一致。 */
static uint8_t app_format_value(const calc_complex_t *v, char *r, uint8_t size, fmt_digits_t frac)
{
  float   eps = FMT_ZERO_EPS;
  uint8_t j = 0U;
  uint8_t show_im;

  for (uint8_t i = 0U; i < (uint8_t)frac; i++)
  {
    eps /= 10.0f;                      /* 0.5 / 10^frac */
  }
  show_im = !((v->imag > -eps) && (v->imag < eps));  /* 虚部≈0 → 按实数显示 */

  /* 实部：仅当它有存在感时输出 */
  if ((show_im == 0U) || !((v->real > -eps) && (v->real < eps)))
  {
    app_format_float(v->real, r, size, frac);
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
    char  t[FMT_IMAG_TMP_SIZE];
    float ai = v->imag;
    char  sc = '+';

    if (ai < 0.0f)
    {
      sc = '-';
      ai = -ai;
    }
    app_format_float(ai, t, (uint8_t)sizeof(t), frac);
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

/* 求值状态码 → LCD 错误文案（第一行显示原表达式，第二行显示这里的文案，均 ≤16 字符） */
static const char *app_err_text(calc_status_t st)
{
  switch (st)
  {
    case CALC_DIV_ZERO: return "! div by 0";
    case CALC_DOMAIN:   return "! domain";
    case CALC_BRACKET:  return "! bracket";
    case CALC_BAD_OP:   return "! bad op";
    case CALC_DEPTH:    return "! too deep";
    case CALC_SYNTAX:
    default:            return "! syntax";
  }
}

/* '=' 的入口：组一份求值快照、投给 computeTask；不计算、不阻塞。
 * 在途未回（s_pending_seq != 0）时忽略本次——求值只需毫秒级，基本撞不上。 */
static void app_eval_submit(void)
{
  calc_req_t req;

  if (s_expr_len == 0U)
  {
    return;                              /* 空表达式：当没按过 */
  }
  if (s_pending_seq != 0U)
  {
    return;                              /* 上一单还在算：忽略 */
  }

  for (uint8_t i = 0U; i <= s_expr_len; i++)
  {
    req.expr[i] = s_expr[i];             /* 含结尾 '\0' */
  }

  s_req_seq++;
  if (s_req_seq == 0U)
  {
    s_req_seq = 1U;                      /* 0 保留给"空闲" */
  }
  req.serial    = s_req_seq;
  req.angle_rad = s_angle_rad;
  req.cmplx     = s_cmplx_mode;
  req.frac      = (uint8_t)s_frac_digits;

  if (osMessageQueuePut(s_req_q, &req, 0U, 0U) == osOK)
  {
    s_pending_seq  = req.serial;
    s_pending_edit = s_edit_seq;
    s_pending_tick = osKernelGetTickCount();
  }
}

/* 应用一条配对成功的结果：成功 → 撤销点 + "=结果"显示 + 回填（继续运算）；
 * 失败 → 按状态码显示错误文案（app_err_text）。只在 controllerTask 里跑。 */
static void app_eval_apply(const calc_res_t *res)
{
  char    line1[17];
  uint8_t rlen;
  uint8_t fill;

  if (res->status != CALC_OK)
  {
    app_display(s_expr, app_err_text(res->status));   /* 细分文案：! bracket / ! bad op / … */
    return;
  }

  rlen = res->len;
  app_undo_push();      /* 记撤销点：按 BACK 可回到求值前的表达式 */

  /* 显示行："=" + 结果（例 "=7.00"、"=-5.00+10.00i"）；
   * 结果超过 15 字符时显示尾部（保住虚部信息） */
  if (rlen <= FMT_RESULT_LINE_MAX)
  {
    line1[0] = '=';
    for (uint8_t i = 0U; i < rlen; i++)
    {
      line1[1U + i] = res->text[i];
    }
    line1[1U + rlen] = '\0';
  }
  else
  {
    uint8_t off = (uint8_t)(rlen - FMT_RESULT_LINE_MAX);

    for (uint8_t i = 0U; i < FMT_RESULT_LINE_MAX; i++)
    {
      line1[i] = res->text[off + i];
    }
    line1[FMT_RESULT_LINE_MAX] = '\0';
  }

  /* 回填：s_expr 变成结果字符串——复数带隐式乘法记号（"10.00i"），
   * 引擎可直接再解析；超长结果按缓冲上限截断。
   * just_eval=1 → 下一次按数字则开新算式（卡西欧式行为） */
  fill = (rlen > (uint8_t)(sizeof(s_expr) - 1U)) ? (uint8_t)(sizeof(s_expr) - 1U) : rlen;
  for (uint8_t i = 0U; i < fill; i++)
  {
    s_expr[i] = res->text[i];
  }
  s_expr[fill] = '\0';
  s_expr_len = fill;
  s_cursor = fill;      /* 结果显示/回填后，光标回到末尾 */
  s_just_eval = 1U;
  s_edit_seq++;         /* 回填也是内容变化 */

  app_display(line1, "BACK=undo AC=clr");
}

/* 每周期调用：取回结果并对号入座；绝不阻塞（0 超时）。
 * 双判据：①序号匹配（正是在途的那单）②式子没被改过（s_edit_seq 未变）——
 * 求值期间用户在拼新式子，就把旧结果静静地丢掉，别覆盖用户正在打的字。 */
static void app_eval_poll(void)
{
  calc_res_t res;

  if (s_pending_seq == 0U)
  {
    return;
  }

  while (osMessageQueueGet(s_res_q, &res, NULL, 0U) == osOK)
  {
    if (res.serial == s_pending_seq)
    {
      s_pending_seq = 0U;                /* 这一单结清 */
      if (s_edit_seq == s_pending_edit)
      {
        app_eval_apply(&res);
      }
      break;                             /* 陈旧/迟到的结果不管 */
    }
  }

  /* 兜底：computeTask 假死（或消息丢了）也不会把界面永久锁在"在途" */
  if ((s_pending_seq != 0U) &&
      ((osKernelGetTickCount() - s_pending_tick) > APP_EVAL_TIMEOUT_MS))
  {
    s_pending_seq = 0U;
    app_display(s_expr, "! timeout");
  }
}

/* ------------------------------ 开机硬件自检 ----------------------------- */

/* 判据：连续读到"30 键全按下"（0x3FFFFFFF）是不可能的物理状态，
 * 它意味着 SDO 恒为高——芯片无响应 / 接线故障的典型特征。
 * 注意：全 0 是正常的"没按键"静默态，不能当故障！（否则每次开机都误报） */
#define APP_SELFTEST_SAMPLES 5U
#define APP_SELFTEST_GAP_MS  20U
#define TTP229_ALL_KEYS      0x3FFFFFFFU

static uint8_t s_hold_boot_page;    /* 1 = 自检异常页，保持到首次按键 */

/* 返回 1 = 通过；0 = 异常。调度器启动前调用：用 HAL_Delay（TIM4 时基已工作）。 */
static uint8_t app_touch_selftest(void)
{
  for (uint8_t i = 0U; i < APP_SELFTEST_SAMPLES; i++)
  {
    if (ttp229_read_physical() != TTP229_ALL_KEYS)
    {
      return 1U;                   /* 读到任意"正常"值（含全 0）即通过 */
    }
    if ((i + 1U) < APP_SELFTEST_SAMPLES)
    {
      HAL_Delay(APP_SELFTEST_GAP_MS);
    }
  }
  return 0U;                       /* 连续 5 次全 1 → 判定故障 */
}

/* ============================== 6 个契约函数 ============================== */

/* 在调度器启动之前被调用：只能做"初始化和硬件自检"，不能用 osDelay！
 * LCD 在这里直接写屏（此时无并发，还不用走显示队列）。 */
void calculator_app_init(void)
{
  lcd1602_init();                          /* LCD 初始化（内部用 HAL_Delay，没问题） */
  touch_filter_init(&s_filter);
  CDC_RxInit();                            /* D3：建 USB 接收流缓冲（调度器启动前，静态创建） */
  app_rtos_init();                         /* 建按键/显示队列（静态创建，不依赖调度器） */

  if (app_rtos_ready() == 0U)
  {
    app_lcd_direct("RTOS ERR", "check queues");
    for (;;) { }                           /* 对象没建起来：停机报错，别带着坏队列跑 */
  }

  if (app_touch_selftest() != 0U)
  {
    app_lcd_direct("MY  MINI  CASIO", "SOEI  2607  LYH");   /* 自检通过：欢迎页 */
  }
  else
  {
    app_lcd_direct("TOUCH ERR", "check TTP229 wire");       /* 连续全 1 = 故障特征 */
    s_hold_boot_page = 1U;                                  /* 错误页保持到首次按键 */
  }
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

/* 显示任务：全工程唯一允许调用 lcd1602_* 的任务。
 * 阻塞等显示帧（事件驱动，不再 100ms 轮询）；等到一帧就慢写整屏。 */
void calculator_lcd_task(void)
{
  lcd_frame_t f;

  for (;;)
  {
    if (osMessageQueueGet(s_lcd_q, &f, NULL, osWaitForever) == osOK)
    {
      /* 带硬件光标（编辑视图显示，方向键移动即这里跟进） */
      lcd1602_write_frame(f.line1, f.line2, f.cur_on, f.cur_row, f.cur_col);
    }
  }
}

/* 控制任务：D3/D4 在这里汇聚"串口数据 + 按键事件" */
void calculator_controller_task(void)
{
  uint8_t  ev;
  uint32_t t0 = osKernelGetTickCount();   /* 开机时刻 ≈ 本任务第一次运行 */
  uint8_t  intro_done = 0U;
  uint32_t t_hwm = 0U;                    /* 上次栈余量采样时刻 */

  for (;;)
  {
    app_usb_poll();                 /* D3 */

    while (app_key_pop(&ev))        /* D4/D5：按键事件 → 输入 */
    {
      if (s_hold_boot_page != 0U)
      {
        s_hold_boot_page = 0U;      /* 自检异常页：首次按键后放行主界面 */
        app_calc_show();
      }
      uint8_t ch = s_key_map[ev];
      if (ch != 0xFFU)              /* 0xFF = 该键未映射，忽略 */
      {
        app_calc_input(ch);
      }
    }

    /* 欢迎页展示 APP_SPLASH_MS 后自动进入主界面（"0" + 随机彩蛋）。
     * 自检异常页（s_hold_boot_page）期间不自动切，保持错误提示可见。 */
    if ((s_hold_boot_page == 0U) && (intro_done == 0U) &&
        ((osKernelGetTickCount() - t0) >= APP_SPLASH_MS))
    {
      intro_done = 1U;
      app_calc_show();
    }

    app_eval_poll();                /* 收求值结果 / 超时兜底（非阻塞） */

    /* 栈余量采样（调试用，见 g_stack_hwm 说明） */
    if ((osKernelGetTickCount() - t_hwm) >= APP_HWM_PERIOD_MS)
    {
      t_hwm = osKernelGetTickCount();
      g_stack_hwm[0] = osThreadGetStackSpace(calculatorTaskHandle);
      g_stack_hwm[1] = osThreadGetStackSpace(controllerTaskHandle);
      g_stack_hwm[2] = osThreadGetStackSpace(lcdTaskHandle);
      g_stack_hwm[3] = osThreadGetStackSpace(KeyTaskHandle);
      g_stack_hwm[4] = osThreadGetStackSpace(defaultTaskHandle);
    }

    osDelay(APP_CTRL_PERIOD_MS);
  }
}

/* 计算任务：阻塞等求值请求 → 求值 + 格式化（纯计算，不碰任何 UI 状态）→ 回结果。
 * 递归下降解析的重活全在这里跑；栈 3328B 就是留给它的（上界公式见 engine 的
 * CALC_MAX_DEPTH 注释）。格式化用请求里的 frac 快照，与 UI 状态完全解耦。 */
void calculator_compute_task(void)
{
  calc_req_t     req;
  calc_res_t     res;
  calc_complex_t v;

  for (;;)
  {
    if (osMessageQueueGet(s_req_q, &req, NULL, osWaitForever) != osOK)
    {
      continue;                    /* 系统错误：下轮再来 */
    }

    res.serial = req.serial;
    res.status = calculator_evaluate(req.expr,
                                     (req.angle_rad != 0U) ? CALC_ANGLE_RAD : CALC_ANGLE_DEG,
                                     req.cmplx,
                                     (calc_complex_t){0},
                                     &v);
    if (res.status == CALC_OK)
    {
      res.len = app_format_value(&v, res.text, (uint8_t)sizeof(res.text), (fmt_digits_t)req.frac);
    }
    else
    {
      res.len = 0U;
      res.text[0] = '\0';
    }

    (void)osMessageQueuePut(s_res_q, &res, 0U, 0U);
  }
}