#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VibePet 中文字库子集生成器（电脑端 · 离线工具，不需要硬件）

━━━━━━━━━━━━━━━ 这个脚本在干嘛 ━━━━━━━━━━━━━━━

设备的屏幕芯片只有 32KB Flash，而完整的中文字库放不下：文泉驿点阵宋体 12px
在 U8g2 里是 202690 字节，是整片 Flash 的六倍多。所以这里只挑出设备**真正用得上**
的几百个字，把它们从 U8g2 的字库里抠出来，重新编码成一张紧凑的表，写进
`firmware/VibePet_UNO/cn_font.h`，跟固件一起编译。

为什么不干脆找个矢量字体缩小渲染？因为 12 像素的汉字用矢量渲染会糊成一团，
必须用**手工调过的点阵**。U8g2 里的这份文泉驿点阵宋体正好是这种，而且它已经装在
你这台机器上，不需要联网下载任何东西。

为什么不把 U8g2 的解码器搬到设备上？因为设备端根本不需要解码 —— 在本脚本里解码
一次、存成裸位图反而更小（平均 27.85 字节/字 → 20 字节/字），设备端也少一堆代码。

───────────────── 数据格式（U8g2 私有格式，逐行对照 u8g2_font.c）─────────────────

文件头 23 字节：

    0 glyph_cnt          1 bbx_mode           2 bits_per_0        3 bits_per_1
    4 bits_per_char_w    5 bits_per_char_h    6 bits_per_char_x  7 bits_per_char_y
    8 bits_per_delta_x   9 max_w             10 max_h            11 x_offset
    12 y_offset         13 ascent_A          14 descent_g        15 ascent_paren
    16 descent_paren    17-18 start_upper_A   19-20 start_lower_a 21-22 start_unicode（大端）

字形记录（紧跟在头部之后）：

    · 码点 < 0x100 的段：[码点 1 字节][size 1 字节][数据 size-2 字节]
    · Unicode 段：     [码点高 1 字节][码点低 1 字节][size 1 字节][数据 size-3 字节]

    注意 size **包含**记录头本身的字节数（这是本格式最容易搞错的地方，
    按「size = 数据长度」解析会走出来一堆乱序码点）。

字形数据是**按位打包的游程**（一个位流，不是字节对齐的）：

    w = 读 4 位，h = 读 4 位，x/y/d 各读 5 位（有符号，偏置编码：v - (1<<(n-1))）
    然后循环：读 a 位（bits_per_0，本字体是 2）→ 画 a 个背景像素
              读 b 位（bits_per_1，本字体是 2）→ 画 b 个前景像素
              读 1 位：还是 1 就再来一组 a/b，是 0 就结束「这一行」
              画够 h 行就整个字结束（判断依据是当前的 y 已经 >= h）
    像素沿着宽度 w 折行，一行画满就换到下一行行首。
    位流的读取是**每字节内低位在前**（见 u8g2_font.c 的 get_unsigned_bits）。

用法：

    python tools/gen_cn_font.py --verify        # 自检：解几个已知字形，逐位比对人眼可判
    python tools/gen_cn_font.py                 # 生成 firmware/VibePet_UNO/cn_font.h
    python tools/gen_cn_font.py --check         # 检查设备可见文案是否都被字集覆盖
    python tools/gen_cn_font.py --dump 中文测试  # 把几个字的点阵打成 ASCII 图（开发用）
