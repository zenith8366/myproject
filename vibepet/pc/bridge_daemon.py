#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VibePet 桥接守护进程（电脑端 · 常驻）

━━━━━━━━━━━━━━━ 新手导读：这个文件是干嘛的 ━━━━━━━━━━━━━━━

它是整个系统的「中间人」—— 一个**常驻**（开着就不关）的程序。为什么需要它？

因为蓝牙有两条硬规矩：
  · 连接建立很慢，扫描 + 连接要好几秒；
  · 同一条连接同时只能有一个程序「拿着」。

而 Claude Code 那边每发生一件事，就会新起一个短命进程（hook_client.py），
办完事就退。要是让那种短命进程自己去连蓝牙 —— 每次都得等好几秒，而且几个
进程还会互相抢连接。所以分工如下：

    hook_client.py ──本地 Socket──> bridge_daemon.py ──BLE 蓝牙──> ESP32 设备
     （短命，随叫随到）              （常驻，独占蓝牙）              （桌上的小屏）

本进程主要干四件事：
  1. 独占管理蓝牙连接：找设备、连上、断了自动重连；
  2. 每秒给设备发一次「心跳」，让它知道电脑还活着 —— 设备那边 5 秒收不到
     就显示「失联」，这样蓝牙悄悄断了你也能一眼看出来；
  3. 收设备传回来的按钮消息，核对 request_id 之后把决定交还给 hook_client；
  4. 开一个本地端口（127.0.0.1:8765），听 hook_client 来喊话。

**第一次读，建议按这个顺序**：
  ① 先扫一眼下面的「常量区」，知道有哪些可调的参数；
  ② 再看 Bridge 类 —— 它是核心，审批逻辑都在那儿；
  ③ 然后看两个后台循环 heartbeat_loop / connection_loop，理解它怎么守住连接；
  ④ 最后看底部的 dispatch / handle_client / amain，那是「接待客人」的部分。

───────────────── 以下为技术细节 ─────────────────

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

# ─────────────── 先解决编码问题（和 hook_client.py 同款）───────────────
#
# 中文 Windows 上 Python 默认按 GBK 写字，遇到 GBK 装不下的字符就抛
# UnicodeEncodeError、当场崩掉。本进程是常驻的，崩了就得重新启动，所以
# 这里把两个输出流都改成 UTF-8。
#
# 顺手把 stdout 也改了：本进程的 stdout 不承载任何协议数据（日志全在
# stderr），改了之后 `--help` 里的中文说明也不会变成乱码。
for _stream in (sys.stdout, sys.stderr):
    try:
        # typeshed 把 sys.stdout 标注成 TextIO 协议（协议里没有 reconfigure），
        # 运行时实际是 TextIOWrapper，该方法一定存在 —— 属类型存根局限，
        # 故 type: ignore；真遇到不支持的流对象由 except 兜底。
        _stream.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore
    except Exception:
        pass

# ─────────────── 协议常量：改这里必须两端一起改 ───────────────
#
# 下面几个是「和设备说好的暗号」。电脑端和固件必须一字不差，
# 差一个字符设备就听不懂我们在说什么（设计文档 3.3）。

# 设备广播自己时用的名字。扫描时按这个名字找它。
DEVICE_NAME = "VibePet"

# Nordic UART Service 的三个标准 UUID（这是蓝牙串口的通用约定，不是本项目
# 发明的）。一个 Service 下面挂两条「管道」：RX 用来发，TX 用来收。
NUS_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  # 电脑 → 设备（Write）
NUS_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  # 设备 → 电脑（Notify）

# ─────────────── 时序常量：几个「等多久」的决定 ───────────────
#
# 这一组数值彼此有关系，改一个要顺手想想另一个（设计文档 2.2 / 5.4）。

# 心跳间隔：每秒给设备打个招呼。
# 为什么是 1 秒？因为设备那边的看门狗是 5 秒 —— 每秒一次意味着哪怕连续
# 丢 4 个包也不会被误判成失联，够稳。
HEARTBEAT_INTERVAL = 1.0

# 等用户按按钮的最长时间；超时按拒绝处理。
APPROVAL_TIMEOUT = 120.0

