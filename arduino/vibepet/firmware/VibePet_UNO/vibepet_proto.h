// vibepet_proto.h —— VibePet 的协议解析内核（行重组 + 极简 JSON 字段提取）
//
// ━━━━━━━━━━━ 这个文件是干嘛的 ━━━━━━━━━━━
//
// 设备要在 2KB 内存的 8 位机上，从串口不断到来的字节流里认出「一条消息」，
// 再把里面的几个字段取出来。本来这活儿装个 JSON 库就完了，但 ArduinoJson 这类
// 库在这块芯片上代价太高（Flash 多几 KB、堆内存还不可控），所以这里自己写。
//
// 它只做两件事，都是本项目协议用得上的最小集合：
//
//   1. 行重组：串口一次读到的字节**不保证**正好是一条消息。可能半条、可能两条半。
//      所以先按字节攒进行缓冲，遇到 '\n' 才算攒够一条。攒的时候**只能按字节**，
//      不能先转成文字 —— 一个汉字在 UTF-8 里占 3 个字节，万一被拆到两次读取里，
//      先解码就会变成乱码。按字节攒、切分完再解码，就绝不会切坏汉字。
//
//   2. 取字段：从 `{"type":"state","status":"working","msg":"..."}` 这样的行里，
//      把 type / status / msg / request_id / tool / summary 取出来。
//
// ─────────────── 为什么不用「查字符串」的土办法 ───────────────
//
// 因为要取的值里可能**包含和语法一样的字符**。比如 summary 是要执行的命令，
// 完全可能是 `{"type":"state","msg":"echo {\"a\":1}"}` 这种带引号和花括号的内容；
// 也可能带 Windows 路径 `C:\\Users\\...`（JSON 里反斜杠要写成两个）。
// 所以这里老老实实按 JSON 的语法走一遍：跳过键、跳过值、处理转义，只是不走
// 「构造对象」，而是边走边看有没有我们要的字段。能处理：
//
//   · `\\` `\"` `\/` `\b` `\f` `\n` `\r` `\t` 这些常见转义
//   · `\uXXXX`（Python 的 json.dumps 遇到控制字符就会写成这样）
//   · 值里嵌套的对象 / 数组（不会被里面的 `}` 骗到，提前结束解析）
//   · 写不下时**只截断到完整汉字的边界**，绝不切出半个字
//
// ─────────────── 内存 ───────────────
//
// 全静态，无 malloc。行缓冲 320 字节（VpLine），字段目标由调用方提供。
// 解析失败时**什么都不改**，调用方保留原状态 —— 屏幕上宁可显示旧内容，
// 也不该因为一条坏消息变成空白。

#ifndef VIBEPET_PROTO_H
#define VIBEPET_PROTO_H

#include <Arduino.h>

// 整行上限（含结尾的 '\n' 之前的正文）。与电脑端 daemon 的约定一致：
// 一条审批请求最坏情况约 258 字节（骨架 66 + request_id 8 + tool 24 + summary 160），
// 所以留到 320。超长的行整条丢弃 —— 绝不把半截报文当消息处理。
#define VP_LINE_MAX 320

// 行缓冲：攒字节 + 判断状态
struct VpLine {
  char buf[VP_LINE_MAX];
  uint16_t len;      // 当前攒了多少字节
  bool dropping;     // 正在丢弃一条超长的行（要一路丢到 '\n' 为止）
  bool ready;        // buf 里已经有一条完整的行等着处理
};

// 取字段的落点。buf[len] 恒为 '\0'，所以可以直接当 C 字符串用。
struct VpSink {
  char *buf;
  uint16_t cap;      // 最多写 cap-1 个字节，外加一个结尾的 '\0'
  uint16_t len;
};

// ───────────────────────── 行重组 ─────────────────────────

static inline void vpLineInit(VpLine *line) {
  line->len = 0;
  line->dropping = false;
  line->ready = false;
  line->buf[0] = '\0';
}

// 喂一个字节进去。攒够一整行就把 ready 置上（**不在这里处理**，见下面的说明）。
//
// 为什么收和分要分开？因为处理一条消息可能要重绘整屏（几十毫秒），而重绘期间
// 串口还在不停来数据。要是「收到就处理」，重绘时就会陷进递归；分开之后，
// 渲染循环里只要调用 vpLineFeed 收字节（很快，几微秒），真正的处理留给主循环。
static inline void vpLineFeed(VpLine *line, char c) {
  if (c == '\n') {
    if (!line->dropping) {
      line->buf[line->len] = '\0';
      line->ready = true;
    }
    // 不论是正常结束还是丢弃结束，都要复位
    line->dropping = false;
    line->len = 0;
    return;
  }
  if (line->dropping) return;          // 这条行已经判定超长，扔到行尾为止
  if (line->len >= VP_LINE_MAX - 1) {  // 攒满了还没见到 '\n' → 整条丢弃
    line->dropping = true;
    line->len = 0;
    line->buf[0] = '\0';
    return;
  }
  line->buf[line->len++] = c;
}

