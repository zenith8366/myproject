#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VibePet 串口探针（开发/排障用，不参与正常使用）

━━━━━━━━━━━━━━━ 这个脚本干嘛用的 ━━━━━━━━━━━━━━━

它相当于一根「手动摇柄」：直接打开设备串口，把设备发过来的每一行打印出来，
也允许你手敲一行 JSON 发过去。上机调试时用它就能验证一大半功能，不必先启动
daemon：

    python tools/serial_probe.py COM7

然后会看到一个交互界面（`>` 提示符），可以直接做这些事：

    /state working 正在分析代码     ← 让屏幕切到「工作中」并显示这行中文
    /state needs_you 要删掉缓存了    ← 屏幕变黄 + 响一声
    /approve                        ← 假装用户按了批准按钮（带当前 request_id）
    /deny
    /heartbeat                      ← 发一次心跳
    /raw {"type":"state"}           ← 原样发送（自己写完整 JSON 时用）
    直接敲一行 JSON 再回车也可以，会原样发过去

设备回传的每一行都会带时间戳打印出来（按钮消息就是从这里看到的）。

注意：
  · **打开串口会让开发板复位**，所以启动后要等约 2 秒才收得到东西 —— 本脚本
    会提示，不用慌；
  · 用它之前先停掉 daemon（串口同一时刻只能被一个程序拿着）；
  · 设备 5 秒收不到任何消息就会切成「失联」，所以本脚本**每 2 秒自动替你发一次
    心跳**，否则你边想边敲的时候屏幕早就变橙色了。想亲眼看 LOST 加
    `--no-heartbeat`；
  · 这只是个手工工具，**不参与正常链路**：平时用 `pc/bridge_daemon.py`。
"""

import argparse
import json
import sys
import threading
import time

# 中文 Windows 的控制台默认按 GBK 解码，本脚本要打印中文，先把输出流改成 UTF-8
# （与 pc/ 下几个脚本同一套规矩）。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    except Exception:
        pass

BAUD = 115200
BOOT_GRACE = 2.0
HEARTBEAT_INTERVAL = 2.0   # 设备端 5 秒收不到消息就显示「失联」，2 秒发一次够稳


def log(message):
    print(f"[探针] {message}", file=sys.stderr)


def heartbeat_loop(ser, stop_event, write_lock):
    """后台线程：每 2 秒替用户发一次心跳。

    为什么需要它：设备端 5 秒收不到**任何**消息就会切成橙色的「失联」画面。
    手工调试时你不可能边想边每 5 秒敲一行，于是屏幕上永远只有 LOST —— 想看的
    状态反而看不见。所以由探针自动保活。想亲眼看 LOST 就用 --no-heartbeat 关掉。

    写串口要加锁：这里和主线程都会写，两边的字节一交错，设备就会收到半截报文。
    """
    seq = 0
    while not stop_event.wait(HEARTBEAT_INTERVAL):
        seq += 1
        line = json.dumps({"type": "heartbeat", "seq": seq}) + "\n"
        try:
            with write_lock:
                ser.write(line.encode("utf-8"))
        except Exception:
            return   # 设备掉了，交给读取线程去报错


def reader_loop(ser, stop_event):
    """后台线程：把设备发来的每一行带时间戳打出来。"""
    buffer = b""
    while not stop_event.is_set():
        try:
            chunk = ser.read(ser.in_waiting or 1)
        except Exception as exc:
            if not stop_event.is_set():
                log(f"读取失败（设备掉线了？）：{exc}")
            return
        if not chunk:
            continue
        buffer += chunk
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").strip()
            if text:
                stamp = time.strftime("%H:%M:%S")
                print(f"{stamp} ← {text}")


def send_line(ser, text, write_lock):
    with write_lock:
        ser.write((text + "\n").encode("utf-8"))
    print(f"         → {text}")


def decode_input(raw):
    """把从标准输入读到的**原始字节**解成文字。

    为什么要自己解码、不直接 `for line in sys.stdin`：中文 Windows 的
    sys.stdin 按系统编码（GBK）解码，而你手敲的中文确实是 GBK 字节 —— 但输入
    是从管道/脚本灌进来的时候又常常是 UTF-8，两种都得认。所以先试 UTF-8，
    解不了再退回系统编码兜底。（hook_client.py 处理 stdin 也是同一条规矩：
    绝不因为编码问题把整条消息弄坏或者让程序崩掉。）
    """
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(sys.stdin.encoding or "utf-8", errors="replace")


def build_shortcut(parts, last_request_id):
    """把 /state 这类简写翻译成完整的 JSON 行。看不懂就返回 None。"""
    command = parts[0][1:]
    if command == "state" and len(parts) >= 2:
        return json.dumps({"type": "state", "status": parts[1],
                           "msg": " ".join(parts[2:])}, ensure_ascii=False)
    if command in ("approve", "deny"):
        if not last_request_id[0]:
            log("还没见过 request_id —— 先发一条 /state needs_you 或审批请求")
            return None
        return json.dumps({"type": "button", "request_id": last_request_id[0],
                           "action": command})
    if command == "heartbeat":
        return json.dumps({"type": "heartbeat", "seq": int(time.time())})
    if command == "raw" and len(parts) >= 2:
        return " ".join(parts[1:])
    return None


def main():
    parser = argparse.ArgumentParser(description="VibePet 串口探针（手工调试用）")
    parser.add_argument("port", help="串口号，如 COM7")
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument("--no-heartbeat", action="store_true",
                        help="不自动发心跳（想亲眼看设备切到「失联」LOST 画面时用）")
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        log("未安装 pyserial。安装：pip install pyserial")
        return 1

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=0.5)
    except Exception as exc:
        log(f"打开 {args.port} 失败：{exc}")
        log("检查：线是不是数据线、端口有没有被串口监视器或 daemon 占着")
        return 1

    log(f"已打开 {args.port}（{args.baud}）—— 打开串口会让开发板复位，"
        f"等 {BOOT_GRACE:g} 秒再看输出")
    time.sleep(BOOT_GRACE)

    stop_event = threading.Event()
    write_lock = threading.Lock()
    threading.Thread(target=reader_loop, args=(ser, stop_event), daemon=True).start()

    if not args.no_heartbeat:
        threading.Thread(target=heartbeat_loop,
                         args=(ser, stop_event, write_lock), daemon=True).start()

    print("=" * 60)
    print("直接敲 JSON 回车发送；简写见 /state /approve /deny /heartbeat /raw")
    if args.no_heartbeat:
        print("（已关掉自动心跳：设备 5 秒后会切到 LOST）")
    else:
        print("（已自动每 2 秒发心跳，屏幕不会误显示 LOST）")
    print("Ctrl+C 退出")
    print("=" * 60)

    last_request_id = [None]
    try:
        # 读原始字节再自己解码 —— 见 decode_input 的说明
        for raw_bytes in sys.stdin.buffer:
            text = decode_input(raw_bytes).strip()
            if not text:
                continue
            parts = text.split()
            if text.startswith("/"):
                built = build_shortcut(parts, last_request_id)
                if built is None:
                    log(f"看不懂的简写：{text}")
                    continue
                text = built
            try:
                message = json.loads(text)
                if isinstance(message, dict) and message.get("request_id"):
                    last_request_id[0] = message["request_id"]
            except json.JSONDecodeError:
                pass   # 不是 JSON 也照样发（方便试坏报文）
            send_line(ser, text, write_lock)
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        try:
            ser.close()
        except Exception:
            pass
        log("已退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
