#ifndef CALCULATOR_ENGINE_H
#define CALCULATOR_ENGINE_H

#include <stdint.h>

typedef enum
{
  CALC_ANGLE_DEG = 0,
  CALC_ANGLE_RAD
} calc_angle_unit_t;

/* 复数类型：分量用 float。
 * 【已评估】不改 double：Debug 构建 Flash 余量有限（已用 >72%），double 会显著
 * 增大 libm 的 pow/atan2/hypot 等函数；显示侧最多 6 位小数且有半显示单位归零
 * （app_format_float 的 FMT_ZERO_EPS），本题运算规模下 float（7 位有效数字）足够。 */
typedef struct
{
  float real;
  float imag;
} calc_complex_t;

typedef enum
{
  CALC_OK = 0,
  CALC_SYNTAX,     /* 词法/格式错误：坏数字（"1E"、"3."）、不认识的记号 */
  CALC_DOMAIN,     /* 定义域 / 模式不允许（COMP 模式 sqrt(-1)、sin(i) 等） */
  CALC_DIV_ZERO,   /* 除以零 / 0 的非正次幂 */
  CALC_BRACKET,    /* 括号不匹配（含函数名后缺 '('、参数表达式后缺 ')'） */
  CALC_BAD_OP,     /* 操作数缺失 / 运算符位置错误（"2+"、"2*"、"2^" 等） */
  CALC_DEPTH       /* 表达式嵌套层数超过上限（见 CALC_MAX_DEPTH） */
} calc_status_t;

calc_status_t calculator_evaluate(const char *expression,
                                  calc_angle_unit_t angle_unit,
                                  uint8_t allow_complex,
                                  calc_complex_t answer,
                                  calc_complex_t *result);

calc_status_t calculator_solve_linear(float a, float b, float *x);

#endif
