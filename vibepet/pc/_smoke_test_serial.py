# -*- coding: utf-8 -*-
"""SerialTransport 冒烟测试 —— 不需要硬件，也不需要真的插着串口。

怎么做到的：SerialTransport 允许注入一个「假串口对象」（`serial_factory`），
于是收数据、分片重组、断线判定、重连这些逻辑全都能在没有设备的情况下验证。

**刻意不用 pyserial 自带的 loop:// 假串口** —— 那个东西会把写出去的内容原样
回显，反而绕过了我们要测的两件事：跨线程投递（数据是从后台线程回到事件循环的）
和字节级分片重组。

覆盖：行重组（含把汉字劈成两半）、多行与半截行、发送补换行、写失败判定掉线、
读失败后重连补发、boot grace（打开端口后不能立刻认为可用）、回调线程正确性、
端口自动发现。
"""

import asyncio
import os
import sys
import threading
import time

sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore

PC_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PC_DIR)

from bridge_daemon import Bridge, SerialTransport  # noqa: E402

BOOT_GRACE = 0.05      # 测试里用很短的等待时间，跑得快
SETTLE = 0.25          # 等后台线程把数据送过来的时间


# ————————————————————— 假串口 —————————————————————

class FakeSerial:
    """模拟 pyserial 的串口对象，只实现 SerialTransport 用到的那部分。"""

    def __init__(self):
        self.is_open = True
        self.closed = False
        self.written = []          # 每次 write 的原始字节
        self.fail_read = None      # 设成异常实例：read() 抛错，模拟掉线
        self.fail_write = None
        self._incoming = bytearray()

    def feed(self, data):
        """测试用：把「设备发来的」字节塞进接收缓冲。"""
        self._incoming += data

    @property
    def in_waiting(self):
        return len(self._incoming)

    def read(self, count=1):
        if self.fail_read is not None:
            raise self.fail_read
        if not self._incoming:
            # 真串口在没数据时会阻塞到 timeout（50 毫秒）再返回；
            # 这里小睡一下，免得后台线程把 CPU 跑满。
            time.sleep(0.005)
            return b""
        data = bytes(self._incoming[:count])
        del self._incoming[:count]
        return data

    def write(self, data):
        if self.fail_write is not None:
            raise self.fail_write
        self.written.append(bytes(data))
        return len(data)

    def close(self):
        self.closed = True
        self.is_open = False


def make_transport(fakes, **kwargs):
    """造一个接假串口的传输层。fakes 是「每次打开端口依次返回」的对象列表。"""
    queue = list(fakes)

    def factory(port):
        if not queue:
            raise AssertionError(f"意外地打开了第 {len(fakes) + 1} 次端口：{port}")
        fake = queue.pop(0)
        fake.port = port
        return fake

    transport = SerialTransport(port=kwargs.pop("port", "COM_TEST"),
                                boot_grace=BOOT_GRACE, serial_factory=factory, **kwargs)
    return transport, factory


results = []


def record(name, problems, skipped=None):
    results.append((name, not problems, problems, skipped))


# ————————————————————— 用例 —————————————————————

def case_line_reassembly():
    """一行被拆成 5 片（其中一片把汉字劈成两半）→ 必须还原成一整行且不乱码。"""
    problems = []
    fake = FakeSerial()
    transport, _ = make_transport([fake])
    got = []
    transport.set_line_handler(got.append)

    async def drive():
        await transport.ensure_connected()
        # 让「中」的 3 个字节恰好被切开：报文头部 43 字节，中 = 43,44,45
        payload = '{"type":"state","status":"working","msg":"中文测试"}'
        raw = payload.encode("utf-8") + b"\n"
        for chunk in (raw[:10], raw[10:44], raw[44:45], raw[45:60], raw[60:]):
            fake.feed(chunk)
            await asyncio.sleep(0.02)
        await asyncio.sleep(SETTLE)

    asyncio.run(drive())
    if got != ['{"type":"state","status":"working","msg":"中文测试"}']:
        problems.append(f"分片重组结果不对: {got!r}")
    if any("中" not in line for line in got):
        problems.append("汉字被拆坏（出现了乱码）")
    record("行重组：5 片投喂（含劈开汉字）", problems)