"""

import argparse
import ast
import os
import re
import sys

# ───────────────────────── 默认路径与常量 ─────────────────────────

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(HERE)

# U8g2 库里那份字体源码（本机已安装；换机器时用 --font-src 指定）
FONT_SRC_DEFAULT = r"E:\Users\lyh35\Documents\Arduino\libraries\U8g2\src\clib\u8g2_fonts.c"
FONT_ARRAY_NAME = "u8g2_font_wqy12_t_gb2312"

CHARSET_DEFAULT = os.path.join(HERE, "cn_charset.txt")
OUT_DEFAULT = os.path.join(PROJECT_ROOT, "firmware", "VibePet_UNO", "cn_font.h")

# 设备端点阵格子：12 宽 × 13 高
#
# 为什么是 13 行而不是 12？因为汉字最高 11 行、且最底部可以顶到基线；
# 而 ASCII 里有下伸部分的字（g、p、逗号…）还要再往下伸一行。上面两种加起来
# 正好需要 13 行，少一行就会把逗号的尾巴切掉。
CELL_W = 12
CELL_H = 13
# 每字占的字节数：13 行 × 12 位 = 156 位 → 向上取整
BYTES_PER_GLYPH = (CELL_H * CELL_W + 7) // 8  # = 20

# 字体头里描述「每个字段占几位」的字段名，解析时按这个顺序取
HEADER_FIELDS = (
    "glyph_cnt", "bbx_mode", "bits_per_0", "bits_per_1",
    "bits_per_char_width", "bits_per_char_height",
    "bits_per_char_x", "bits_per_char_y", "bits_per_delta_x",
    "max_w", "max_h", "x_offset", "y_offset",
    "ascent_A", "descent_g", "ascent_paren", "descent_paren",
)
HEADER_SIZE = 23  # 17 个单字节字段 + 3 个大端双字节位置

# ASCII 可打印字符总是全量收录（约 95 字），这样中英文能走同一套绘制代码、
# 天然基线对齐。想省 Flash 的话这里是第一个可以砍的地方。
ASCII_FIRST, ASCII_LAST = 0x20, 0x7E

# 黄金字形：解码正确性的判据。
# 只要解码逻辑（位序、游程、归一化位置）有任何一处不对，这几个比对就会失败。
# 数据来自开发时人工肉眼核对过的点阵图。
GOLDEN_GLYPHS = {
    0x4E2D: {  # 中
        "w": 9, "h": 11, "x": 1, "y": -1, "adv": 12,
        "art": [
            "....#....",
            "....#....",
            "#########",
            "#...#...#",
            "#...#...#",
            "#...#...#",
            "#########",
            "#...#...#",
            "....#....",
            "....#....",
            "....#....",
        ],
    },
    ord("A"): {
        "w": 7, "h": 8, "x": 0, "y": 0, "adv": 8,
        "art": [
            "...#...",
            "...#...",
            "..#.#..",
            "..#.#..",
            ".#...#.",
            ".#####.",
            "#.....#",
            "#.....#",
        ],
    },
}


# ───────────────────────── 一、把 C 源码里的字节数组读出来 ─────────────────────────

def load_font_bytes(path, array_name):
    """从 u8g2_fonts.c 里取出那个字体的字节数组。

    数组在 C 源码里是**一串字符串字面量**拼起来的，内容用八进制转义写
    （比如 `\\60` 就是一个字节 0x30）。所以要按 C 的转义规则还原，不能直接
    按字节切片 —— 源码文本的长度和数据的长度完全不是一回事。
    """
    with open(path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()

    marker = array_name + "["
    start = text.find(marker)
    if start < 0:
        raise SystemExit(f"在 {path} 里找不到 {array_name}，请检查 --font-src")

    # 声明形如：const uint8_t NAME[202690] U8G2_FONT_SECTION("NAME") = "...";
    # 从第一个 = 之后开始读字符串字面量
    pos = text.index("=", start) + 1
    out = bytearray()
    simple_escapes = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11,
                      "\\": 92, '"': 34, "'": 39, "?": 63}

    while True:
        while pos < len(text) and text[pos] in " \t\r\n":
            pos += 1
        if pos >= len(text) or text[pos] != '"':
            break  # 字面量读完了（下一个字符是分号）
        pos += 1
        while True:
            ch = text[pos]
            if ch == '"':
                pos += 1
                break
            if ch == "\\":
                nxt = text[pos + 1]
                if nxt in "01234567":  # 八进制：最多三位
                    match = re.match(r"[0-7]{1,3}", text[pos + 1:pos + 4])
                    out.append(int(match.group(0), 8) & 0xFF)
                    pos += 1 + len(match.group(0))
                elif nxt == "x":  # 十六进制 \xHH
                    match = re.match(r"[0-9a-fA-F]{1,2}", text[pos + 2:pos + 4])
                    out.append(int(match.group(0), 16))
                    pos += 2 + len(match.group(0))
                else:
                    out.append(simple_escapes.get(nxt, ord(nxt)))
                    pos += 2
            else:
                code = ord(ch)
                if code > 0xFF:  # 源码里真出现非 ASCII 时的兜底
                    out.extend(ch.encode("utf-8"))
                else:
                    out.append(code)
                pos += 1

    declared = int(text[start + len(marker):text.index("]", start)])
    # C 的字符串字面量末尾隐含一个 \0，而它也算进数组长度。所以实际数据要么
    # 正好等于声明值，要么比它少 1（少的那 1 个就是这个终止符）。少别的数就
    # 说明转义解析写错了，必须停下来。
    if len(out) == declared - 1:
        out.append(0)  # 补上那个隐含的终止符，让数组与 Flash 里的排布完全一致
    elif len(out) != declared:
        raise SystemExit(f"解析出的字节数 {len(out)} 与声明值 {declared} 不符，"
                         "说明转义解析有误，请检查脚本")
    return bytes(out)


# ───────────────────────── 二、解析字体头与字形目录 ─────────────────────────

def parse_font(data):
    """解析 23 字节文件头，返回一个描述这份字体的字典。"""
    info = {name: data[i] for i, name in enumerate(HEADER_FIELDS)}
    # 三个位置是「相对头部之后」的偏移（大端），绝对位置要加上 HEADER_SIZE
    info["start_upper_A"] = (data[17] << 8) | data[18]
    info["start_lower_a"] = (data[19] << 8) | data[20]
    info["start_unicode"] = (data[21] << 8) | data[22]
    return info


def build_directory(data, info):
    """遍历两段字形记录，建立 {码点: 数据偏移} 的索引。

    解析是否正确的判据很硬：**每一段都必须精确落在它该结束的位置上**，
    差一个字节都说明记录格式理解错了。所以这里每一步都做断言。
    """
    directory = {}
    one_byte_end = HEADER_SIZE + info["start_unicode"]

    # 段一：一字节码点（ASCII 与 Latin-1），末尾以 00 00 结束
    pos = HEADER_SIZE
    while pos + 1 < one_byte_end:
        code_point = data[pos]
        if code_point == 0:  # 段终止符
            break
        size = data[pos + 1]
        if size < 2:
            raise SystemExit(f"偏移 {pos} 处的记录 size={size} 不合法")
        directory[code_point] = pos + 2
        pos += size
    if pos + 2 == one_byte_end and data[pos] == 0 and data[pos + 1] == 0:
        pos += 2  # 段终止符
    if pos != one_byte_end:
        raise SystemExit(f"一字节码点段没有精确落在段尾：停在 {pos}，"
                         f"应为 {one_byte_end}（格式理解有误）")

    # 段二：两字节码点（Unicode），末尾同样以 00 00 结束
    pos = one_byte_end
    while pos + 2 < len(data):
        code_point = (data[pos] << 8) | data[pos + 1]
        if code_point == 0:
            break
        size = data[pos + 2]
        if size < 3:
            raise SystemExit(f"偏移 {pos} 处的 Unicode 记录 size={size} 不合法")
        directory[code_point] = pos + 3
        pos += size
    # 剩下应当全是 0：段终止符 + C 字符串字面量隐含的那个 \0
    if any(data[pos:]):
        raise SystemExit(f"Unicode 段之后还有非零数据（偏移 {pos}），格式理解有误")
    return directory


# ───────────────────────── 三、解码单个字形 ─────────────────────────

class BitReader:
    """按位读取器 —— 每字节内**低位在前**，与 u8g2_font.c 完全一致。"""

    def __init__(self, data, pos):
        self.data = data
        self.byte_pos = pos
        self.bit_pos = 0

    def unsigned(self, count):
        if count == 0:
            return 0
        value = self.data[self.byte_pos] >> self.bit_pos
        next_bit_pos = self.bit_pos + count
        if next_bit_pos >= 8:  # 跨字节：把下一个字节的低位接上来
            shift = 8 - self.bit_pos
            self.byte_pos += 1
            value |= self.data[self.byte_pos] << shift
            next_bit_pos -= 8
        value &= (1 << count) - 1
        self.bit_pos = next_bit_pos
        return value

    def signed(self, count):
        """有符号数用的是偏置编码：减掉最高位权重（u8g2_font.c 第 282 行）。"""
        return self.unsigned(count) - (1 << (count - 1))


def decode_glyph(data, offset, info):
    """解出一个字形：返回 (w, h, x, y, adv, 每行的像素位图列表)。

    位图的行里，**最高位那一侧是最左边的像素**（与设备端的约定一致）。
    """
    reader = BitReader(data, offset)
    width = reader.unsigned(info["bits_per_char_width"])
    height = reader.unsigned(info["bits_per_char_height"])
    x = reader.signed(info["bits_per_char_x"])
    y = reader.signed(info["bits_per_char_y"])
    adv = reader.signed(info["bits_per_delta_x"])

    rows = [0] * height
    if width > 0:
        bits_0 = info["bits_per_0"]
        bits_1 = info["bits_per_1"]
        # 画笔坐标：lx 沿宽度走，画满一行就从下一行的行首继续
        lx, ly = 0, 0

        def paint(count, is_ink, lx, ly):
            """画 count 个像素；一行画满折到下一行。与 decode_len 同构。"""
            while count > 0:
                remaining = width - lx
                current = count if count < remaining else remaining
                if is_ink:
                    for i in range(current):
                        if ly < height:
                            rows[ly] |= 1 << (width - 1 - (lx + i))
                if count < remaining:
                    lx += count
                    break
                count -= remaining
                lx = 0
                ly += 1
            return lx, ly

        for _ in range(4096):  # 防御性上限，正常几十次就结束了
            run_a = reader.unsigned(bits_0)
            run_b = reader.unsigned(bits_1)
            while True:
                lx, ly = paint(run_a, False, lx, ly)
                lx, ly = paint(run_b, True, lx, ly)
                if reader.unsigned(1) == 0:
                    break
            if ly >= height:
                break
        else:
            raise SystemExit("字形游程解码没有正常结束，格式理解可能有误")

    return width, height, x, y, adv, rows


# ───────────────────────── 四、放进统一格子 ─────────────────────────

def to_cell(width, height, x, y, rows):
    """把字形放进 12×13 的格子，返回 13 行的 12 位位图。

    横向：格内列 = x（字形的 x 偏移）
    纵向：格内行 = 11 - h - y

    纵向那条公式看着绕，其实就是照抄 u8g2 的绘制规则（字的墨迹盒左上角画在
    「基线 - (h + y)」），再统一把基线摆到格子的第 11 行；这样汉字和 ASCII
    天然共享同一条基线。越界会直接报错退出 —— 宁可停下来，也不要悄悄切掉笔画。
    """
    row0 = (CELL_H - 2) - height - y
    col0 = x
    if row0 < 0 or col0 < 0 or row0 + height > CELL_H or col0 + width > CELL_W:
        raise SystemExit(
            f"字形超出 {CELL_W}×{CELL_H} 格子：w={width} h={height} "
            f"x={x} y={y} → 行 {row0}..{row0 + height - 1}，列 {col0}..{col0 + width - 1}")
    cell = [0] * CELL_H
    for r in range(height):
        # 行位图右对齐到 width 位；左移到格子的目标列
        cell[row0 + r] = rows[r] << (CELL_W - col0 - width)
    return cell


def pack_cell(cell):
    """13 行 × 12 位 → 20 字节，按位连续打包（低位在前，与设备端约定一致）。"""
    out = bytearray(BYTES_PER_GLYPH)
    for r, row_bits in enumerate(cell):
        for c in range(CELL_W):
            if row_bits & (1 << (CELL_W - 1 - c)):  # 格内第 c 列从左往右
                index = r * CELL_W + c
                out[index >> 3] |= 1 << (index & 7)
    return bytes(out)


# ───────────────────────── 五、字集与输出 ─────────────────────────

def read_charset(path):
    """读字集清单：忽略 # 注释，**所有非空白字符都算**（顺序无所谓）。

    这里刻意不按「词」切分：中文本来就不加空格，清单里写成
    「已批准拒绝」或「已批准 拒绝」都应当一样能用。
    """
    code_points = set(range(ASCII_FIRST, ASCII_LAST + 1))
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            for ch in line.split("#", 1)[0]:
                if not ch.isspace():
                    code_points.add(ord(ch))
    return sorted(code_points)


def decode_all(data, info, directory, code_points):
    """把要的字全部解出来，返回 [(码点, 格子位图, 步进宽度)]。"""
    result = []
    missing = []
    for cp in code_points:
        offset = directory.get(cp)
        if offset is None:
            missing.append(cp)
            continue
        width, height, x, y, adv, rows = decode_glyph(data, offset, info)
        result.append((cp, to_cell(width, height, x, y, rows), adv if adv > 0 else CELL_W))
    if missing:
        shown = " ".join(f"{chr(cp)}(U+{cp:04X})" for cp in missing[:20])
        raise SystemExit(f"下列字在这份字体里不存在：{shown}")
    return result


def write_header(path, glyphs, font_src):
    """生成 C 头文件。**故意不写时间戳**，这样重复生成不会产生多余的 diff。"""
    index_rows, adv_rows, bit_rows = [], [], []
    for i, (cp, cell, adv) in enumerate(glyphs):
        index_rows.append(f"0x{cp:04X},")
        adv_rows.append(f"{adv:2d},")
        for byte in pack_cell(cell):
            bit_rows.append(f"0x{byte:02X},")

    def wrap(items, per_line):
        lines = []
        for i in range(0, len(items), per_line):
            lines.append("  " + " ".join(items[i:i + per_line]))
        return "\n".join(lines)

    total_bytes = len(glyphs) * BYTES_PER_GLYPH + len(glyphs) * 3 + 40
    text = f"""// 本文件由 tools/gen_cn_font.py 自动生成，请不要手工修改。