# 蓝牙断了之后，隔多久重试一次连接。
RECONNECT_DELAY = 2.0

# 每次扫描设备时，听多久的广播。
SCAN_TIMEOUT = 5.0

# 读一条本地请求最多等多久。注意这和「等按钮」是两码事 ——
# 它只管「请求有没有发完整」，发完整之后才进入漫长的等按钮阶段。
REQUEST_TIMEOUT = 10.0

# 命令摘要先截到 60 个字符，再按字节数把关（见下一行）。
SUMMARY_MAX_LEN = 60

# 单个字段（summary / msg）的字节上限：240 字节。
#
# 为什么要按「字节」而不是「字数」来限？因为设备是按字节数收行的：整行
# 超过 512 字节就整条丢掉。而一个汉字在 UTF-8 里占 3 个字节，60 个汉字
# 就是 180 字节 —— 按字数限长会放任它悄悄超限、然后被设备整条扔掉。
# （固件整行上限 LINE_MAX=512，整行的固定开销约 85 字节，240 是留足余量的值。）
MAX_FIELD_BYTES = 240

# 当前是不是「啰嗦模式」（命令行加了 -v）。开着才打调试日志。
_VERBOSE = False


# 打日志（给人看的）。规矩和 hook_client.py 一样：一律走 stderr。
# 本进程的 stdout 其实不承载协议，但保持同一条规矩不容易出错。
def log(message):
    """日志一律走 stderr（设计文档 8.3：stdout 只属于 Hook JSON）。"""
    try:
        print(f"[daemon] {message}", file=sys.stderr)
    except Exception:
        pass


# 调试日志：要加 -v 才看得见，平时静默。
def debug(message):
    if _VERBOSE:
        log(message)


# 把异常变成一句人话。
#
# 为什么要专门写这么个小函数？因为 bleak 抛异常时会把「消息 + 原因枚举」
# 打包成元组塞进 args，直接 str(exc) 会把整个元组连枚举一起打出来，
# 又长又难读。这里只取第一段文本。
def format_error(exc):
    """取异常的可读消息。bleak 把 (message, reason) 打包进 args，
    直接 str() 会把整个元组连 reason 枚举一起打出来。"""
    if exc.args and isinstance(exc.args[0], str):
        return exc.args[0]
    return str(exc)


# 读环境变量里的数字（同 hook_client.py）：读不懂就用默认值，别崩。
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
#
# 「传输层」的意思是：只管「怎么把一句话送过去」，不管送的是什么内容。
# 把蓝牙的琐碎细节全关在这一节里，上面的 Bridge 类就能干净地只操心审批逻辑；
# 以后想换通信方式（比如改走串口）也只动这一层。


