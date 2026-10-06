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

extern USBD_HandleTypeDef hUsbDeviceFS;   /* 定义在 usb_device.c */

/* ------------------------------- 可调参数 -------------------------------- */
#define APP_KEY_PERIOD_MS    10U    /* 触摸扫描周期：库约定 10ms 一次 */
#define APP_CTRL_PERIOD_MS   10U    /* 控制任务周期 */
#define APP_LCD_PERIOD_MS   100U    /* 刷新周期：LCD 库内部用 HAL_Delay 逐字节写，别设太小 */
#define APP_HB_PERIOD_MS    500U    /* 心跳灯：每 500ms 翻转一次 = 每秒闪 1 下 */

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

/* ------------------------------- D4：按键事件与试触页 ---------------------- */
#define APP_KEY_EVENT_RELEASE  0xFEU   /* "松手"事件（只给试触页用） */

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

/* 试触页：把键号显示到屏幕（D5 换成计算器输入后，这个函数退休） */
static void app_show_key(uint8_t ev)
{
  char t[17];

  if (ev == APP_KEY_EVENT_RELEASE)
  {
    app_display("KEY: ---", "touch trial page");
    return;
  }

  t[0] = 'K'; t[1] = 'E'; t[2] = 'Y'; t[3] = ':'; t[4] = ' ';
  t[5] = 'T';
  t[6] = (char)('0' + (ev / 10U));
  t[7] = (char)('0' + (ev % 10U));
  t[8] = '\0';
  app_display(t, "touch trial page");
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

/* 按键任务：D4 再填内容，现在先空转 */
void calculator_key_task(void)
{
  uint32_t prev = 0U;

  for (;;)
  {
    uint32_t raw = ttp229_read_physical();                 /* 库：读 30 位位图 */
    uint32_t key = touch_filter_update(&s_filter, raw);    /* 你的：滤波 */

    if (key != prev)
    {
      app_key_push((key == 0U) ? APP_KEY_EVENT_RELEASE : (uint8_t)__builtin_ctz(key));
      prev = key;
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
  uint8_t ev;

  for (;;)
  {
    app_usb_poll();                 /* D3 */

    while (app_key_pop(&ev))        /* D4：按键事件 */
    {
      app_show_key(ev);
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