// 重新生成： python tools/gen_cn_font.py
//
// 字体：{FONT_ARRAY_NAME}（文泉驿点阵宋体 12px，取自 U8g2）
// 来源：{os.path.basename(font_src)}
// 字形：{len(glyphs)} 个（ASCII 0x20-0x7E 全量 + tools/cn_charset.txt 里的字）
// 格子：{CELL_W}×{CELL_H} 点阵，每字 {BYTES_PER_GLYPH} 字节，约占 {total_bytes} 字节 Flash
//
// 位图是「13 行 × 12 位」连续打包的位流：第 r 行第 c 列（都从 0 开始、c 从左往右）
// 落在第 r*12+c 位上，字节内低位在前。取一行的做法见下面的 cnFontRow()。

// 注意这个 include guard 的名字：不能叫 CN_FONT_H —— 那是「格子高度」宏的名字，
// 两者撞名的话编译器会警告宏被重定义，而且高度值会把 guard 覆盖掉。
#ifndef VIBEPET_CN_FONT_H
#define VIBEPET_CN_FONT_H

#include <Arduino.h>

#define CN_FONT_W   {CELL_W}
#define CN_FONT_H   {CELL_H}
#define CN_FONT_BPG {BYTES_PER_GLYPH}
#define CN_FONT_COUNT {len(glyphs)}

