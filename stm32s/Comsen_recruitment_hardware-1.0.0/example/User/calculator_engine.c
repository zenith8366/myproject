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
 *
 * 【文法】三层递归下降 + 一元/主元层（D7 任务 5 升级版）：
 *   expression := term  (('+' | '-') term)*
 *   term       := factor (('*' | '/') factor)*
 *   factor     := ('+' | '-') factor | primary
 *   primary    := number | '(' expression ')' | 常数 | 函数 '(' expression ')'
 *   number     := 数字[.数字][E[+/-]数字]        （E = ×10^x，T27 键）
 *   常数：pi、e          函数：sin( cos( tan( log( ln( sqrt(
 *
 * sin/cos/tan 的角度/弧度由 angle_unit 参数决定（App 里 MODE 菜单的全局设置）。
 * 数学函数用 libm（CMakeLists 链接 m）；不用 printf。
 * ========================================================================== */

#include "calculator_engine.h"
#include <math.h>              /* sinf / cosf / tanf / log10f / logf / sqrtf */

#define CALC_PI   3.14159265358979f
#define CALC_E    2.71828182845905f

/* 读一个数字：整数[.小数][E[+/-]指数]。正负号不在这层（归 parse_factor 的一元层） */
static calc_status_t parse_number(const char **p, float *out)
{
  const char *s = *p;
  float v = 0.0f;

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
  if (*s == 'E')                       /* ×10^x 科学计数（T27 键输入 'E'） */
  {
    int sign = 1;
    int exp = 0;

    s++;
    if (*s == '+')
    {
      s++;
    }
    else if (*s == '-')
    {
      sign = -1;
      s++;
    }
    if (*s < '0' || *s > '9')          /* "1E" 后面没指数 */
    {
      return CALC_SYNTAX;
    }
    while (*s >= '0' && *s <= '9')
    {
      if (exp < 100)                   /* 封顶：更大的指数结果必然是 inf/0，不必精确 */
      {
        exp = exp * 10 + (*s - '0');
      }
      s++;
    }
    while (exp > 0)
    {
      v = (sign > 0) ? (v * 10.0f) : (v / 10.0f);
      exp--;
    }
  }
  *p = s;
  *out = v;
  return CALC_OK;
}

/* 前向声明：primary ↔ expression 互相递归调用 */
static calc_status_t parse_expression(const char **p, float *out, calc_angle_unit_t unit);

/* 函数：名字开头已在游标处，吃掉 "名字( 表达式 )" 并计算。
 * 成功返回 CALC_OK；名字不认识 / 缺括号 → CALC_SYNTAX。
 * 定义域外（log 非正、sqrt 负数）交给 libm 产生 NaN/-inf，
 * 由 App 的格式化层的范围检查显示 "Error"——不崩、不卡。 */
static calc_status_t parse_function(const char **p, float *out, calc_angle_unit_t unit)
{
  const char *s = *p;
  float v;
  calc_status_t st;
  uint8_t kind;

  if (s[0] == 's' && s[1] == 'i' && s[2] == 'n')
  {
    kind = 0U;
    s += 3;
  }
  else if (s[0] == 'c' && s[1] == 'o' && s[2] == 's')
  {
    kind = 1U;
    s += 3;
  }
  else if (s[0] == 't' && s[1] == 'a' && s[2] == 'n')
  {
    kind = 2U;
    s += 3;
  }
  else if (s[0] == 'l' && s[1] == 'o' && s[2] == 'g')
  {
    kind = 3U;
    s += 3;
  }
  else if (s[0] == 'l' && s[1] == 'n')
  {
    kind = 4U;
    s += 2;
  }
  else if (s[0] == 's' && s[1] == 'q' && s[2] == 'r' && s[3] == 't')
  {
    kind = 5U;
    s += 4;
  }
  else
  {
    return CALC_SYNTAX;                /* 不认识的字母序列 */
  }

  if (*s != '(')                       /* 函数名后必须紧跟 '('（按键输入会自动带上） */
  {
    return CALC_SYNTAX;
  }
  s++;
  st = parse_expression(&s, &v, unit);
  if (st != CALC_OK)
  {
    return st;
  }
  if (*s != ')')
  {
    return CALC_SYNTAX;
  }
  s++;

  switch (kind)
  {
    case 0U:                           /* sin：角度制先度→弧度 */
      if (unit == CALC_ANGLE_DEG)
      {
        v = v * (CALC_PI / 180.0f);
      }
      v = sinf(v);
      break;
    case 1U:                           /* cos */
      if (unit == CALC_ANGLE_DEG)
      {
        v = v * (CALC_PI / 180.0f);
      }
      v = cosf(v);
      break;
    case 2U:                           /* tan */
      if (unit == CALC_ANGLE_DEG)
      {
        v = v * (CALC_PI / 180.0f);
      }
      v = tanf(v);
      break;
    case 3U:                           /* log：10 底 */
      v = log10f(v);
      break;
    case 4U:                           /* ln：e 底 */
      v = logf(v);
      break;
    default:                           /* sqrt：根号 */
      v = sqrtf(v);
      break;
  }

  *p = s;
  *out = v;
  return CALC_OK;
}

/* 主元：数字 | '(' 表达式 ')' | 常数 pi / e | 函数 */
static calc_status_t parse_primary(const char **p, float *out, calc_angle_unit_t unit)
{
  const char *s = *p;

  if (*s == '(')
  {
    calc_status_t st;

    s++;
    st = parse_expression(&s, out, unit);
    if (st != CALC_OK)
    {
      return st;
    }
    if (*s != ')')
    {
      return CALC_SYNTAX;
    }
    *p = s + 1;
    return CALC_OK;
  }
  if (s[0] == 'p' && s[1] == 'i')      /* 常数 π（按键：SHIFT+7） */
  {
    *p = s + 2;
    *out = CALC_PI;
    return CALC_OK;
  }
  if (s[0] == 'e')                     /* 常数 e（按键：SHIFT+4） */
  {
    *p = s + 1;
    *out = CALC_E;
    return CALC_OK;
  }
  if (s[0] >= 'a' && s[0] <= 'z')      /* 字母开头 → 函数（不认识就报语法错） */
  {
    return parse_function(p, out, unit);
  }
  return parse_number(p, out);
}

/* 因子：一元 + / - 前缀，后面跟主元。支持 "-5+3"、"3+-4"、"(-2)" */
static calc_status_t parse_factor(const char **p, float *out, calc_angle_unit_t unit)
{
  const char *s = *p;

  if (*s == '+')
  {
    *p = s + 1;
    return parse_factor(p, out, unit);
  }
  if (*s == '-')
  {
    calc_status_t st;

    *p = s + 1;
    st = parse_factor(p, out, unit);
    if (st == CALC_OK)
    {
      *out = -*out;
    }
    return st;
  }
  return parse_primary(p, out, unit);
}

/* 读"一项"：因子（含内部的所有 * / 运算）。
 * 例：读到 "2*(3+4)+5" 时，吃掉 "2*(3+4)"，返回 14，游标停在 '+'。 */
static calc_status_t parse_term(const char **p, float *out, calc_angle_unit_t unit)
{
  const char *s = *p;
  float v;
  calc_status_t st;

  st = parse_factor(&s, &v, unit);
  if (st != CALC_OK)
  {
    return st;
  }

  while (*s == '*' || *s == '/')
  {
    char  op = *s;
    float rhs;

    s++;
    st = parse_factor(&s, &rhs, unit);
    if (st != CALC_OK)
    {
      return CALC_SYNTAX;              /* 例："2*" 后面没数 */
    }
    if (op == '/')
    {
      if (rhs == 0.0f)
      {
        return CALC_DIV_ZERO;          /* 除零：给专用提示，不崩不卡 */
      }
      v = v / rhs;
    }
    else
    {
      v = v * rhs;
    }
  }

  *p = s;
  *out = v;
  return CALC_OK;
}

/* 加减扫描：把一个个"项"用 + / - 连起来（最低优先级） */
static calc_status_t parse_expression(const char **p, float *out, calc_angle_unit_t unit)
{
  const char *q = *p;
  float acc;
  float term;
  calc_status_t st;

  st = parse_term(&q, &acc, unit);
  if (st != CALC_OK)
  {
    return st;
  }

  while (*q == '+' || *q == '-')
  {
    char op = *q;

    q++;
    st = parse_term(&q, &term, unit);
    if (st != CALC_OK)
    {
      return st;
    }
    acc = (op == '+') ? (acc + term) : (acc - term);
  }

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

  /* COMPLX（复数）模式目前只做了"入口"：求值本体仍按实数计算；
   * answer（按值传参）暂未使用——留给以后的 ANS 回填。 */
  (void)allow_complex;
  (void)answer;

  st = parse_expression(&p, &v, angle_unit);
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