def case_partial_and_multiple_lines():
    """半截行不回调；一次来三行则按顺序回调三次。"""
    problems = []
    fake = FakeSerial()
    transport, _ = make_transport([fake])
    got = []
    transport.set_line_handler(got.append)

    async def drive():
        await transport.ensure_connected()
        fake.feed(b'{"type":"heartbeat","seq":1}')     # 没有换行 → 不算一条
        await asyncio.sleep(SETTLE)
        fake.feed(b"\n")                               # 补上换行 → 这才算
        await asyncio.sleep(SETTLE)
        fake.feed(b'{"type":"heartbeat","seq":2}\n{"type":"heartbeat","seq":3}\n\n')
        await asyncio.sleep(SETTLE)

    asyncio.run(drive())
    expected = ['{"type":"heartbeat","seq":1}',
                '{"type":"heartbeat","seq":2}',
                '{"type":"heartbeat","seq":3}']
    if got != expected:
        problems.append(f"行的切分或顺序不对: {got!r}")
    record("半截行不回调 / 多行按序回调 / 空行跳过", problems)


def case_send_line():
    """send_line 必须原样补一个换行，并按 UTF-8 编码。"""
    problems = []
    fake = FakeSerial()
    transport, _ = make_transport([fake])

    async def drive():
        await transport.ensure_connected()
        fake.written.clear()          # 忽略确保连接期间可能写出去的东西
        await transport.send_line('{"type":"button","action":"deny"}')
        await transport.send_line('中文')

    asyncio.run(drive())
    if fake.written != [b'{"type":"button","action":"deny"}\n',
                        '中文\n'.encode("utf-8")]:
        problems.append(f"写出去的字节不对: {fake.written!r}")
    record("发送：补换行 + UTF-8 编码", problems)


def case_write_failure():
    """写失败必须抛 ConnectionError，并把连接标记为不可用（上层靠它记账）。"""
    problems = []
    fake = FakeSerial()
    transport, _ = make_transport([fake])

    async def drive():
        await transport.ensure_connected()
        fake.fail_write = OSError("模拟设备掉线")
        try:
            await transport.send_line("x")
        except ConnectionError as exc:
            return str(exc)
        return None

    message = asyncio.run(drive())
    if message is None:
        problems.append("写失败时没有抛 ConnectionError")
    if transport.connected:
        problems.append("写失败后 connected 仍为 True")
    if not fake.closed:
        problems.append("写失败后没有关闭串口")
    record("写失败 → 抛 ConnectionError 且判定掉线", problems)


def case_read_failure_and_reconnect():
    """读失败 → 判定掉线；随后重连成功，并且 Bridge 会自动补发当前画面。"""
    problems = []
    first, second = FakeSerial(), FakeSerial()
    transport, _ = make_transport([first, second])
    got = []
    transport.set_line_handler(got.append)

    async def drive():
        bridge = Bridge(transport, 1.0)
        await transport.ensure_connected()
        await bridge.send_state("working", "补发我")

        # 设备掉线：读直接抛错
        first.fail_read = OSError("模拟 USB 被拔")
        await asyncio.sleep(SETTLE)
        if transport.connected:
            problems.append("读失败后没有判定掉线")

        # connection_loop 会不断重试打开端口；这次拿到第二个假串口
        loop_task = asyncio.create_task(bridge.connection_loop())
        await asyncio.sleep(SETTLE * 3)
        loop_task.cancel()
        try:
            await loop_task
        except asyncio.CancelledError:
            pass

        if not transport.connected:
            problems.append("重连没有成功")
        # 重连后应把「当前应有的画面」补发给新连接
        resent = [data for data in second.written
                  if b'"type": "state"' in data or b'"type":"state"' in data]
        if not resent:
            problems.append(f"重连后没有补发状态，实际写出: {second.written!r}")
        elif b"working" not in resent[0] or "补发我".encode("utf-8") not in resent[0]:
            problems.append(f"补发的内容不对: {resent[0]!r}")

    asyncio.run(drive())
    record("读失败判定掉线 → 自动重连并补发画面", problems)


def case_boot_grace():
    """打开端口后不能立刻认为可用 —— 那 2 秒里开发板正在重启，字节会被吃掉。"""
    problems = []
    fake = FakeSerial()
    transport, _ = make_transport([fake])

    async def drive():
        task = asyncio.create_task(transport.ensure_connected())
        await asyncio.sleep(0.01)          # 端口已开、等待期未满
        during = transport.connected
        opened = transport._ser is not None
        ok = await task
        return during, opened, ok, transport.connected

    during, opened, ok, after = asyncio.run(drive())
    if not opened:
        problems.append("等待期内端口应已打开")
    if during:
        problems.append("boot grace 期间 connected 必须为 False（否则补发逻辑不会触发）")
    if not ok or not after:
        problems.append("等待结束后应报告已连接")
    record("boot grace：打开端口后不立刻算可用", problems)