class BleTransport:
    """真实 BLE 传输层。bleak 的细节全部关在这里，Bridge 只看到 connect / send。"""

    def __init__(self, device_name):
        self.device_name = device_name   # 要找的设备叫什么
        self._client = None              # bleak 的连接对象；没连上时是 None
        self._line_handler = None        # 收到一整行时调的回调（由 Bridge 注册）
        self._rx_buffer = b""            # 收数据的缓冲区，见 _on_notify

    def set_line_handler(self, handler):
        """handler(line: str) —— 每收到一整行就回调一次。"""
        self._line_handler = handler

    @property
    def connected(self):
        # 写成 @property 是为了让外面用 transport.connected（不带括号），
        # 读起来像在问一个「状态」，而不是调一个「动作」。
        client = self._client
        return client is not None and client.is_connected

    async def ensure_connected(self):
        """确保已连接；未连接则扫描并连接。返回是否连通。"""
        # 已经连着就什么都不做。这个函数会被后台循环反复调用，
        # 「有则跳过」让它能安心地一直在循环里调下去。
        if self.connected:
            return True

        # 延迟导入：bleak 只有真实 BLE 模式才用得上。放在函数体里 import，
        # 用 --no-ble 模式时就不必安装它。
        from bleak import BleakClient, BleakScanner

        # 听 SCAN_TIMEOUT 秒的广播，再从听到的设备里挑名字对得上的。
        # 用「包含」而不是「完全相等」来匹配，因为广播名可能带后缀。
        devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
        device = next(
            (d for d in devices if d.name and self.device_name in d.name), None
        )
        if device is None:
            debug(f"未扫描到 {self.device_name}（发现 {len(devices)} 个设备）")
            return False   # 没找到不算错误，交给上层过一会儿再试

        # 连上，并订阅「设备主动推消息」的通知（Notify）—— 设备回传的按钮
        # 消息就是从这条通道来的。
        client = BleakClient(device, disconnected_callback=self._on_disconnect)
        await client.connect()
        await client.start_notify(NUS_TX_UUID, self._on_notify)
        self._client = client
        self._rx_buffer = b""    # 新连接，清掉上一轮可能残留的半截数据
        log(f"已连接 {device.name or device.address}")
        return True

    async def send_line(self, text):
        # 用局部变量而不是 self.connected 做前置检查：属性检查无法让类型检查器
        # 窄化 self._client（BleakClient | None），会误报「不是 None 的属性」。
        # 语义与 connected 完全一致，且检查与使用之间引用不会变。
        client = self._client
        if client is None or not client.is_connected:
            raise ConnectionError("BLE 未连接")
        # 协议规定「一条消息一行」，所以补上换行符再发。
        await client.write_gatt_char(NUS_RX_UUID, (text + "\n").encode("utf-8"))
        debug(f"→ 设备: {text}")

    def _on_disconnect(self, client):
        # 这是注册给 bleak 的回调：连接掉了它会来喊一声。
        # 我们只把引用清掉，重连的事由 connection_loop 操心。
        log("BLE 连接断开")
        self._client = None

    def _on_notify(self, sender, data):
        """设备 → 电脑。通知回调运行在事件循环线程里，可直接碰 asyncio 原语。

        按 bytes 累积再按 b"\\n" 切分：UTF-8 多字节字符绝不会与 0x0A 冲突，
        因此跨包分片的字符不会被破坏（若先 decode 再拼接则会解出乱码）。
        """
        # 蓝牙一次推来的数据不保证「恰好一整行」—— 可能半行，也可能两行半。
        # 所以先把字节攒起来，再从头找换行符，切出一条算一条。
        #
        # 关键细节：**先按字节攒、切分完再解码**。若反过来（先解码再拼接），
        # 某个汉字的 3 个字节要是被拆进两个包里，就会解出乱码。而按字节处理
        # 时，换行符 0x0A 永远不会出现在一个 UTF-8 多字节字符的内部，
        # 所以这样切一定安全。
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

    # 注意：它的「接口」和 BleTransport 一模一样（同样有 connected /
    # ensure_connected / send_line / set_line_handler），所以 Bridge 类根本
    # 分不出自己手里拿的是哪一个 —— 这正是「传输层」这层抽象的意义：
    # 换一个实现，上层一行都不用改。
    def __init__(self):
        self._line_handler = None

    def set_line_handler(self, handler):
        self._line_handler = handler

    @property
    def connected(self):
        return True      # 假装永远连着，反正也没有真设备

    async def ensure_connected(self):
        return True

    async def send_line(self, text):
        # 真蓝牙没了，就把「本来要发什么」打进日志，方便对着调试。
        debug(f"[no-ble] → 设备: {text}")


# —————————————————————————— 桥接核心 ——————————————————————————
#
# Bridge 是「大脑」：审批怎么走、状态怎么同步、断了怎么补，规则都在这儿。
# 它不碰蓝牙细节（那是传输层的事），也不碰网络细节（那是下面 Socket 那节的事），
# 只对着两件事写逻辑：往设备发一条、收到设备一条。