// 一条行处理完了。**只把 ready 摘掉，绝不动 len 和 buf。**
//
// 为什么不能顺手清空缓冲？因为处理一条消息可能要重绘整屏（几十毫秒），
// 这期间串口会源源不断送来**下一条消息的开头若干字节**，它们正躺在缓冲里
// 排着队。要是这里清空，下一条消息就被截成半截 —— 解析失败、整条丢掉，
// 而屏幕上什么都不会显示。
//
// 正确的用法是在主循环里这样转（见 VibePet_UNO.ino）：
//     while (line.ready) { vpLineConsume(&line); dispatchLine(); }
// 先摘旗再处理：万一处理期间又攒够一整条，ready 会被重新置上，
// 下一轮循环就能接着处理，一条都不会漏。
static inline void vpLineConsume(VpLine *line) {
  line->ready = false;
}

// ───────────────────────── 取字段 ─────────────────────────

// 把落点清空（buf[0] = '\0'），调用方在解析前调用
static inline void vpSinkInit(VpSink *sink, char *buf, uint16_t cap) {
  sink->buf = buf;
  sink->cap = cap;
  sink->len = 0;
  if (cap > 0) buf[0] = '\0';
}

static inline const char *vpSkipWs(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

// 跳过一段字符串（p 指向开引号），返回闭引号之后的位置
static inline const char *vpSkipString(const char *p) {
  p++;  // 开引号
  while (*p != '\0' && *p != '"') {
    if (*p == '\\' && p[1] != '\0') p++;  // 转义：连同被转义的字符一起跳过
    p++;
  }
  if (*p == '"') p++;
  return p;
}

// 跳过任意一个值（字符串 / 对象 / 数组 / 数字 / true / false / null）
static inline const char *vpSkipValue(const char *p) {
  p = vpSkipWs(p);
  if (*p == '"') return vpSkipString(p);
  if (*p == '{' || *p == '[') {
    // 括号配对。字符串里的括号不算数，所以要顺手跳过字符串。
    char open = *p;
    char close = (open == '{') ? '}' : ']';
    int8_t depth = 0;
    while (*p != '\0') {
      if (*p == '"') {
        p = vpSkipString(p);
        continue;
      }
      if (*p == open) {
        depth++;
      } else if (*p == close) {
        depth--;
        p++;
        if (depth <= 0) return p;
        continue;
      }
      p++;
    }
    return p;
  }
  // 数字 / true / false / null：一路走到分隔符
  while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
         *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
    p++;
  }
  return p;
}

// 比较字符串值（p 指向开引号）是否等于 match。零拷贝，不占额外内存。
static inline bool vpMatchString(const char *p, const char *match) {
  const char *m = match;
  p++;
  while (*p != '\0' && *p != '"') {
    char c;
    if (*p == '\\') {
      p++;
      if (*p == '\0') break;
      c = *p;  // 我们的 match 都是纯 ASCII，转义后按原字符比即可
    } else {
      c = *p;
    }
    if (*m == '\0' || *m != c) return false;
    m++;
    p++;
  }
  return (*p == '"' && *m == '\0');
}

static inline uint8_t vpUtf8Len(uint8_t lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 0;  // 不是合法的 UTF-8 首字节
}

// 往落点里放一个字节。放不下就**丢弃**（不覆盖已写内容），返回是否写入。
static inline bool vpSinkPutByte(VpSink *sink, uint8_t byte) {
  if (sink->cap == 0 || sink->len + 1 >= sink->cap) return false;
  sink->buf[sink->len++] = (char)byte;
  sink->buf[sink->len] = '\0';
  return true;
}