def case_callback_thread():
    """行回调必须在事件循环线程里执行 —— Bridge 里的 asyncio 原语假定如此。"""
    problems = []
    fake = FakeSerial()
    transport, _ = make_transport([fake])
    seen = []
    transport.set_line_handler(lambda line: seen.append(threading.current_thread()))

    async def drive():
        await transport.ensure_connected()
        loop_thread = threading.current_thread()
        fake.feed(b'{"type":"heartbeat","seq":9}\n')
        await asyncio.sleep(SETTLE)
        return loop_thread

    loop_thread = asyncio.run(drive())
    if not seen:
        problems.append("没有收到回调")
    elif any(thread is not loop_thread for thread in seen):
        problems.append(f"回调跑在别的线程里: {[t.name for t in seen]}")
    record("行回调在事件循环线程里执行", problems)


def case_port_discovery():
    """端口自动发现：优先官方 Arduino，其次常见串口芯片；找不到返回 None。"""
    problems = []
    try:
        from serial.tools import list_ports
    except ImportError:
        record("端口自动发现", [], skipped="未安装 pyserial，跳过")
        return

    import bridge_daemon

    class FakePortInfo:
        def __init__(self, device, vid, pid, description="", manufacturer=""):
            self.device = device
            self.vid = vid
            self.pid = pid
            self.description = description
            self.manufacturer = manufacturer

    original = list_ports.comports
    try:
        # 一个 CH340 兼容板 + 一个官方 UNO → 应该选官方那个
        list_ports.comports = lambda: [
            FakePortInfo("COM3", 0x1A86, 0x7523, "USB-SERIAL CH340"),
            FakePortInfo("COM7", 0x2341, 0x0043, "Arduino Uno"),
            FakePortInfo("COM1", None, None, "通讯端口"),
        ]
        picked = bridge_daemon.discover_port()
        if picked != "COM7":
            problems.append(f"应优先选官方 Arduino（COM7），实际 {picked!r}")

        # 只有兼容板 → 选它
        list_ports.comports = lambda: [FakePortInfo("COM3", 0x1A86, 0x7523, "CH340")]
        if bridge_daemon.discover_port() != "COM3":
            problems.append("只有兼容板时应选它")

        # 什么都没有 → None（不是异常）
        list_ports.comports = lambda: []
        if bridge_daemon.discover_port() is not None:
            problems.append("没有候选端口时应返回 None")
    finally:
        list_ports.comports = original

    # 指定了 --serial-port 时必须用它，且**不去自动发现**
    fake = FakeSerial()
    transport, _ = make_transport([fake], port="COM42")
    asyncio.run(transport.ensure_connected())
    if getattr(fake, "port", None) != "COM42":
        problems.append(f"指定端口未被使用: {getattr(fake, 'port', None)!r}")

    record("端口自动发现与 --serial-port 覆盖", problems)


def case_open_failure_is_not_fatal():
    """端口打不开时应返回 False（交给上层过一会儿重试），而不是抛异常崩掉。"""
    problems = []

    def factory(port):
        raise OSError("could not open port COM9: 拒绝访问")

    transport = SerialTransport(port="COM9", boot_grace=BOOT_GRACE,
                                serial_factory=factory)

    async def drive():
        return await transport.ensure_connected()

    ok = asyncio.run(drive())
    if ok:
        problems.append("打不开端口却报告已连接")
    if transport.connected:
        problems.append("打不开端口却认为已连接")
    record("端口打不开 → 返回 False 而非崩溃", problems)


# ————————————————————— 跑完汇总 —————————————————————

def main():
    for case in (case_line_reassembly,
                 case_partial_and_multiple_lines,
                 case_send_line,
                 case_write_failure,
                 case_read_failure_and_reconnect,
                 case_boot_grace,
                 case_callback_thread,
                 case_port_discovery,
                 case_open_failure_is_not_fatal):
        try:
            case()
        except Exception as exc:  # 用例自己崩了也算失败，但别带动其他用例
            record(case.__name__, [f"用例内部异常: {exc!r}"])

    print("=" * 70)
    failed = 0
    for name, ok, problems, skipped in results:
        if skipped:
            print(f"[SKIP] {name} —— {skipped}")
            continue
        print(f"[{'PASS' if ok else 'FAIL'}] {name}")
        for problem in problems:
            print(f"        {problem}")
        if not ok:
            failed += 1
    print("=" * 70)
    counted = [r for r in results if not r[3]]
    print(f"{len(counted) - failed}/{len(counted)} 通过")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
