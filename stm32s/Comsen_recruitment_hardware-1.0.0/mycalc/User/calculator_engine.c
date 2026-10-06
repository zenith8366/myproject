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
 * 【文法】四层递归下降（全复数域运算）：
 *   expression := term (('+' | '-' | '@') term)*
 *   term       := factor (('*' | '/') factor | 隐式乘法)*
 *   factor     := ('+' | '-') factor | power
 *   power      := primary ('^' factor)?          （^ = 幂运算，右结合；SHIFT+* 输入）
 *   primary    := number | '(' expression ')' | 常数 | 函数 '(' expression ')'
 *   number     := 数字[.数字][E[+/-]数字]        （E = ×10^x，T27 键）
 *   常数：pi、e、i       函数：sin( cos( tan( log( ln( sqrt(
 *
 * 【复数】（COMPLX 模式，allow_complex=1）：
 *   - i 虚数单位、四则运算全复数域（乘/除按复数公式）；
 *   - '@' 极坐标算子：a@θ = a·(cosθ + i·sinθ)，θ 的单位听 angle_unit
 *     （这就是"角度/弧度换算"——DEG 时 θ° 先换成弧度再算）；
 *   - sqrt 负数出纯虚数（√(r∠θ) = √r∠θ/2）；log/ln 支持复数（ln|z|+i·arg z）；
 *   - 幂：z^w = e^(w·Ln z)，负底数的非整数次幂出主值复数（(-1)^0.5 = i）；
 *     双实数时直接 powf（(-2)^3 = -8 走实数路径）；
 *   - sin/cos/tan 暂不支持复数参数（返回 CALC_DOMAIN）。
 * COMP 模式（allow_complex=0）下 sqrt 负数给 NaN（App 显示 Error）。
 *
 * 数学函数用 libm（CMakeLists 链接 m）；不用 printf。
 * ========================================================================== */

#include "calculator_engine.h"
#include <math.h>              /* sinf/cosf/tanf/log10f/logf/sqrtf/hypotf/atan2f */

#define CALC_PI    3.14159265358979f
#define CALC_E     2.71828182845905f
#define CALC_LN10  2.30258509299405f
#define CALC_TAN_EPS 1.0e-6f   /* tan 判域阈值：|cos| 小于它视为 π/2+kπ（无定义） */

/* 解析上下文：角度制 / 是否允许复数（由 calculator_evaluate 的参数带入） */
typedef struct
{
  calc_angle_unit_t unit;
  uint8_t           allow_complex;
} parse_ctx_t;

/* ------------------------------ 复数小工具 ------------------------------- */

static calc_complex_t cx_make(float re, float im)
{
  calc_complex_t c;

  /* -0.0 归一成 +0.0：负零会让 atan2(±0, 负数) 的辐角符号翻车
   *（sqrt(-4) 会算成 -2i、ln(-1) 会算成 -iπ），显示上也会冒 "-0.00"。 */
  c.real = (re == 0.0f) ? 0.0f : re;
  c.imag = (im == 0.0f) ? 0.0f : im;
  return c;
}

static calc_complex_t cx_add(calc_complex_t a, calc_complex_t b)
{
  return cx_make(a.real + b.real, a.imag + b.imag);
}

static calc_complex_t cx_sub(calc_complex_t a, calc_complex_t b)
{
  return cx_make(a.real - b.real, a.imag - b.imag);
}

static calc_complex_t cx_mul(calc_complex_t a, calc_complex_t b)
{
  return cx_make(a.real * b.real - a.imag * b.imag,
                 a.real * b.imag + a.imag * b.real);
}

/* 复数除法：(a+bi)/(c+di) = ((ac+bd) + (bc-ad)i) / (c²+d²) */
static calc_status_t cx_div(calc_complex_t a, calc_complex_t b, calc_complex_t *out)
{
  float denom = b.real * b.real + b.imag * b.imag;

  if (denom == 0.0f)
  {
    return CALC_DIV_ZERO;
  }
  out->real = (a.real * b.real + a.imag * b.imag) / denom;
  out->imag = (a.imag * b.real - a.real * b.imag) / denom;
  return CALC_OK;
}

/* 幂运算 a^b（'^'，SHIFT+* 输入）：
 *   - 双实数且"实数可算"：直接 powf（(-2)^3 = -8；0^0 = 1）；
 *     域外结果（如 COMP 模式的 (-8)^0.5）得 NaN，App 显示 Error；
 *   - 负底数 + 非整数指数、或有虚部参与：z^w = e^(w·Ln z)（仅 COMPLX 模式）——
 *     主值复数：(-1)^0.5 = i；2^i = cos(ln2)+i·sin(ln2)。 */
static calc_status_t cx_pow(calc_complex_t a, calc_complex_t b,
                            const parse_ctx_t *ctx, calc_complex_t *out)
{
  if ((a.imag == 0.0f) && (b.imag == 0.0f))
  {
    /* 负底数的非整数次幂在实数域无定义——COMPLX 模式让它掉进下面的复数路径 */
    if (!((a.real < 0.0f) && (b.real != floorf(b.real)) && (ctx->allow_complex != 0U)))
    {
      out->real = powf(a.real, b.real);
      out->imag = 0.0f;
      return CALC_OK;
    }
  }
  else if (ctx->allow_complex == 0U)
  {
    return CALC_DOMAIN;                /* COMP 模式拒绝复数参与 */
  }

  if ((a.real == 0.0f) && (a.imag == 0.0f))
  {
    if (b.real > 0.0f)                 /* 0^正数 = 0（0 的实数正幂） */
    {
      *out = cx_make(0.0f, 0.0f);
      return CALC_OK;
    }
    return CALC_DIV_ZERO;              /* 0^非正：无定义 / 除零 */
  }

  {
    float lr = logf(hypotf(a.real, a.imag));    /* Ln z = ln|z| + i·arg z */
    float li = atan2f(a.imag, a.real);
    float er = b.real * lr - b.imag * li;       /* w·Ln z 的实部 */
    float ei = b.real * li + b.imag * lr;       /* w·Ln z 的虚部 */
    float em = expf(er);                        /* e^(w·Ln z) */

    *out = cx_make(em * cosf(ei), em * sinf(ei));
    return CALC_OK;
  }
}

/* ------------------------------- 词法/语法 -------------------------------- */

/* 读一个数字（纯数字部分；正负号归 parse_factor，复数单位 i 归 primary） */
static calc_status_t parse_number(const char **p, calc_complex_t *out)
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
  *out = cx_make(v, 0.0f);
  return CALC_OK;
}

/* 前向声明：primary ↔ expression 互相递归调用 */
static calc_status_t parse_expression(const char **p, calc_complex_t *out,
                                      const parse_ctx_t *ctx);

/* 函数：吃掉 "名字( 表达式 )" 并计算。
 * 实数参数走实数库函数；复数参数见开头注释的功能范围。 */
static calc_status_t parse_function(const char **p, calc_complex_t *out,
                                    const parse_ctx_t *ctx)
{
  const char *s = *p;
  calc_complex_t v;
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
  st = parse_expression(&s, &v, ctx);
  if (st != CALC_OK)
  {
    return st;
  }
  if (*s != ')')
  {
    return CALC_SYNTAX;
  }
  s++;

  if (kind <= 2U)                      /* sin / cos / tan：暂不支持复数参数 */
  {
    float x;

    if (v.imag != 0.0f)
    {
      return CALC_DOMAIN;
    }
    x = v.real;
    if (ctx->unit == CALC_ANGLE_DEG)
    {
      x = x * (CALC_PI / 180.0f);      /* 角度制 → 弧度（角度/弧度换算） */
    }
    if (kind == 0U)
    {
      v = cx_make(sinf(x), 0.0f);
    }
    else if (kind == 1U)
    {
      v = cx_make(cosf(x), 0.0f);
    }
    else if (fabsf(cosf(x)) < CALC_TAN_EPS)
    {
      /* tan 在 π/2 + kπ 处数学上无定义。float 的 π/2 自身约有 4e-8 的误差，
       * 直接 tanf() 会吐出一个巨大的伪值（tan(90°) ≈ -2.3e7）而不是报错——
       * 这里主动判域：|cos| 小于阈值即视为无定义，给 NaN 让 App 显示 Error。 */
      v = cx_make(NAN, 0.0f);
    }
    else
    {
      v = cx_make(tanf(x), 0.0f);
    }
  }
  else if (kind <= 4U)                 /* log（10 底）/ ln（e 底） */
  {
    if (v.imag != 0.0f || (v.real < 0.0f && ctx->allow_complex != 0U))
    {
      if (ctx->allow_complex == 0U)
      {
        if (v.imag != 0.0f)
        {
          return CALC_DOMAIN;          /* COMP 模式拒绝复数参数 */
        }
        /* 负实数的 log 交给库函数产生 NaN，App 显示 Error */
        v = cx_make((kind == 3U) ? log10f(v.real) : logf(v.real), 0.0f);
      }
      else
      {
        /* 复数对数：ln(z) = ln|z| + i·arg(z)；log10(z) = ln(z)/ln10 */
        float m = hypotf(v.real, v.imag);
        float a = atan2f(v.imag, v.real);
        float l = logf(m);

        if (kind == 3U)
        {
          l /= CALC_LN10;
          a /= CALC_LN10;
        }
        v = cx_make(l, a);
      }
    }
    else
    {
      v = cx_make((kind == 3U) ? log10f(v.real) : logf(v.real), 0.0f);
    }
  }
  else                                 /* sqrt（根号） */
  {
    if (v.imag != 0.0f || (v.real < 0.0f && ctx->allow_complex != 0U))
    {
      if (ctx->allow_complex == 0U)
      {
        if (v.imag != 0.0f)
        {
          return CALC_DOMAIN;          /* COMP 模式拒绝复数参数 */
        }
        v = cx_make(sqrtf(v.real), 0.0f);   /* 负数 → NaN → App 显示 Error */
      }
      else
      {
        /* 极坐标半角：√(r∠θ) = √r ∠(θ/2)（sqrt(-4) → 2i） */
        float m = hypotf(v.real, v.imag);
        float a = atan2f(v.imag, v.real) * 0.5f;
        float r = sqrtf(m);

        v = cx_make(r * cosf(a), r * sinf(a));
      }
    }
    else
    {
      v = cx_make(sqrtf(v.real), 0.0f);
    }
  }

  *p = s;
  *out = v;
  return CALC_OK;
}

/* 主元：数字 | '(' 表达式 ')' | 常数 pi / e / i | 函数 */
static calc_status_t parse_primary(const char **p, calc_complex_t *out,
                                   const parse_ctx_t *ctx)
{
  const char *s = *p;

  if (*s == '(')
  {
    calc_status_t st;

    s++;
    st = parse_expression(&s, out, ctx);
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
    *out = cx_make(CALC_PI, 0.0f);
    return CALC_OK;
  }
  if (s[0] == 'e')                     /* 常数 e（按键：SHIFT+4） */
  {
    *p = s + 1;
    *out = cx_make(CALC_E, 0.0f);
    return CALC_OK;
  }
  if (s[0] == 'i')                     /* 虚数单位（按键：SHIFT+9，COMPLX 模式） */
  {
    *p = s + 1;
    *out = cx_make(0.0f, 1.0f);
    return CALC_OK;
  }
  if (s[0] >= 'a' && s[0] <= 'z')      /* 其它字母开头 → 函数（不认识就报语法错） */
  {
    return parse_function(p, out, ctx);
  }
  return parse_number(p, out);
}

/* 前向声明：power 的右操作数要递归回 factor（右结合 + 允许指数带一元符号） */
static calc_status_t parse_factor(const char **p, calc_complex_t *out,
                                  const parse_ctx_t *ctx);

/* 幂：primary ('^' factor)? —— 右结合（2^3^2 = 2^(3^2)）；
 * 指数走 factor，所以允许一元符号（2^-3）与继续叠幂。
 * 优先级：高于 * /（2*3^2 = 18）；一元符号比它低（-2^2 = -(2^2) = -4）。 */
static calc_status_t parse_power(const char **p, calc_complex_t *out,
                                 const parse_ctx_t *ctx)
{
  const char *s = *p;
  calc_complex_t base;
  calc_status_t st;

  st = parse_primary(&s, &base, ctx);
  if (st != CALC_OK)
  {
    return st;
  }
  if (*s == '^')
  {
    calc_complex_t ex;

    s++;
    st = parse_factor(&s, &ex, ctx);   /* "2^" 后面没数 → 语法错自然上抛 */
    if (st != CALC_OK)
    {
      return st;
    }
    st = cx_pow(base, ex, ctx, &base);
    if (st != CALC_OK)
    {
      return st;
    }
  }
  *p = s;
  *out = base;
  return CALC_OK;
}

/* 因子：一元 + / - 前缀，后面跟幂。支持 "-5+3"、"3+-4"、"(-2)" */
static calc_status_t parse_factor(const char **p, calc_complex_t *out,
                                  const parse_ctx_t *ctx)
{
  const char *s = *p;

  if (*s == '+')
  {
    *p = s + 1;
    return parse_factor(p, out, ctx);
  }
  if (*s == '-')
  {
    calc_status_t st;

    *p = s + 1;
    st = parse_factor(p, out, ctx);
    if (st == CALC_OK)
    {
      *out = cx_make(-out->real, -out->imag);
    }
    return st;
  }
  return parse_power(p, out, ctx);
}

/* 读"一项"：因子 + 乘除（含隐式乘法："2pi"、"3sin(30)"、"10.00i"）。 */
static calc_status_t parse_term(const char **p, calc_complex_t *out,
                                const parse_ctx_t *ctx)
{
  const char *s = *p;
  calc_complex_t v;
  calc_status_t st;

  st = parse_factor(&s, &v, ctx);
  if (st != CALC_OK)
  {
    return st;
  }

  /* 显式 * / ；或隐式乘法（下一个 token 是"因子开头"：数字/字母/'('）——
   * 这让回填的结果串（如 "-5.00+10.00i"）可以直接再解析。 */
  while (*s == '*' || *s == '/' || *s == '(' ||
         (*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z'))
  {
    char  op = *s;
    calc_complex_t rhs;

    if (op == '*' || op == '/')
    {
      s++;
    }
    else
    {
      op = '*';                        /* 隐式乘法：不消耗字符，直接读下一个因子 */
    }

    st = parse_factor(&s, &rhs, ctx);
    if (st != CALC_OK)
    {
      return CALC_SYNTAX;              /* 例："2*" 后面没数 */
    }
    if (op == '/')
    {
      st = cx_div(v, rhs, &v);
      if (st != CALC_OK)
      {
        return st;                     /* 除零（实部虚部同时为 0 时） */
      }
    }
    else
    {
      v = cx_mul(v, rhs);
    }
  }

  *p = s;
  *out = v;
  return CALC_OK;
}

/* 加减 + 极坐标扫描：
 *   a + b / a - b：复数加减；
 *   a@θ：极坐标——a·(cosθ + i·sinθ)，θ 按 angle_unit 换算（角度/弧度）。 */
static calc_status_t parse_expression(const char **p, calc_complex_t *out,
                                      const parse_ctx_t *ctx)
{
  const char *q = *p;
  calc_complex_t acc;
  calc_complex_t term;
  calc_status_t st;

  st = parse_term(&q, &acc, ctx);
  if (st != CALC_OK)
  {
    return st;
  }

  while (*q == '+' || *q == '-' || *q == '@')
  {
    char op = *q;

    q++;
    st = parse_term(&q, &term, ctx);
    if (st != CALC_OK)
    {
      return st;
    }
    if (op == '+')
    {
      acc = cx_add(acc, term);
    }
    else if (op == '-')
    {
      acc = cx_sub(acc, term);
    }
    else                           /* '@'：极坐标 a∠θ */
    {
      float m  = hypotf(acc.real, acc.imag);   /* 模（输入通常是实数，兼容复数） */
      float th = term.real;                    /* 辐角取实部 */

      if (ctx->unit == CALC_ANGLE_DEG)
      {
        th = th * (CALC_PI / 180.0f);          /* DEG → RAD（角度/弧度换算） */
      }
      acc = cx_make(m * cosf(th), m * sinf(th));
    }
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
  parse_ctx_t ctx;
  calc_complex_t v;
  calc_status_t st;

  /* answer（按值传参）暂未使用——留给以后的 ANS 回填 */
  (void)answer;

  ctx.unit = angle_unit;
  ctx.allow_complex = allow_complex;

  st = parse_expression(&p, &v, &ctx);
  if (st != CALC_OK)
  {
    return st;
  }
  if (*p != '\0')
  {
    return CALC_SYNTAX;        /* 尾巴没吃完，比如 "3+"、"1+2)" */
  }

  *result = v;
  return CALC_OK;
}

/* calculator_solve_linear：目前没有任何代码调用它，可以先不定义（不会报错）；
 * 想顺手实现也很简单（a != 0 时 *x = -b / a）——你定。 */