// 放一个 Unicode 码点的 UTF-8 编码。**整段一起判断放不放得下**，
// 放不下就整段不要 —— 这样绝不会把一个汉字切成两半。
static inline void vpSinkPutCodePoint(VpSink *sink, uint16_t cp) {
  if (cp < 0x20) cp = ' ';        // 控制字符（含 \n \t）在屏幕上显示成空格
  if (cp < 0x80) {
    vpSinkPutByte(sink, (uint8_t)cp);
  } else if (cp < 0x800) {
    if ((uint16_t)(sink->cap - sink->len) >= 3) {
      vpSinkPutByte(sink, (uint8_t)(0xC0 | (cp >> 6)));
      vpSinkPutByte(sink, (uint8_t)(0x80 | (cp & 0x3F)));
    }
  } else {
    if ((uint16_t)(sink->cap - sink->len) >= 4) {
      vpSinkPutByte(sink, (uint8_t)(0xE0 | (cp >> 12)));
      vpSinkPutByte(sink, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
      vpSinkPutByte(sink, (uint8_t)(0x80 | (cp & 0x3F)));
    }
  }
}

static inline int8_t vpHexDigit(char c) {
  if (c >= '0' && c <= '9') return (int8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (int8_t)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int8_t)(c - 'A' + 10);
  return -1;
}

// 把字符串值（含转义）反转义后写到落点里。p 指向开引号，返回是否成功走到闭引号。
static inline bool vpCopyString(const char *p, VpSink *sink) {
  p++;
  while (*p != '\0' && *p != '"') {
    if (*p != '\\') {
      // 普通字节。非 ASCII 的按 UTF-8 序列**整段**拷贝，避免切碎汉字。
      uint8_t lead = (uint8_t)*p;
      uint8_t need = vpUtf8Len(lead);
      if (need <= 1) {
        vpSinkPutByte(sink, lead);
        p++;
        continue;
      }
      // 检查剩下的是不是完整的续接字节
      bool complete = true;
      for (uint8_t i = 1; i < need; i++) {
        if (p[i] == '\0' || ((uint8_t)p[i] & 0xC0) != 0x80) {
          complete = false;
          break;
        }
      }
      if (!complete) {  // 残缺的序列：丢掉首字节，继续往下走
        p++;
        continue;
      }
      if ((uint16_t)(sink->cap - sink->len) >= (uint16_t)need + 1) {
        for (uint8_t i = 0; i < need; i++) vpSinkPutByte(sink, (uint8_t)p[i]);
      }
      p += need;
      continue;
    }

    // 转义序列
    p++;
    switch (*p) {
      case 'n': case 't': case 'r': case 'b': case 'f':
        vpSinkPutByte(sink, ' ');
        p++;
        break;
      case 'u': {
        // ⚠️ 这里必须是**无符号**：码点到 0xFFFF，用 int16_t 存的话
        //    · 大于 0x7FFF 的字（比如全角冒号 U+FF1A）会溢出成负数；
        //    · 下面那段「代理对变问号」的判断也会因为负数比较而永远不成立。
        uint16_t value = 0;
        uint8_t k = 0;
        for (; k < 4; k++) {
          int8_t digit = vpHexDigit(p[1 + k]);
          if (digit < 0) break;
          value = (uint16_t)(value * 16 + (uint16_t)digit);
        }
        if (k < 4) {  // 不是合法的 \uXXXX —— 丢掉反斜杠，把 u 当成普通字符
          vpSinkPutByte(sink, 'u');
          p++;
          break;
        }
        p += 5;
        // 代理对（emoji 之类）在 JSON 里写成两段 \uXXXX，设备既显示
        // 不了也不必支持：高位那段输出一个问号，低位那段直接跳过 ——
        // 这样一个 emoji 正好变成一个「?」，而不是两个。
        if (value >= 0xD800 && value <= 0xDFFF) {
          if (value < 0xDC00) vpSinkPutByte(sink, '?');
        } else {
          vpSinkPutCodePoint(sink, value);
        }
        break;
      }
      case '"': vpSinkPutByte(sink, '"'); p++; break;
      case '\\': vpSinkPutByte(sink, '\\'); p++; break;
      case '/': vpSinkPutByte(sink, '/'); p++; break;
      case '\0': break;  // 行尾截断，外层循环会结束
      default:
        // 没见过的转义：**丢掉反斜杠、保留原字符**。
        // 绝不能把那个孤零零的反斜杠放进结果里 —— 后面的 UTF-8 解码会乱。
        vpSinkPutByte(sink, (uint8_t)*p);
        p++;
        break;
    }
  }
  return (*p == '"');
}

// 在一条 JSON 行里找字段 key，把它的字符串值交给 sink（match 为 NULL），
// 或者比较它是否等于 match（match 不为 NULL，此时 sink 可以为 NULL）。
static inline bool vpJsonGet(const char *line, const char *key,
                             const char *match, VpSink *sink) {
  const char *p = vpSkipWs(line);
  if (*p != '{') return false;
  p++;
  uint8_t key_len = (uint8_t)strlen(key);

  while (true) {
    p = vpSkipWs(p);
    if (*p != '"') return false;  // 不是键 → 格式不对或到结尾了
    const char *key_start = p + 1;
    const char *key_end = vpSkipString(p);

    p = vpSkipWs(key_end);
    if (*p != ':') return false;
    p = vpSkipWs(p + 1);

    // 键名精确比较（键里不会有转义，我们的键都是纯 ASCII）
    if ((uint16_t)(key_end - key_start - 1) == key_len &&
        strncmp(key_start, key, key_len) == 0) {
      if (*p != '"') return false;  // 我们要的字段必须是字符串
      if (match != NULL) return vpMatchString(p, match);
      return vpCopyString(p, sink);
    }

    p = vpSkipValue(p);
    p = vpSkipWs(p);
    if (*p == ',') {
      p++;
      continue;
    }
    return false;  // '}' 或格式错误
  }
}

// 取字符串字段（含反转义、UTF-8 边界安全的截断）
static inline bool vpJsonScan(const char *line, const char *key, VpSink *sink) {
  sink->len = 0;
  if (sink->cap > 0) sink->buf[0] = '\0';
  return vpJsonGet(line, key, NULL, sink);
}

// 零拷贝比较固定值字段，例如 vpJsonKeyIs(line, "type", "state")
static inline bool vpJsonKeyIs(const char *line, const char *key, const char *value) {
  return vpJsonGet(line, key, value, NULL);
}

#endif  // VIBEPET_PROTO_H