// 升序码点表（供二分查找）
static const uint16_t cn_font_index[CN_FONT_COUNT] PROGMEM = {{
{wrap(index_rows, 12)}
}};

// 每个字的步进宽度：汉字恒为 12，ASCII 是 5~8
static const uint8_t cn_font_adv[CN_FONT_COUNT] PROGMEM = {{
{wrap(adv_rows, 20)}
}};

// 每个字的位图，顺序与码点表一一对应
static const uint8_t cn_font_bits[CN_FONT_COUNT * CN_FONT_BPG] PROGMEM = {{
{wrap(bit_rows, 16)}
}};

// 取出某个字形第 row 行的 12 位点阵。
//
// ★ 位序约定：返回值的**第 c 位就是第 c 列**（位 0 = 最左边那一列，位 11 = 最右）。
//   画的时候要写成 `(mask >> c) & 1` 或 `mask & (1 << c)`，别写成 `0x800 >> c`
//   —— 那样读的是镜像的列，整个字会左右翻转（这个坑踩过一次）。
//
// 这个函数**故意放在生成的字库里**，而不是写在固件里：这样电脑上的离线测试
// （tools/test_proto.cpp）能拿同一份代码去验证「打包」和「取行」是一致的。
// 曾经这里踩过的另一个坑：参数一度写成 uint8_t，而字库有 {len(glyphs)} 个字
// —— 索引超过 255 的那些字全被截断成了别的字，屏幕上表现是「有的汉字变成了
// 英文字母」。所以下面的参数、偏移计算一律用 16 位以上。
static inline uint16_t cnFontRow(uint16_t glyph, uint8_t row) {{
  const uint8_t *p = &cn_font_bits[(uint32_t)glyph * CN_FONT_BPG];
  uint16_t bit = (uint16_t)row * CN_FONT_W;
  uint16_t byte_index = bit >> 3;   // 12 位一行，任意行最多跨 2 字节
  uint16_t value = pgm_read_byte(p + byte_index) |
                   ((uint16_t)pgm_read_byte(p + byte_index + 1) << 8);
  return (value >> (bit & 7)) & 0x0FFF;
}}

