// 一个极小的「假 Arduino.h」，只为了让 vibepet_proto.h 能在电脑上编译。
//
// 真机上这份头文件来自 Arduino 核心，里面是 uint8_t/bool、pgm_read_* 之类的
// 基础定义。离线测试时用这个替身顶上，于是**协议解析内核不用插硬件就能验证**
// （见 tools/test_proto.c，跑法写在那个文件开头）。
#ifndef ARDUINO_H_SHIM
#define ARDUINO_H_SHIM

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// AVR 上这些宏是「从 Flash 读数据」；电脑上没有 Flash 与内存之分，直接读即可。
#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))

#endif  // ARDUINO_H_SHIM
