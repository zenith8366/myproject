/*
 * VibePet 临时探针固件 2b —— 判定 DC/RES 实际接在哪个 GPIO
 *
 * 已知：
 *   Adafruit 的 tfttest_esp32c3 代码在 9/19「大成功」时用的是 DC=1 / RES=2
 *   现在按设计文档接 DC=8 / RES=5 —— 屏幕全白无反应（Adafruit 与 TFT_eSPI 都白）
 *
 * 本版把 DC/RES 换回 9/19 那份成功配置（DC=1 / RES=2），其余不变：
 *   屏幕开始四色循环 = 实际接线就是 GPIO1/GPIO2（设计文档的 8/5 没接上）
 *   仍然全白         = 问题不是引脚选择，而在别处（松线 / 屏幕 / 背光）
 *
 * ⚠️ 诊断用临时文件，定位后即删。
 */

#include <Adafruit_ST7735.h>
#include <SPI.h>

// 9/19 那份「tft 大成功」用的引脚
#define TFT_CS    7
#define TFT_DC    1     // ← 改回 1（设计文档写的是 8）
#define TFT_RES   2     // ← 改回 2（设计文档写的是 5）

#define TFT_INIT      INITR_BLACKTAB
#define TFT_ROTATION  1

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RES);

void mark(const char* s) {
  Serial.print("[probe2b] ");
  Serial.println(s);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(2200);

  Serial.println();
  mark("=== probe2b start ===");
  mark("使用 DC=GPIO1, RES=GPIO2（9/19 成功配置）");

  SPI.begin(4, -1, 6, 7);
  tft.initR(TFT_INIT);
  tft.setRotation(TFT_ROTATION);
  mark("initR 完成，进入四色无限循环");
}

void loop() {
  static const uint16_t colors[4] = {ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_WHITE};
  static const char*    names[4]  = {"RED", "GREEN", "BLUE", "WHITE"};
  static int i = 0;

  tft.fillScreen(colors[i]);
  Serial.printf("[probe2b] 当前刷屏色：%s\n", names[i]);
  Serial.flush();

  i = (i + 1) & 3;
  delay(1500);
}