// 按码点查字形序号；查不到返回 -1（调用方会画一个空心方框）
static inline int16_t cnFontFind(uint16_t code_point) {{
  int16_t low = 0, high = CN_FONT_COUNT - 1;
  while (low <= high) {{
    int16_t mid = (int16_t)((low + high) >> 1);
    uint16_t probe = pgm_read_word(&cn_font_index[mid]);
    if (probe == code_point) return mid;
    if (probe < code_point) low = mid + 1; else high = mid - 1;
  }}
  return -1;
}}

#endif  // VIBEPET_CN_FONT_H
"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return total_bytes


# ───────────────────────── 六、自检与覆盖率检查 ─────────────────────────

def glyph_art(rows, width):
    """把位图打成 ASCII 图，给人眼判断（# 是墨，. 是空）。

    宽度必须显式传进来 —— 不能靠位图的最高位去猜，因为整行都不着墨时
    位图是 0，猜出来就是 0 列，图会错位。
    """
    return ["".join("#" if row & (1 << (width - 1 - i)) else "." for i in range(width))
            for row in rows]


def verify(data, info, directory):
    """对照黄金字形逐位比对。这是「解码逻辑没写错」的唯一证据。"""
    failures = 0
    for cp, expect in GOLDEN_GLYPHS.items():
        width, height, x, y, adv, rows = decode_glyph(data, directory[cp], info)
        actual = {
            "w": width, "h": height, "x": x, "y": y, "adv": adv,
            "art": glyph_art(rows, width),
        }
        problems = [key for key in expect if actual[key] != expect[key]]
        name = chr(cp)
        if not problems:
            print(f"  [通过] {name} (U+{cp:04X})  w={width} h={height} "
                  f"x={x} y={y} adv={adv}")
            continue
        failures += 1
        print(f"  [失败] {name} (U+{cp:04X})  不一致的字段：{', '.join(problems)}")
        for key in problems:
            if key == "art":
                print(f"    期望        实际")
                for want, got in zip(expect["art"], actual["art"]):
                    print(f"    {want}   {got}")
                for extra in actual["art"][len(expect["art"]):]:
                    print(f"    {'(无)'.ljust(len(extra))}   {extra}")
            else:
                print(f"    {key}: 期望 {expect[key]}，实际 {actual[key]}")
    return failures