class Bridge:
    def __init__(self, transport, approval_timeout=APPROVAL_TIMEOUT):
        # transport 就是上面那个「传输层」—— 真蓝牙或空壳都行，反正接口一样。
        self.transport = transport
        self.approval_timeout = approval_timeout

        # ── 等按钮用的一套零件 ──
        # 同一时刻只允许有一个审批在等（需求 F9 的「单槽」设计）。
        # _pending_request_id 就是那个槽：里面存着当前在等回应的请求编号。
        self._pending_request_id = None
        # 按钮按下的结果先存这儿，稍后由等待方取走。
        self._button_action = None
        # asyncio 的「事件」，可以理解成门铃：等按钮的代码 await 它，
        # 按钮来了就 set() 一下把人叫醒。
        self._button_event = asyncio.Event()

        # ── 「屏幕该显示什么」的记账 ──
        # 设备「应当」显示的 state（记账：最近一次下发的 (status, msg)，
        # 不论发送成败），用于抑制重复刷新。
        self._last_state = None
        # True = 设备实际显示可能不是 _last_state：发送失败、屏幕被审批卡占据、
        # 或刚重连（屏幕停在 LOST / 设备本地恢复的旧状态）。
        self._state_dirty = False

        # ── 审批进行中的存档 ──
        # 审批进行中的完整参数。BLE 中断重连后用它重发审批卡，且必须沿用
        # 原 request_id，否则设备回传的按钮对不上（F8）。
        self._pending_approval = None

        # ── 两把锁（新手可以先把它们理解成「排队」，细节以后再深究）──
        # F9：同一时间只处理一个审批请求。并发调用在 handle_approval 里排队，
        # 否则 _pending_request_id 这个单槽会被第二个请求覆盖，
        # 第一个请求就会永远等不到匹配的按钮，直到超时。
        self._approval_lock = asyncio.Lock()
        # 串行化所有「会改变屏幕内容」的发送。send_state / send_approval_request
        # 都是「读改写 _state_dirty + await 发送」的组合，并发交错时后一个会把
        # 前一个设的脏标记覆盖掉（重连要补发的那一条就此丢失）。锁的嵌套顺序
        # 固定为 _approval_lock → _send_lock，没有反向路径，不会死锁。
        self._send_lock = asyncio.Lock()

        # 把「收到设备一整行」的回调交给传输层 —— 以后设备发消息过来，
        # 传输层就会来喊 _on_device_line。
        transport.set_line_handler(self._on_device_line)

    # —— 设备 → 电脑 ——
    #
    # 这条方向上跑的是「用户按了哪个按钮」。

    def _on_device_line(self, line):
        # 第一步：能解析成 JSON 吗？不能就丢掉 —— 绝不因为一条坏消息把进程带崩。
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            log(f"设备消息不是合法 JSON，已忽略: {line[:80]!r}")
            return

        # 设备固件只发对象，但这条链路是攻击面之外的外部输入（BLE 对端可能是
        # 任何东西）。非对象 JSON 会让下面的 .get() 抛 AttributeError —— 它虽被
        # asyncio 的异常处理器兜住不至于崩进程，却会留下未处理异常并丢掉这一行。
        if not isinstance(msg, dict):
            log(f"设备消息不是 JSON 对象（{type(msg).__name__}），已忽略: {line[:80]!r}")
            return

        # 第二步：是「按钮」这一类消息吗？
        if msg.get("type") != "button":
            log(f"未知的设备消息类型 {msg.get('type')!r}，已忽略")
            return

        # 第三步：动作合法吗？我们只认批准 / 拒绝两种。
        action = msg.get("action")
        if action not in ("approve", "deny"):
            log(f"按钮动作非法 {action!r}，已忽略")
            return

        # 第四步（最关键）：这个按钮是「这一次审批」的吗？
        #
        # F8：只接受当前等待中的 request_id。上一次审批超时之后才按下的
        # 「迟到按钮」在这里被丢弃，不会误批准下一次请求。
        #
        # 想象这个场景：命令早就超时作废了，你去泡了杯咖啡回来，顺手按了一下
        # 按钮 —— 要是没有这道校验，这根「迟到的手指」就会批准掉下一条你
        # 根本没看过的命令。
        if self._pending_request_id is None:
            log(f"当前没有待审批请求，丢弃按钮 {action}")
            return
        if msg.get("request_id") != self._pending_request_id:
            log(f"request_id 不匹配（收到 {msg.get('request_id')!r}，"
                f"等待 {self._pending_request_id!r}），丢弃按钮 {action}")
            return

        # 全过了：记下结果，按门铃（正在 await 的那段代码会被唤醒）。
        self._button_action = action
        self._button_event.set()

    def inject_button(self, msg):
        """把一条 button 消息当作设备回传处理（供 --no-ble 模式与测试使用）。"""
        # 没有真设备时怎么测按钮？靠它：把一条假消息塞进同一条处理链路，
        # 走的是和真设备完全一样的代码。
        self._on_device_line(json.dumps(msg, ensure_ascii=False))

    # —— 电脑 → 设备 ——
    #
    # 这个方向上跑的是「屏幕该显示什么」。

    async def _send(self, payload):
        # 所有发给设备的消息都从这儿出去：统一检查连接、统一转成 JSON 文本。
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
        # 先把内容压进协议允许的长度（按字节算，见 clamp_bytes）。
        msg = clamp_bytes(msg, MAX_FIELD_BYTES)
        # 用一个二元组 (状态, 小字) 当这次要显示的内容的「身份证」。
        key = (status, msg)
        async with self._send_lock:
            # 去重判断必须和下面两行记账待在同一个临界区里，否则两次并发调用
            # 会双双通过判断，把同一条状态发两遍。
            #
            # 为什么要去重？因为设备每收到一条状态就整屏重绘一次。而 Claude Code
            # 会把「正在干活」这个状态反复上报，不去重屏幕就每秒闪好几次。
            if key == self._last_state and not self._state_dirty:
                debug(f"状态与当前显示相同，跳过: {status} / {msg!r}")
                return
            # 注意顺序：先「记账」再「发送」，而不是等发成功了才记。
            # 因为这两行之间的 await 会让出控制权、别的代码可能插进来。
            # _state_dirty=True 的意思是「账本可能不准了，屏幕未必显示着
            # _last_state」—— 万一这次发送失败，这个标记就留着，重连时
            # resend_current() 看到它会重新补发一遍。
            self._last_state = key        # 先记账，再尝试送达
            self._state_dirty = True
            await self._send({"type": "state", "status": status, "msg": msg})
            self._state_dirty = False     # 发送成功，账本和屏幕又一致了

    async def send_approval_request(self, request_id, tool, summary):
        # 发一张「审批卡」给设备（屏幕上那个黄框 APPROVE? 加一行命令）。
        async with self._send_lock:
            await self._send({
                "type": "approval_request",
                "request_id": request_id,   # 设备回传按钮时要把这个原样带回来
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
        # 情况一：正在等按钮 —— 那就把审批卡原样再发一遍。注意 request_id
        # 必须沿用原来那个，否则设备回传的按钮会因为「编号对不上」被丢掉。
        if self._pending_approval is not None:
            await self.send_approval_request(**self._pending_approval)
            return
        # 情况二：没在等按钮 —— 把最后一次下发的状态重发一遍。
        # 先把 _state_dirty 置上，是为了绕过 send_state 里的去重判断：
        # 刚重连时屏幕显示什么我们并不确定（可能还是大大的 LOST），
        # 必须无条件补发一次。
        if self._last_state is not None:
            self._state_dirty = True
            await self.send_state(*self._last_state)

    async def wait_for_button(self, request_id, timeout):
        """阻塞等待匹配 request_id 的按钮；超时返回 deny（设计文档 5.4）。

        调用方必须**先**占好槽并清空按钮状态 —— 见 handle_approval 里那个同步块。
        本函数只管等待与超时；request_id 用来确认 finally 里清的是自己的槽。
        """
        try:
            # 这一行就是「卡住等按钮」的全部秘密：await 这个事件，一直等到
            # _on_device_line 里 set() 把它点亮，或者等到超时放弃。
            # asyncio.wait_for 负责「到点还没等到就走人」。
            await asyncio.wait_for(self._button_event.wait(), timeout=timeout)
            return self._button_action
        except asyncio.TimeoutError:
            # 等了 timeout 秒也没人按 —— 按拒绝处理。这是贯穿全项目的原则：
            # 拿不准就拒绝。
            log(f"审批超时（{timeout:g} s），降级为 deny")
            return "deny"
        finally:
            # finally 的意思是「不管上面怎么结束，这里都走一遍」——
            # 把槽腾出来，好让下一次审批能用。
            # 只清自己的槽。审批已被 _approval_lock 串行化，理论上不会遇到别人
            # 的槽，但按 id 确认一下总比无条件清空安全。
            if self._pending_request_id == request_id:
                self._pending_request_id = None

    # —— 审批主流程 ——

    # 一次完整审批的全过程：发卡 → 等按钮 → 收尾。这是整个文件的中心。
    async def handle_approval(self, hook_input):
        # hook_input 是 Claude Code 给的那份原始事件（由 hook_client 原样转来）。
        tool_name = str(hook_input.get("tool_name", ""))
        summary = summarize(hook_input.get("tool_input"))
        # 给这次审批发一个随机编号。设备回传按钮时必须原样带回来，我们靠它
        # 分辨「这个按钮属于哪次审批」—— 见 _on_device_line 的第四步。
        request_id = uuid.uuid4().hex[:8]

        # 在这一行排队：同一时刻只跑一个审批（F9）。第二个请求会等在这儿，
        # 直到第一个彻底走完。
        async with self._approval_lock:
            # 没蓝牙就没什么可等的了，直接拒绝。
            if not self.transport.connected:
                log(f"BLE 未连接，审批降级为 deny（tool={tool_name}）")
                return {"action": "deny"}

            # 先占槽再发送：按钮校验只认槽里的 request_id，若等 wait_for_button
            # 才写入，请求已发出而槽还空着的窗口里按钮会被误判为「无待审批」丢弃。
            #
            # 清空上一轮的按钮残留也必须在**这个同步块里**做（到下面 await 之间
            # 没有让出点）：按钮只可能在槽占好之后到达，这里清完就不会被更晚到达
            # 的按钮抢跑。若把清理挪进 wait_for_button，从占槽到那里之间到达的
            # 按钮会被 clear() 抹掉，这次审批就只能干等到超时。
            self._pending_request_id = request_id
            self._button_action = None
            self._button_event.clear()
            # 存一份完整参数 —— 万一发送途中蓝牙断了，重连后靠它把卡重发一遍。
            self._pending_approval = {
                "request_id": request_id,
                "tool": tool_name,
                "summary": summary,
            }
            try:
                await self.send_approval_request(request_id, tool_name, summary)
            except Exception as exc:
                # 卡都没发出去，再等下去也没意义。把占的槽清干净，拒绝。
                self._pending_request_id = None
                self._pending_approval = None
                log(f"审批请求发送失败，降级为 deny: {exc}")
                return {"action": "deny"}

            log(f"等待按钮: tool={tool_name} request_id={request_id} summary={summary!r}")
            # ↓ 就是这一行，整个进程会停在这儿等你按按钮（或等超时）。
            try:
                action = await self.wait_for_button(request_id, self.approval_timeout)
            finally:
                # 审批结束了，把存档清掉 —— 否则重连时会把这张过期的卡又发一遍。
                self._pending_approval = None
            log(f"审批结果: {action} (request_id={request_id})")

            # 让屏幕脱离 APPROVE? 的滞留状态。完整的「事件 → 状态」映射
            # （done / error 等）不在本文件范围内，这里只闭合审批流程。
            # 发送失败不致命：_state_dirty 已置位，重连后 resend_current 会补发。
            try:
                await self.send_state("working" if action == "approve" else "idle")
            except Exception as exc:
                log(f"审批后状态发送失败（重连后将补发）: {exc}")

            # 把结果交回给调用方（dispatch），最终一路回到 hook_client。
            return {"action": action}

    def status(self):
        # 给调试用的「现在什么情况」快照。--no-ble 模式下想手动模拟按钮，
        # 就得先问它要 pending_request_id（见文件开头的用法说明）。
        return {
            "connected": self.transport.connected,
            "pending_request_id": self._pending_request_id,
            "approval_timeout": self.approval_timeout,
        }

    # —— 后台任务 ——
    #
    # 下面两个循环在程序启动时被派出去，之后一直自己跑，不用谁去叫。

    async def heartbeat_loop(self):
        """每秒一次心跳。设备端 5 s 收不到就切「失联」显示。"""
        seq = 0
        while True:
            try:
                if self.transport.connected:
                    seq += 1
                    await self._send({"type": "heartbeat", "seq": seq})
            except Exception as exc:
                # 发心跳失败不是世界末日（可能蓝牙正在断），记一笔就好。
                # 关键是别让异常把循环打断 —— 循环一断就再也不会发心跳了。
                log(f"心跳发送失败: {exc}")
            # 睡一秒再发下一次。注意这句在 try 外面：就算上面出错也要等一秒，
            # 否则出错时会变成疯狂重试、刷屏。
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
                        # 刚从断开变成连上：设备屏幕还停在 LOST（或它自己恢复的
                        # 旧状态），把「现在到底该显示什么」重新推过去。
                        try:
                            await self.resend_current()
                        except Exception as exc:
                            log(f"重连后补发状态失败: {exc}")
                    # 连上了就清掉错误记录，这样下次再断会有新的日志。
                    last_error = None
                except Exception as exc:
                    # 同一句话只喊一遍：蓝牙没开时每 2 秒失败一次，每次都打印
                    # 会把真正有用的日志冲没。
                    message = format_error(exc)
                    if message != last_error:
                        log(f"BLE 连接失败: {message}")
                        last_error = message
                    else:
                        debug(f"BLE 连接失败（与上次相同）: {message}")
            # 连着的时候每秒瞄一眼就行；断着的时候隔 2 秒再试一次 ——
            # 别把 CPU 和蓝牙适配器逼疯。
            await asyncio.sleep(1.0 if self.transport.connected else RECONNECT_DELAY)


# —————————————————————————— 摘要提取 ——————————————————————————
#
# 设备屏幕只有 160 像素宽，一行塞不下几个字。所以得把发过去的命令压成
# 一行短摘要 —— 这一节负责把 Claude Code 给的复杂 tool_input 揉成一句话。


# 把一段文字截短，但不许超过 max_bytes 个**字节**。
#
# 为什么强调「字节」？因为设备是按字节数收行的（超 512 字节整条丢掉）。
# 而 Python 的切片是按「字符」算的：直接写 text[:240] 看着挺安全，实际
# 240 个汉字会变成 720 字节，整条消息被设备扔掉，屏幕上什么都看不到。
#
# 末尾那个 errors="ignore" 是关键：切到第 240 字节时，可能正好把一个汉字
# 的 3 个字节切成 1 个半，这种残缺字节解码会报错；ignore 让 Python 干脆
# 丢弃它 —— 结果最多是「少显示一个字」，绝不会切出乱码。
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


# 把换行、制表符、连续空格统统压成单个空格。
#
# 为什么需要它？屏幕上的摘要只占一行。命令里要是带了换行（比如多行的
# shell 脚本），原样发过去会让后面的内容跑到屏幕外面。
def collapse(text):
    """折掉换行与连续空白 —— 屏幕上只有一行位置，多行摘要会溢出。"""
    return " ".join(str(text).split())


# 从工具参数里挑出「最能让人一眼看懂」的那个字段，做成摘要。
#
# 比如 Bash 工具的参数长这样：{"command": "rm -rf 某目录", ...}
# 我们真正想显示的是那句 command —— 直接把它摆出来，比显示一整坨 JSON 强得多。
def summarize(tool_input):
    """把 tool_input 压成一行短摘要给屏幕显示。

    按「人能一眼看懂」的优先级取字段：命令 > 路径 > 模式 > URL，
    都没有就退化成整个 tool_input 的字符串形式。
    """
    if not isinstance(tool_input, dict):
        # 少数工具的参数不是字典（可能是字符串），兜底处理。
        return clamp_bytes(collapse(tool_input)[:SUMMARY_MAX_LEN], MAX_FIELD_BYTES)

    # 按优先级依次找，哪个字段有内容就用哪个。
    # 顺序是按「人在屏幕上最想看到什么」排的：命令 > 文件路径 > 搜索模式 > 网址。
    for key in ("command", "file_path", "path", "pattern", "url", "query"):
        value = tool_input.get(key)
        if isinstance(value, str) and value.strip():
            return clamp_bytes(collapse(value)[:SUMMARY_MAX_LEN], MAX_FIELD_BYTES)

    # 上面那些字段一个都没有（比如遇上了没见过的工具）—— 那就把整个
    # tool_input 转成字符串凑合显示，总比空着强。
    return clamp_bytes(collapse(tool_input)[:SUMMARY_MAX_LEN], MAX_FIELD_BYTES)


# —————————————————————————— 本地 Socket 服务 ——————————————————————————
#
# 这一节是「前台」：负责接待 hook_client 发来的请求。
# 两端的约定很简单 —— 发一行 JSON 过来，回一行 JSON 过去。


# 按请求类型分派。注意：hook_client 发来的审批请求**没有** type 字段，
# 所以「没有 type」就等于「这是审批」。
async def dispatch(bridge, req):
    """本地请求分发。

    hook_client.py 发来的是原始 Hook JSON（没有 type 字段），按审批请求处理；
    其余 type 供调试与后续的状态 Hook 使用。
    """
    msg_type = req.get("type", "approval")   # 缺省当审批处理

    if msg_type == "status":
        # 问问现在什么情况（调试用，--no-ble 模式下靠它拿 request_id）。
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
        # 就是这条：走上面那个完整的审批流程（发卡 → 等按钮 → 收尾）。
        return await bridge.handle_approval(req)

    return {"error": f"未知请求类型 {msg_type!r}"}


# 每来一个连接，就调一次这个函数 —— 一次对话，一回生。
#
# reader / writer 是 asyncio 给的「读端 / 写端」，可以理解成一根管子的两头。
async def handle_client(bridge, reader, writer):
    # 兜底初值：except Exception 接不住 BaseException（如任务取消），
    # 那条路径下 finally 里读未赋值的 resp 会抛 UnboundLocalError。
    resp = {"error": "请求处理被中断"}
    try:
        # 读一行。加超时是防止有人连上来却半句话不说，把这里一直占着。
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

        # 交给分派器。注意这里可能会阻塞很久 —— 审批要一直等到用户按键。
        resp = await dispatch(bridge, req)
    except Exception as exc:
        # 任何问题都变成一条 {"error": ...} 回给对方，绝不把异常抛出去：
        # 这次对话就此结束，不能连累整个 daemon。
        log(f"处理本地请求失败: {exc}")
        resp = {"error": str(exc)}
    finally:
        # 不管上面怎么结束，都要把答复写回去、把连接关掉。
        try:
            writer.write((json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8"))
            await writer.drain()   # 等数据真的写出去
        except Exception:
            pass
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


# —————————————————————————— 入口 ——————————————————————————


# 程序主体：挑传输层 → 建 Bridge → 派两个后台循环 → 开台子等客人。
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

    # 把传输层交给 Bridge（大脑），之后它俩自己配合。
    bridge = Bridge(transport, args.timeout)

    # 派出两个后台循环。create_task 的意思是「放它出去自己跑，我不管了」——
    # 它们会在后台一直转，直到程序退出。
    tasks = [
        asyncio.create_task(bridge.heartbeat_loop()),
        asyncio.create_task(bridge.connection_loop()),
    ]

    # 开台子：监听本地端口，每来一个连接就交给 handle_client。
    server = await asyncio.start_server(
        lambda r, w: handle_client(bridge, r, w), args.host, args.port
    )
    log(f"本地 Socket 监听 {args.host}:{args.port}")

    # 一直待客，直到被 Ctrl+C 打断。
    try:
        async with server:
            await server.serve_forever()
    finally:
        # 退出前把两个后台循环收掉，别留野任务。
        for task in tasks:
            task.cancel()


def parse_args(argv=None):
    # argparse 是 Python 自带的命令行参数解析器，负责处理用户敲的参数
    # 和自动生成 --help。每个选项下面都写了中文说明。
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
        # asyncio.run 是异步程序的「总开关」：把协程跑起来，跑完返回结果。
        # 后面那个 or 0：amain 正常结束时返回 None，转成退出码 0。
        return asyncio.run(amain(args)) or 0
    except KeyboardInterrupt:
        # 用户按了 Ctrl+C。这不是错误，打个招呼、正常退出。
        log("已退出")
        return 0


if __name__ == "__main__":
    sys.exit(main())   # 用 main() 的返回值当进程退出码
