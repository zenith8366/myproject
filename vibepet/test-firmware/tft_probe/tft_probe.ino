/*
 * VibePet 临时探针固件 v5 —— 根因验证
 *
 * 这是之前「卡死」的 v3 场景的精确复刻：含 pinMode 块、不含任何显式 SPI.begin()，
 * 完全依赖 TFT_eSPI 内部的 spi.begin()。
 *
 * 修复前（TFT_eSPI_ESP32.h 把 TFT_MISO 改成 TFT_MOSI）：
 *   spi.begin(4, 6, 6, -1) → MISO/MOSI 撞脚 → SPI 发不出 → 卡死在 tft.init()
 *
 * 修复后（核心 3.x 下 TFT_MISO 保持 -1）：
 *   spi.begin(4, -1, 6, -1) → 正常
 *
 * 若本固件能跑到底并让屏幕变红，根因即被端到端证实。
 *
 * ⚠️ 诊断用临时文件，验证完即删。
 */

#include <TFT_eSPI.h>

// 与 VibePet.ino 一致的引脚定义（设计文档 §4.7.1）
#define PIN_BTN_APPROVE  1
#define PIN_BTN_DENY     10
#define PIN_BUZZER       3
#define PIN_LED_STATUS   2

TFT_eSPI tft = TFT_eSPI();

void mark(const char* s) {
  Serial.print("[probe] ");
  Serial.println(s);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(2200);

  Serial.println();
  mark("=== probe v5 (根因验证) ===");

  pinMode(PIN_BTN_APPROVE, INPUT_PULLUP);
  pinMode(PIN_BTN_DENY,    INPUT_PULLUP);
  pinMode(PIN_BUZZER,      OUTPUT);
  pinMode(PIN_LED_STATUS,  OUTPUT);
  digitalWrite(PIN_BUZZER,     LOW);
  digitalWrite(PIN_LED_STATUS, LOW);
  mark("P: 引脚块完成（与 VibePet 一致）");

  mark("T1: 即将调用 tft.init()（不做任何显式 SPI.begin）");
  tft.init();
  mark("T2: tft.init() 返回了  <<< 修复成功的标志");

  tft.setRotation(1);
  tft.fillScreen(TFT_RED);
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(20, 50);
  tft.print("VibePet");
  mark("T3: 绘制完成 —— 屏幕应为红底白字 VibePet");

  mark("=== 全部通过：根因已修复 ===");
}

void loop() {
  delay(2000);
  mark("loop alive");
}