def check_coverage(charset_cps, project_root):
    """检查设备可见的中文文案是否都被字集覆盖。

    hook_client.py 里的中文字符串分两类：发给设备的（必须能被渲染）和只写进
    日志的（不需要）。脚本无法100% 区分，所以这里把「哪些字缺」列出来供人判断，
    真正的硬判据是 tools/cn_charset.txt 本身。
    """
    path = os.path.join(project_root, "pc", "hook_client.py")
    used = set()
    with open(path, encoding="utf-8") as handle:
        tree = ast.parse(handle.read())
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            for ch in node.value:
                if ord(ch) > 0x7F:
                    used.add(ord(ch))

    covered = set(charset_cps)
    absent = sorted(used - covered)
    print(f"hook_client.py 里出现的中文字符：{len(used)} 种")
    print(f"字集收录：{len(covered)} 个码点")
    if not absent:
        print("  [通过] 全部覆盖")
        return 0
    print(f"  [提示] 有 {len(absent)} 个字不在字集里 —— 其中属于**状态小字**的必须补上：")
    grouped = " ".join(f"{chr(cp)}" for cp in absent)
    print(f"    {grouped}")
    print("  （很多只是日志文案，不会显示到屏幕上；对照 hook_client.py 的"
          " SIMPLE_EVENT_MAP 判断即可）")
    return 0


