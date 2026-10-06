/* ============================================================================
 * calculator_engine.c —— 你自己的表达式引擎（替换 lib/libcalculator_engine.a）
 *
 * 【契约】与 Core/Inc/calculator_engine.h 完全一致：
 *   calc_status_t calculator_evaluate(const char *expression,
 *                                     calc_angle_unit_t angle_unit,
 *                                     uint8_t allow_complex,
 *                                     calc_complex_t answer,   // 按值传入
 *                                     calc_complex_t *result);
 *   calc_status_t calculator_solve_linear(float a, float b, float *x);
 * ========================================================================== */

#include "calculator_engine.h"

/* 内部解析函数：static，名字随便起（外面看不见，不会撞名） */

/* 读一个数字（支持前导正负号、小数点）。p 是"游标"，读完自动前移。 */
static calc_status_t parse_number(const char **p, float *out)
{
  const char *s = *p;
  float sign = 1.0f;
  float v = 0.0f;

  if (*s == '+')
  {
    s++;
  }
  else if (*s == '-')
  {
    sign = -1.0f;
    s++;
  }
  if (*s < '0' || *s > '9')            /* 至少要有 1 位数字 */
  {
    return CALC_SYNTAX;
  }
  while (*s >= '0' && *s <= '9')
  {
    v = v * 10.0f + (float)(*s - '0');
    s++;
  }
  if (*s == '.')
  {
    float scale = 0.1f;
    s++;
    if (*s < '0' || *s > '9')          /* "3." 这种写法不允许 */
    {
      return CALC_SYNTAX;
    }
    while (*s >= '0' && *s <= '9')
    {
      v += (float)(*s - '0') * scale;
      scale *= 0.1f;
      s++;
    }
  }
  *p = s;
  *out = sign * v;
  return CALC_OK;
}

/* 读"一项"：数字（含内部的所有 * / 运算）。
 * 例：读到 "2*3+4" 时，吃掉 "2*3"，返回 6，游标停在 '+'。 */
static calc_status_t parse_term(const char **p, float *out)
{
  const char *s = *p;
  float v;
  calc_status_t st;

  /* 1) 第一项：一个数 */
  st = parse_number(&s, &v);
  if (st != CALC_OK)
  {
    return st;
  }

  /* 2) 循环吃掉所有 * /（乘除优先级高，先在"项"内部算完） */
  while (*s == '*' || *s == '/')
  {
    char  op = *s;
    float rhs;

    s++;
    st = parse_number(&s, &rhs);
    if (st != CALC_OK)
    {
      return CALC_SYNTAX;      /* 例："2*" 后面没数 */
    }
    if (op == '/')
    {
      if (rhs == 0.0f)
      {
        return CALC_DIV_ZERO;  /* 除零：给专用提示，不崩不卡 */
      }
      v = v / rhs;
    }
    else
    {
      v = v * rhs;
    }
  }

  /* 3) 成功：回写游标和结果（游标停在不是 * / 的地方） */
  *p = s;
  *out = v;
  return CALC_OK;
}

/* 加减扫描：把一个个"项"用 + / - 连起来 */
static calc_status_t parse_expression(const char **p, float *out)
{
  const char *q = *p;          /* 别直接改 *p，解析成功后再回写 */
  float acc;
  float term;
  calc_status_t st;

  /* 第一项（parse_term 内部已经算完乘除） */
  st = parse_term(&q, &acc);
  if (st != CALC_OK)
  {
    return st;
  }

  /* 用 + / - 把一项项连起来（加减优先级低，放外层扫描）
   * 例："1+2*3-4"：acc=1 → +6 → -4 = 3 */
  while (*q == '+' || *q == '-')
  {
    char op = *q;

    q++;
    st = parse_term(&q, &term);
    if (st != CALC_OK)
    {
      return st;
    }
    if (op == '+')
    {
      acc += term;
    }
    else
    {
      acc -= term;
    }
  }

  /* 成功：回写游标（停在 '\0' 或无法识别的字符上）和结果 */
  *p = q;
  *out = acc;
  return CALC_OK;
}

/* ====== 契约函数：与头文件一字不差 ====== */
calc_status_t calculator_evaluate(const char *expression,
                                  calc_angle_unit_t angle_unit,
                                  uint8_t allow_complex,
                                  calc_complex_t answer,
                                  calc_complex_t *result)
{
  const char *p = expression;
  float v;
  calc_status_t st;

  /* 本期先不做复数/三角函数：这三个参数暂时用不到（契约要求保留） */
  (void)angle_unit;
  (void)allow_complex;
  (void)answer;

  st = parse_expression(&p, &v);
  if (st != CALC_OK)
  {
    return st;
  }
  if (*p != '\0')
  {
    return CALC_SYNTAX;        /* 尾巴没吃完，比如 "3+"、"1+2)" */
  }

  result->real = v;
  result->imag = 0.0f;
  return CALC_OK;
}

/* calculator_solve_linear：目前没有任何代码调用它，可以先不定义（不会报错）；
 * 想顺手实现也很简单（a != 0 时 *x = -b / a）——你定。 */

/* 注：app_format_float 是"显示层"函数，2026-10-06 已移回 calculator_app.c */