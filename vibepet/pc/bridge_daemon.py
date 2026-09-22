#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VibePet 桥接守护进程（电脑端 · 常驻）

    hook_client.py ──本地 Socket──> bridge_daemon.py ──BLE NUS──> ESP32
                                      （本文件）

职责（设计文档 5.2）：
  1. 独占管理到 ESP32 的 BLE NUS 连接：扫描 / 连接 / 断线自动重连
  2. 每秒发送心跳；设备端 5 s 收不到就切「失联」显示
  3. 接收设备回传的按钮通知，按 request_id 校验后放行决策（F8）
  4. 监听本地 Socket 127.0.0.1:8765，为短生命周期的 hook_client.py 服务

设计文档 5.5 的示例代码缺少重连、并发保护与审批后状态复位，本实现补齐。

用法：
    python pc/bridge_daemon.py              # 正常模式（需要 bleak 与真实设备）
    python pc/bridge_daemon.py --no-ble     # 无硬件开发模式：跳过 BLE，仅跑本地 Socket

--no-ble 模式下可以不用设备就验证完整审批链路：往 8765 发
    {"type":"button","request_id":"<从 status 查询>","action":"approve"}
即可模拟用户按下物理按钮。

环境变量：VIBEPET_HOST / VIBEPET_PORT / VIBEPET_DEBUG，用法同 hook_client.py。
"""

import argparse
import asyncio
import json
import os
import sys
import uuid

# Windows 文本流默认跟随系统 locale（中文系统为 GBK），写非 GBK 字符会抛
# UnicodeEncodeError 直接崩掉常驻进程。与 hook_client.py 保持一致，统一 UTF-8。
# 这里连 stdout 一起改：daemon 的 stdout 不承载任何协议数据（日志全走 stderr），
# 顺手让 `--help` 的中文说明也不乱码。
for _stream in (sys.stdout, sys.stderr):
    try:
        # typeshed 把 sys.stdout 标注成 TextIO 协议（协议里没有 reconfigure），
        # 运行时实际是 TextIOWrapper，该方法一定存在 —— 属类型存根局限，
        # 故 type: ignore；真遇到不支持的流对象由 except 兜底。
        _stream.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore
    except Exception:
        pass

# —— 协议常量（设计文档 3.3，与固件必须一致）——
DEVICE_NAME = "VibePet"
NUS_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  # 电脑 → 设备（Write）
NUS_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  # 设备 → 电脑（Notify）

# —— 时序常量（设计文档 2.2 / 5.4）——
HEARTBEAT_INTERVAL = 1.0   # 心跳间隔；设备端看门狗 5 s
APPROVAL_TIMEOUT = 120.0   # 审批超时后降级为 deny
RECONNECT_DELAY = 2.0      # 断线后重连间隔
SCAN_TIMEOUT = 5.0         # BLE 扫描窗口
REQUEST_TIMEOUT = 10.0     # 本地请求的读取超时（与审批等待无关）
SUMMARY_MAX_LEN = 60       # 命令摘要长度上限（设计文档 5.5）
MAX_FIELD_BYTES = 240      # 单字段（summary/msg）字节上限。固件整行上限 LINE_MAX=512，
                           # 整行固定开销约 85 字节；60 个汉字 = 180 字节，按字节限长
                           # 才能保证整行不被设备端整条丢弃（对方按字节数收行）。

_VERBOSE = False


def log(message):
    """日志一律走 stderr（设计文档 8.3：stdout 只属于 Hook JSON）。"""
    try:
        print(f"[daemon] {message}", file=sys.stderr)
    except Exception:
        pass


def debug(message):
    if _VERBOSE:
        log(message)


def format_error(exc):
    """取异常的可读消息。bleak 把 (message, reason) 打包进 args，
    直接 str() 会把整个元组连 reason 枚举一起打出来。"""
    if exc.args and isinstance(exc.args[0], str):
        return exc.args[0]
    return str(exc)


def _env_number(name, default, cast):
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return cast(raw)
    except ValueError:
        log(f"环境变量 {name}={raw!r} 不是合法数值，回退默认值 {default}")
        return default


# —————————————————————————— 传输层 ——————————————————————————


class BleTransport:
    """真实 BLE 传输层。bleak 的细节全部关在这里，Bridge 只看到 connect / send。"""

    def __init__(self, device_name):
        self.device_name = device_name
        self._client = None
        self._line_handler = None
        self._rx_buffer = b""

    def set_line_handler(self, handler):
        """handler(line: str) —— 每收到一整行就回调一次。"""
        self._line_handler = handler

    @property
    def connected(self):
        client = self._client
        return client is not None and client.is_connected

    async def ensure_connected(self):
        """确保已连接；未连接则扫描并连接。返回是否连通。"""
        if self.connected:
            return True

        # 延迟导入：--no-ble 模式下无需安装 bleak
        from bleak import BleakClient, BleakScanner

        devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
        device = next(
            (d for d in devices if d.name and self.device_name in d.name), None
        )
        if device is None:
            debug(f"未扫描到 {self.device_name}（发现 {len(devices)} 个设备）")
            return False

        client = BleakClient(device, disconnected_callback=self._on_disconnect)
        await client.connect()
        await client.start_notify(NUS_TX_UUID, self._on_notify)
        self._client = client
        self._rx_buffer = b""
        log(f"已连接 {device.name or device.address}")
        return True

    async def send_line(self, text):
        # 用局部变量而不是 self.connected 做前置检查：属性检查无法让类型检查器
        # 窄化 self._client（BleakClient | None），会误报「不是 None 的属性」。
        # 语义与 connected 完全一致，且检查与使用之间引用不会变。
        client = self._client
        if client is None or not client.is_connected:
            raise ConnectionError("BLE 未连接")
        await client.write_gatt_char(NUS_RX_UUID, (text + "\n").encode("utf-8"))
        debug(f"→ 设备: {text}")

    def _on_disconnect(self, client):
        log("BLE 连接断开")
        self._client = None

    def _on_notify(self, sender, data):
        """设备 → 电脑。通知回调运行在事件循环线程里，可直接碰 asyncio 原语。

        按 bytes 累积再按 b"\\n" 切分：UTF-8 多字节字符绝不会与 0x0A 冲突，
        因此跨包分片的字符不会被破坏（若先 decode 再拼接则会解出乱码）。
        """
        self._rx_buffer += data
        while b"\n" in self._rx_buffer:
            line, self._rx_buffer = self._rx_buffer.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").strip()
            if text and self._line_handler:
                self._line_handler(text)


class NullTransport:
    """--no-ble 模式：不做真实 BLE。

    待发送的消息记为日志，按钮事件改由本地 Socket 注入 —— 这样在没有硬件的
    情况下也能把 Socket 协议、request_id 匹配、超时降级整条链路跑通。
    """

    def __init__(self):
        self._line_handler = None

    def set_line_handler(self, handler):
        self._line_handler = handler

    @property
    def connected(self):
        return True

    async def ensure_connected(self):
        return True

    async def send_line(self, text):
        debug(f"[no-ble] → 设备: {text}")


# —————————————————————————— 桥接核心 ——————————————————————————


class Bridge:
    def __init__(self, transport, approval_timeout=APPROVAL_TIMEOUT):
        self.transport = transport
        self.approval_timeout = approval_timeout
        self._pending_request_id = None
        self._button_action = None
        self._button_event = asyncio.Event()
        # 设备「应当」显示的 state（记账：最近一次下发的 (status, msg)，
        # 不论发送成败），用于抑制重复刷新。
        self._last_state = None
        # True = 设备实际显示可能不是 _last_state：发送失败、屏幕被审批卡占据、
        # 或刚重连（屏幕停在 LOST / 设备本地恢复的旧状态）。
        self._state_dirty = False
        # 审批进行中的完整参数。BLE 中断重连后用它重发审批卡，且必须沿用
        # 原 request_id，否则设备回传的按钮对不上（F8）。
        self._pending_approval = None
        # F9：同一时间只处理一个审批请求。并发调用在 handle_approval 里排队，
        # 否则 _pending_request_id 这个单槽会被第二个请求覆盖，
        # 第一个请求就会永远等不到匹配的按钮，直到超时。
        self._approval_lock = asyncio.Lock()
        transport.set_line_handler(self._on_device_line)

    # —— 设备 → 电脑 ——

    def _on_device_line(self, line):
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            log(f"设备消息不是合法 JSON，已忽略: {line[:80]!r}")
            return

        if msg.get("type") != "button":
            log(f"未知的设备消息类型 {msg.get('type')!r}，已忽略")
            return

        action = msg.get("action")
        if action not in ("approve", "deny"):
            log(f"按钮动作非法 {action!r}，已忽略")
            return

        # F8：只接受当前等待中的 request_id。上一次审批超时之后才按下的
        # 「迟到按钮」在这里被丢弃，不会误批准下一次请求。
        if self._pending_request_id is None:
            log(f"当前没有待审批请求，丢弃按钮 {action}")
            return
        if msg.get("request_id") != self._pending_request_id:
            log(f"request_id 不匹配（收到 {msg.get('request_id')!r}，"
                f"等待 {self._pending_request_id!r}），丢弃按钮 {action}")
            return

        self._button_action = action
        self._button_event.set()

    def inject_button(self, msg):
        """把一条 button 消息当作设备回传处理（供 --no-ble 模式与测试使用）。"""
        self._on_device_line(json.dumps(msg, ensure_ascii=False))

    # —— 电脑 → 设备 ——

    async def _send(self, payload):
        if not self.transport.connected:
            raise ConnectionError("BLE 未连接")
        await self.transport.send_line(json.dumps(payload, ensure_ascii=False))

    async def send_state(self, status, msg=""):
        """发送状态。与设备当前显示完全相同时跳过 —— 每条状态都会让固件全屏
        重绘（设计文档 5.5 的 draw* 全是 fillScreen），而 PostToolUse 会把同一个
        working 反复上报，不去重就是每秒闪好几次。

        去重键记的是「设备应显示的内容」而非「已确认送达的内容」：发送失败时
        仍记账并保留 _state_dirty，重连补发靠它拿回正确的一条。
        """
        msg = clamp_bytes(msg, MAX_FIELD_BYTES)
        key = (status, msg)
        if key == self._last_state and not self._state_dirty:
            debug(f"状态与当前显示相同，跳过: {status} / {msg!r}")
            return
        self._last_state = key        # 先记账，再尝试送达
        self._state_dirty = True
        await self._send({"type": "state", "status": status, "msg": msg})
        self._state_dirty = False

    async def send_approval_request(self, request_id, tool, summary):
        await self._send({
            "type": "approval_request",
            "request_id": request_id,
            "tool": tool,
            "summary": clamp_bytes(summary, MAX_FIELD_BYTES),
        })
        # 设备此刻显示的是审批卡而非任何 state。标记屏幕内容与 _last_state
        # 不符，否则审批结束后发回 working 会被去重逻辑误判成「与上次相同」
        # 而静默丢弃，屏幕就永远卡在 APPROVE? 了。
        self._state_dirty = True

    async def resend_current(self):
        """刚重连上时调用：把设备屏幕应有的内容补发一份。

        失联期间设备显示 LOST（心跳恢复后本地会切回最后有效状态），实际
        显示已不可知 —— 所以补发一律绕过去重（置 _state_dirty）。审批进行中
        则重发审批卡，并沿用原 request_id，设备回传的按钮才仍然有效（F8）。
        """
        if self._pending_approval is not None:
            await self.send_approval_request(**self._pending_approval)
            return
        if self._last_state is not None:
            self._state_dirty = True
            await self.send_state(*self._last_state)

    async def wait_for_button(self, request_id, timeout):
        """阻塞等待匹配 request_id 的按钮；超时返回 deny（设计文档 5.4）。"""
        self._pending_request_id = request_id
        self._button_action = None
        self._button_event.clear()
        try:
            await asyncio.wait_for(self._button_event.wait(), timeout=timeout)
            return self._button_action
        except asyncio.TimeoutError:
            log(f"审批超时（{timeout:g} s），降级为 deny")
            return "deny"
        finally:
            self._pending_request_id = None

    # —— 审批主流程 ——

    async def handle_approval(self, hook_input):
        tool_name = str(hook_input.get("tool_name", ""))
        summary = summarize(hook_input.get("tool_input"))
        request_id = uuid.uuid4().hex[:8]

        async with self._approval_lock:
            if not self.transport.connected:
                log(f"BLE 未连接，审批降级为 deny（tool={tool_name}）")
                return {"action": "deny"}

            # 先占槽再发送：按钮校验只认槽里的 request_id，若等 wait_for_button
            # 才写入，请求已发出而槽还空着的窗口里按钮会被误判为「无待审批」丢弃。
            self._pending_request_id = request_id
            self._pending_approval = {
                "request_id": request_id,
                "tool": tool_name,
                "summary": summary,
            }
            try:
                await self.send_approval_request(request_id, tool_name, summary)
            except Exception as exc:
                self._pending_request_id = None
                self._pending_approval = None
                log(f"审批请求发送失败，降级为 deny: {exc}")
                return {"action": "deny"}

            log(f"等待按钮: tool={tool_name} request_id={request_id} summary={summary!r}")
            try:
                action = await self.wait_for_button(request_id, self.approval_timeout)
            finally:
                self._pending_approval = None
            log(f"审批结果: {action} (request_id={request_id})")

            # 让屏幕脱离 APPROVE? 的滞留状态。完整的「事件 → 状态」映射
            # （done / error 等）不在本文件范围内，这里只闭合审批流程。
            # 发送失败不致命：_state_dirty 已置位，重连后 resend_current 会补发。
            try:
                await self.send_state("working" if action == "approve" else "idle")
            except Exception as exc:
                log(f"审批后状态发送失败（重连后将补发）: {exc}")

            return {"action": action}

    def status(self):
        return {
            "connected": self.transport.connected,
            "pending_request_id": self._pending_request_id,
            "approval_timeout": self.approval_timeout,
        }

    # —— 后台任务 ——

    async def heartbeat_loop(self):
        """每秒一次心跳。设备端 5 s 收不到就切「失联」显示。"""
        seq = 0
        while True:
            try:
                if self.transport.connected:
                    seq += 1
                    await self._send({"type": "heartbeat", "seq": seq})
            except Exception as exc:
                log(f"心跳发送失败: {exc}")
            await asyncio.sleep(HEARTBEAT_INTERVAL)

    async def connection_loop(self):
        """维持 BLE 连接：断了就重连，重连失败不退出，一直重试。

        同一个错误只打印一次。蓝牙没开之类的持续故障会每 2 s 重试一轮，
        逐轮刷屏会把真正有用的日志冲掉。
        """
        last_error = None
        while True:
            if not self.transport.connected:
                try:
                    if await self.transport.ensure_connected():
                        # 刚从断开变为连上：设备屏幕还停在 LOST（或它本地恢复的
                        # 旧状态），把「现在应该显示什么」重新推过去。
                        try:
                            await self.resend_current()
                        except Exception as exc:
                            log(f"重连后补发状态失败: {exc}")
                    last_error = None
                except Exception as exc:
                    message = format_error(exc)
                    if message != last_error:
                        log(f"BLE 连接失败: {message}")
                        last_error = message
                    else:
                        debug(f"BLE 连接失败（与上次相同）: {message}")
            await asyncio.sleep(1.0 if self.transport.connected else RECONNECT_DELAY)


# —————————————————————————— 摘要提取 ——————————————————————————


def clamp_bytes(text, max_bytes):
    """按 UTF-8 边界把文本截到不超过 max_bytes 字节。

    设备的行长上限按「字节」计（固件 LINE_MAX），而 Python 切片按码点：
    一个汉字 3 字节，只按字符数限长会放任整行超限、被设备整条丢弃。
    errors="ignore" 恰好丢掉落在边界上的半个多字节字符，不会切出乱码。
    """
    raw = text.encode("utf-8")
    if len(raw) <= max_bytes:
        return text
    return raw[:max_bytes].decode("utf-8", errors="ignore")


def collapse(text):
    """折掉换行与连续空白 —— 屏幕上只有一行位置，多行摘要会溢出。"""
    return " ".join(str(text).split())


def summarize(tool_input):
    """把 tool_input 压成一行短摘要给屏幕显示。

    按「人能一眼看懂」的优先级取字段：命令 > 路径 > 模式 > URL，
    都没有就退化成整个 tool_input 的字符串形式。
    """
    if not isinstance(tool_input, dict):
        return clamp_bytes(collapse(tool_input)[:SUMMARY_MAX_LEN], MAX_FIELD_BYTES)

    for key in ("command", "file_path", "path", "pattern", "url", "query"):
        value = tool_input.get(key)
        if isinstance(value, str) and value.strip():
            return clamp_bytes(collapse(value)[:SUMMARY_MAX_LEN], MAX_FIELD_BYTES)

    return clamp_bytes(collapse(tool_input)[:SUMMARY_MAX_LEN], MAX_FIELD_BYTES)


# —————————————————————————— 本地 Socket 服务 ——————————————————————————


async def dispatch(bridge, req):
    """本地请求分发。

    hook_client.py 发来的是原始 Hook JSON（没有 type 字段），按审批请求处理；
    其余 type 供调试与后续的状态 Hook 使用。
    """
    msg_type = req.get("type", "approval")

    if msg_type == "status":
        return bridge.status()

    if msg_type == "button":
        # 供 --no-ble 模式与测试注入按钮事件。真实模式下按钮走 BLE 通知，
        # 但这里仍接受 —— 8765 只监听回环地址，能连上它的进程本就能做更多事。
        bridge.inject_button(req)
        return {"ok": True}

    if msg_type == "state":
        await bridge.send_state(req.get("status", "idle"), req.get("msg", ""))
        return {"ok": True}

    if msg_type == "approval":
        return await bridge.handle_approval(req)

    return {"error": f"未知请求类型 {msg_type!r}"}


async def handle_client(bridge, reader, writer):
    # 兜底初值：except Exception 接不住 BaseException（如任务取消），
    # 那条路径下 finally 里读未赋值的 resp 会抛 UnboundLocalError。
    resp = {"error": "请求处理被中断"}
    try:
        try:
            raw = await asyncio.wait_for(reader.readline(), timeout=REQUEST_TIMEOUT)
        except asyncio.TimeoutError:
            raise ValueError(f"未在 {REQUEST_TIMEOUT:g} s 内收到完整请求")

        if not raw.strip():
            raise ValueError("收到空请求")

        try:
            req = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            raise ValueError(f"请求不是合法的 UTF-8 JSON: {exc}")

        resp = await dispatch(bridge, req)
    except Exception as exc:
        log(f"处理本地请求失败: {exc}")
        resp = {"error": str(exc)}
    finally:
        try:
            writer.write((json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8"))
            await writer.drain()
        except Exception:
            pass
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


# —————————————————————————— 入口 ——————————————————————————


async def amain(args):
    global _VERBOSE
    _VERBOSE = args.verbose

    if args.no_ble:
        log("以 --no-ble 模式启动：不连接 BLE，按钮事件可通过本地 Socket 注入")
        transport = NullTransport()
    else:
        # 先探测依赖。否则 connection_loop 会每 2 s 重试一次并刷一行
        # 「No module named 'bleak'」，看似在工作实则永远连不上。
        try:
            import bleak  # noqa: F401
        except ImportError:
            log("未安装 bleak，无法连接 BLE。安装：pip install bleak")
            log("若只想调试本地链路（不需要硬件），请加 --no-ble")
            return 1
        transport = BleTransport(args.device_name)

    bridge = Bridge(transport, args.timeout)

    tasks = [
        asyncio.create_task(bridge.heartbeat_loop()),
        asyncio.create_task(bridge.connection_loop()),
    ]

    server = await asyncio.start_server(
        lambda r, w: handle_client(bridge, r, w), args.host, args.port
    )
    log(f"本地 Socket 监听 {args.host}:{args.port}")

    try:
        async with server:
            await server.serve_forever()
    finally:
        for task in tasks:
            task.cancel()


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="VibePet 桥接守护进程：BLE 连接管理 + 本地审批 Socket 服务",
    )
    parser.add_argument("--no-ble", action="store_true",
                        help="跳过 BLE，仅运行本地 Socket（无硬件开发/测试用）")
    parser.add_argument("--host", default=os.environ.get("VIBEPET_HOST", "127.0.0.1"),
                        help="本地 Socket 监听地址（默认 127.0.0.1）")
    parser.add_argument("--port", type=int,
                        default=_env_number("VIBEPET_PORT", 8765, int),
                        help="本地 Socket 监听端口（默认 8765）")
    parser.add_argument("--device-name", default=DEVICE_NAME,
                        help=f"BLE 广播名（默认 {DEVICE_NAME}）")
    parser.add_argument("--timeout", type=float,
                        default=_env_number("VIBEPET_TIMEOUT", APPROVAL_TIMEOUT, float),
                        help=f"审批超时秒数，超时降级为 deny（默认 {APPROVAL_TIMEOUT:g}）")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="输出调试日志（含每条收发报文）")
    return parser.parse_args(argv)


def main():
    args = parse_args()
    try:
        return asyncio.run(amain(args)) or 0
    except KeyboardInterrupt:
        log("已退出")
        return 0


if __name__ == "__main__":
    sys.exit(main())