# ───────────────────────── 七、命令行 ─────────────────────────

def main(argv=None):
    # 中文 Windows 的控制台默认按 GBK 解码输出，本脚本要打印汉字点阵图，
    # 所以先把输出流改成 UTF-8（与 pc/ 下几个脚本同一套规矩）。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
        except Exception:
            pass

    parser = argparse.ArgumentParser(description="生成 VibePet 的中文子集字库")
    parser.add_argument("--font-src", default=FONT_SRC_DEFAULT)
    parser.add_argument("--charset", default=CHARSET_DEFAULT)
    parser.add_argument("--out", default=OUT_DEFAULT)
    parser.add_argument("--project-root", default=PROJECT_ROOT)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--verify", action="store_true", help="只做黄金字形自检")
    group.add_argument("--check", action="store_true", help="只做文案覆盖率检查")
    group.add_argument("--dump", metavar="字", help="打印这些字的点阵图（开发用）")
    args = parser.parse_args(argv)

    print(f"读取字体源码：{args.font_src}")
    data = load_font_bytes(args.font_src, FONT_ARRAY_NAME)
    info = parse_font(data)
    directory = build_directory(data, info)
    print(f"  {FONT_ARRAY_NAME}：{len(data)} 字节，"
          f"{len(directory)} 个字形（一字节段 + Unicode 段）")

    if args.dump:
        for ch in args.dump:
            cp = ord(ch)
            width, height, x, y, adv, rows = decode_glyph(data, directory[cp], info)
            print(f"\n{ch} U+{cp:04X}  w={width} h={height} x={x} y={y} adv={adv}")
            for line in glyph_art(rows, width):
                print("  " + line)
            cell = to_cell(width, height, x, y, rows)
            print("  放进 {0}×{1} 格子后：".format(CELL_W, CELL_H))
            for row_bits in cell:
                print("  " + "".join("#" if row_bits & (1 << (CELL_W - 1 - i)) else "."
                                     for i in range(CELL_W)))
        return 0

    if args.verify:
        print("黄金字形自检：")
        failures = verify(data, info, directory)
        print("自检通过。" if not failures else f"自检失败：{failures} 个字形不一致。")
        return 1 if failures else 0

    charset_cps = read_charset(args.charset)
    if args.check:
        return check_coverage(charset_cps, args.project_root)

    glyphs = decode_all(data, info, directory, charset_cps)
    total = write_header(args.out, glyphs, args.font_src)
    print(f"已生成：{args.out}")
    print(f"  {len(glyphs)} 个字 × {BYTES_PER_GLYPH} 字节位图 + 码点表 + 步进表 "
          f"≈ {total} 字节（约占 UNO Flash 的 {total / 32256 * 100:.1f}%）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
