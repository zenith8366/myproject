# -*- coding: utf-8 -*-
"""bridge_daemon.py 冒烟测试 —— --no-device 模式，不需要硬件。跑完即删的临时文件。

覆盖：审批闭环、request_id 匹配（F8）、超时降级、并发排队（F9）、协议容错、
整行长度预算，以及一次 hook_client → daemon 的端到端联调。
"""
import asyncio
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

# type: ignore —— typeshed 把 sys.stdout 标注为 TextIO 协议（无 reconfigure），
# 运行时实际是 TextIOWrapper，属类型存根局限。
sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore

PC_DIR = os.path.dirname(os.path.abspath(__file__))
DAEMON = os.path.join(PC_DIR, "bridge_daemon.py")
CLIENT = os.path.join(PC_DIR, "hook_client.py")
PORT = 8770
APPROVAL_TIMEOUT = 3  # 缩短审批超时，让超时用例跑得快


def send_request(payload, timeout=30):
    """向 daemon 发一条请求并读回响应。"""
    with socket.create_connection(("127.0.0.1", PORT), timeout=timeout) as sock:
        sock.sendall(json.dumps(payload, ensure_ascii=False).encode("utf-8") + b"\n")
        sock.shutdown(socket.SHUT_WR)
        buf = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            try:
                json.loads(buf.decode("utf-8"))
                break
            except Exception:
                continue
    return json.loads(buf.decode("utf-8"))


def wait_for_pending(timeout=5):
    """轮询 status，等 daemon 进入等待审批状态并返回其 request_id。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            rid = send_request({"type": "status"}, timeout=5).get("pending_request_id")
        except Exception:
            rid = None
        if rid:
            return rid
        time.sleep(0.1)
    return None


def approve_async(tool_input):
    """在后台线程发起一次审批，返回 (thread, result_holder)。"""
    holder = {}
    hook_input = {"tool_name": "Bash", "tool_input": tool_input}

    def run():
        try:
            holder["resp"] = send_request(hook_input, timeout=30)
        except Exception as exc:
            holder["error"] = exc

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, holder


results = []


def record(name, problems):
    results.append((name, not problems, problems))


# ————————————————————— 启动 daemon —————————————————————

log_fd, log_path = tempfile.mkstemp(suffix=".log", text=True)
os.close(log_fd)
log_handle = open(log_path, "w", encoding="utf-8")
daemon_proc = subprocess.Popen(
    [sys.executable, DAEMON, "--no-device", "--port", str(PORT),
     "--timeout", str(APPROVAL_TIMEOUT), "-v"],
    stdout=subprocess.DEVNULL, stderr=log_handle,
)

ready = False
deadline = time.time() + 10
while time.time() < deadline:
    try:
        socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
        ready = True
        break
    except OSError:
        time.sleep(0.1)

if not ready:
    print("[FAIL] daemon 未能在 10 s 内启动")
    daemon_proc.terminate()
    log_handle.close()
    print(open(log_path, encoding="utf-8").read())
    sys.exit(1)

try:
    # —— 用例 1：approve 闭环 ——
    thread, holder = approve_async({"command": "rm -rf /tmp/构建缓存"})
    rid = wait_for_pending()
    problems = []
    if not rid:
        problems.append("未进入等待审批状态")
    else:
        send_request({"type": "button", "request_id": rid, "action": "approve"})
    thread.join(timeout=10)
    if holder.get("resp", {}).get("action") != "approve":
        problems.append(f"应返回 approve，实际 {holder.get('resp') or holder.get('error')}")
    record("approve 闭环", problems)

    # —— 用例 2：request_id 不匹配的按钮必须被丢弃（F8）——
    thread, holder = approve_async({"command": "echo hello"})
    rid = wait_for_pending()
    problems = []
    if not rid:
        problems.append("未进入等待审批状态")
    else:
        # 先按一个错误的 request_id：这次审批必须继续等待，直到超时降级
        started = time.time()
        send_request({"type": "button", "request_id": "deadbeef", "action": "approve"})
        thread.join(timeout=15)
        elapsed = time.time() - started
        resp = holder.get("resp", {})
        if resp.get("action") != "deny":
            problems.append(f"错误 request_id 应被丢弃并最终超时 deny，实际 {resp}")
        if elapsed < APPROVAL_TIMEOUT * 0.8:
            problems.append(f"过早返回（{elapsed:.1f}s），说明错误按钮被误接受")
    record("request_id 不匹配 -> 丢弃并超时 deny（F8）", problems)

    # —— 用例 3：迟到按钮不得影响下一次审批（F8）——
    stale_rid = rid
    thread, holder = approve_async({"command": "echo 第二次"})
    new_rid = wait_for_pending()
    problems = []
    if not new_rid:
        problems.append("第二次审批未进入等待状态")
    else:
        if new_rid == stale_rid:
            problems.append(f"request_id 复用了旧值 {new_rid}")
        # 注入上一轮的迟到按钮 —— 必须被忽略，本次应继续等到超时
        send_request({"type": "button", "request_id": stale_rid, "action": "approve"})
        thread.join(timeout=15)
        if holder.get("resp", {}).get("action") != "deny":
            problems.append(f"迟到按钮被误接受，实际 {holder.get('resp')}")
    record("迟到按钮不影响下一次审批（F8）", problems)

    # —— 用例 4：无条件超时降级 ——
    started = time.time()
    resp = send_request({"tool_name": "Bash", "tool_input": {"command": "sleep 1"}}, timeout=20)
    elapsed = time.time() - started
    problems = []
    if resp.get("action") != "deny":
        problems.append(f"超时应返回 deny，实际 {resp}")
    if not (APPROVAL_TIMEOUT * 0.8 <= elapsed <= APPROVAL_TIMEOUT + 3):
        problems.append(f"超时时长异常: {elapsed:.1f}s（期望约 {APPROVAL_TIMEOUT}s）")
    record("审批超时 -> deny", problems)

    # —— 用例 5：并发审批排队，不互相覆盖 request_id（F9）——
    t1, h1 = approve_async({"command": "第一次并发"})
    rid1 = wait_for_pending()
    t2, h2 = approve_async({"command": "第二次并发"})
    time.sleep(0.5)
    # 第二次必须还在排队 —— 不能抢走单槽
    mid_status = send_request({"type": "status"})
    problems = []
    if mid_status.get("pending_request_id") != rid1:
        problems.append(f"第二个请求覆盖了 pending 槽: {mid_status}")
    send_request({"type": "button", "request_id": rid1, "action": "deny"})
    t1.join(timeout=10)
    # 第一个结束后，第二个才拿到槽位
    rid2 = wait_for_pending()
    if not rid2:
        problems.append("第二个请求未能接续进入等待状态")
    else:
        send_request({"type": "button", "request_id": rid2, "action": "approve"})
    t2.join(timeout=10)
    if h1.get("resp", {}).get("action") != "deny":
        problems.append(f"第一个应为 deny，实际 {h1.get('resp')}")
    if h2.get("resp", {}).get("action") != "approve":
        problems.append(f"第二个应为 approve，实际 {h2.get('resp')}")
    record("并发审批排队，request_id 不互相覆盖（F9）", problems)

    # —— 用例 6：协议容错 ——
    problems = []
    with socket.create_connection(("127.0.0.1", PORT), timeout=5) as sock:
        sock.sendall(b"this is not json\n")
        sock.shutdown(socket.SHUT_WR)
        raw = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            raw += chunk
    if "error" not in json.loads(raw.decode("utf-8")):
        problems.append(f"非法 JSON 应返回 error，实际 {raw!r}")
    resp = send_request({"type": "nonsense"})
    if "error" not in resp:
        problems.append(f"未知请求类型应返回 error，实际 {resp}")
    # 容错之后 daemon 必须还活着
    try:
        if not send_request({"type": "status"}).get("connected"):
            problems.append("daemon 状态异常")
    except Exception as exc:
        problems.append(f"容错后 daemon 不可用: {exc}")
    record("协议容错（非法 JSON / 未知类型）", problems)

    # —— 用例 7：状态去重逻辑（直接驱动 Bridge，精确断言下发序列）——
    # 关键陷阱：审批卡期间设备显示的不是 state，若不标记，审批结束后的
    # working 会被当成「与上次相同」静默丢弃，屏幕永远卡在 APPROVE?。
    sys.path.insert(0, PC_DIR)
    from bridge_daemon import Bridge

    class RecordingTransport:
        def __init__(self):
            self.sent = []
            self.connected_flag = True   # 用例 10/11 通过它模拟掉线 / 重连
            self._line_handler = None

        def set_line_handler(self, handler):
            self._line_handler = handler

        @property
        def connected(self):
            return self.connected_flag

        async def ensure_connected(self):
            return True

        async def send_line(self, text):
            self.sent.append(json.loads(text))

    async def drive():
        transport = RecordingTransport()
        bridge = Bridge(transport, 1.0)
        await bridge.send_state("working", "A")
        await bridge.send_state("working", "A")      # 与当前显示相同 -> 跳过
        await bridge.send_state("working", "B")      # msg 不同 -> 下发
        await bridge.send_approval_request("rid", "Bash", "摘要")
        await bridge.send_state("working", "B")      # 审批卡之后 -> 必须重新下发
        return transport.sent

    sent = asyncio.run(drive())
    kinds = [(m.get("type"), m.get("status"), m.get("msg")) for m in sent]
    expected = [
        ("state", "working", "A"),
        ("state", "working", "B"),
        ("approval_request", None, None),
        ("state", "working", "B"),
    ]
    problems = []
    if len(sent) != len(expected):
        problems.append(f"应下发 {len(expected)} 条，实际 {len(sent)} 条: {kinds}")
    else:
        for i, (exp, got) in enumerate(zip(expected, kinds), 1):
            if exp[0] != got[0] or (exp[1] and exp[1] != got[1]) or (exp[2] and exp[2] != got[2]):
                problems.append(f"第 {i} 条不符: 期望 {exp}，实际 {got}")
    record("状态去重 + 审批卡后状态必须重发", problems)

    # —— 用例 8：hook_client → daemon 端到端 ——
    problems = []
    proc_holder = {}

    def run_client():
        env = os.environ.copy()
        env["VIBEPET_PORT"] = str(PORT)
        proc_holder["p"] = subprocess.run(
            [sys.executable, CLIENT],
            input=json.dumps({"tool_name": "Bash", "tool_input": {"command": "ls -la"}},
                             ensure_ascii=False).encode("utf-8"),
            capture_output=True, env=env,
        )

    client_thread = threading.Thread(target=run_client, daemon=True)
    client_thread.start()
    rid = wait_for_pending()
    if not rid:
        problems.append("端到端：未进入等待审批状态")
    else:
        send_request({"type": "button", "request_id": rid, "action": "approve"})
    client_thread.join(timeout=15)
    proc = proc_holder.get("p")
    if proc is None:
        problems.append("hook_client 未返回")
    else:
        try:
            decision = json.loads(proc.stdout.decode("utf-8"))["hookSpecificOutput"]["permissionDecision"]
            if decision != "allow":
                problems.append(f"端到端决策应为 allow，实际 {decision!r}")
        except Exception as exc:
            problems.append(f"hook_client 输出异常 ({exc}): {proc.stdout!r}")
    record("端到端 hook_client -> daemon -> 按钮", problems)

    # —— 用例 9：UTF-8 边界安全截断（clamp_bytes）——
    # 设备按「字节」收行（整行上限 320、单字段 160），所以截断必须按字节算：
    # 一个汉字在 UTF-8 里占 3 个字节，按字符截断会让 60 个汉字悄悄变成 180 字节。
    # （下面用 240 这个限值是为了让边界落在「非整数倍」上，与协议取值无关。）
    problems = []
    try:
        from bridge_daemon import clamp_bytes
    except ImportError as exc:
        problems.append(f"clamp_bytes 尚未实现: {exc}")
    else:
        if clamp_bytes("abc", 240) != "abc" or clamp_bytes("", 240) != "":
            problems.append("未超限的短串被改动")
        sixty = "中" * 60                       # 180 字节：应原样通过
        if clamp_bytes(sixty, 240) != sixty:
            problems.append("60 汉字（180 字节）不应被截断")
        long_cn = clamp_bytes("汉" * 100, 240)  # 300 -> 240 字节（80 字）
        if len(long_cn.encode("utf-8")) > 240 or long_cn != "汉" * 80:
            problems.append(
                f"3 字节字符截断有误: {len(long_cn.encode('utf-8'))} 字节 / 尾字符 {long_cn[-3:]!r}")
        emo = clamp_bytes("😀" * 100, 240)      # 400 -> 240 字节（60 个 4 字节字符）
        if len(emo.encode("utf-8")) > 240 or emo != "😀" * 60:
            problems.append(f"4 字节字符截断有误: {len(emo.encode('utf-8'))} 字节")
        odd = clamp_bytes("汉" * 100, 241)      # 边界落在第 81 个字中间
        if len(odd.encode("utf-8")) > 241 or odd != "汉" * 80:
            problems.append(f"非整除边界截断有误: {len(odd.encode('utf-8'))} 字节")
    record("UTF-8 边界安全截断（clamp_bytes）", problems)

    # —— 用例 10：断线重连补发与审批参数生命周期 ——
    # 场景：审批结束的收尾 state 发送失败 / 审批进行中 BLE 断开重连。
    # 屏幕不能卡在 APPROVE?，也不能停在 LOST —— 重连后必须由 daemon 补发。
    async def drive_resend():
        transport = RecordingTransport()      # connected 可通过 connected_flag 切换
        bridge = Bridge(transport, 1.0)
        await bridge.send_state("working", "A")

        # 发送失败（BLE 已断）：屏幕内容变得未知，重连后必须原样补发
        transport.connected_flag = False
        try:
            await bridge.send_state("done", "D")
        except ConnectionError:
            pass
        transport.connected_flag = True
        await bridge.resend_current()
        resent = transport.sent[-1]

        # handle_approval 期间应保存审批参数，结束后清空
        task = asyncio.create_task(bridge.handle_approval(
            {"tool_name": "Bash", "tool_input": {"command": "echo hi"}}))
        await asyncio.sleep(0.05)
        saved = bridge._pending_approval
        rid = bridge._pending_request_id
        bridge.inject_button({"type": "button", "request_id": rid, "action": "approve"})
        await task
        cleared = bridge._pending_approval

        # 审批进行中重连：重发审批卡，request_id 必须与原来一致（否则设备回传对不上）
        bridge._pending_approval = {"request_id": "rid9", "tool": "Bash", "summary": "摘要9"}
        await bridge.resend_current()
        return resent, saved, rid, cleared, transport.sent[-1]

    problems = []
    try:
        resent, saved, rid, cleared, appr = asyncio.run(drive_resend())
        if (resent.get("type"), resent.get("status"), resent.get("msg")) != ("state", "done", "D"):
            problems.append(f"重连后未补发最后状态: {resent}")
        if not saved or saved.get("request_id") != rid:
            problems.append(f"审批参数未保存: saved={saved} rid={rid}")
        if cleared is not None:
            problems.append(f"审批结束后 _pending_approval 未清空: {cleared}")
        if (appr.get("type"), appr.get("request_id")) != ("approval_request", "rid9"):
            problems.append(f"审批中重连未重发审批卡: {appr}")
    except Exception as exc:
        problems.append(f"补发流程异常: {exc!r}")
    record("断线重连补发 + 审批参数生命周期", problems)

    # —— 用例 11：connection_loop 重连成功后自动补发 ——
    # 这是「重连后屏幕自己切回去」的电脑端那一半：补发动作要挂在重连成功点上。
    async def drive_loop():
        class FlakyTransport(RecordingTransport):
            async def ensure_connected(self):
                self.connected_flag = True
                return True

        transport = FlakyTransport()
        bridge = Bridge(transport, 1.0)
        await bridge.send_state("working", "A")   # 断线前的最后状态
        transport.connected_flag = False          # 模拟掉线
        task = asyncio.create_task(bridge.connection_loop())
        await asyncio.sleep(0.2)                  # 让第一轮「重连 + 补发」跑完
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        return [(m.get("type"), m.get("status"), m.get("msg")) for m in transport.sent]

    problems = []
    try:
        kinds = asyncio.run(drive_loop())
        if kinds.count(("state", "working", "A")) != 2:
            problems.append(f"重连后应补发一次最后状态（同内容共 2 条），实际: {kinds}")
    except Exception as exc:
        problems.append(f"connection_loop 补发异常: {exc!r}")
    record("connection_loop 重连后自动补发", problems)

    # —— 用例 12：并发发送同一条状态只下发一条 ——
    # 去重判断若和记账不在同一个临界区，并发调用会双双通过判断，设备端收到
    # 两条一模一样的消息、白刷一次屏。SlowTransport 的 sleep(0) 是必须的：
    # RecordingTransport 里没有真正的让出点，gather 出来的三个协程会顺序跑完，
    # 那样即使没有锁也测不出问题。
    class SlowTransport(RecordingTransport):
        async def send_line(self, text):
            await asyncio.sleep(0)          # 制造真实让出点，让并发真正交错
            await super().send_line(text)

    async def drive_concurrent():
        transport = SlowTransport()
        bridge = Bridge(transport, 1.0)
        await asyncio.gather(
            bridge.send_state("working", "A"),
            bridge.send_state("working", "A"),
            bridge.send_state("working", "A"),
        )
        return transport.sent

    problems = []
    try:
        sent = asyncio.run(drive_concurrent())
        if len(sent) != 1:
            problems.append(f"并发发同一条状态应只下发 1 条，实际 {len(sent)} 条: {sent}")
    except Exception as exc:
        problems.append(f"并发发送异常: {exc!r}")
    record("并发发送同一条状态只下发一条", problems)

    # —— 用例 13：整行长度预算（最坏情况不能超过设备的行上限）——
    # 背景：设备固件的行上限是 LINE_MAX=320 字节，超长的行**整条丢弃**。
    # 所以电脑端必须保证最坏情况（超长工具名 + 满长度的中文摘要）也塞得下：
    #     固定骨架 66 + request_id 8 + tool 24 + summary 160 ≈ 258 字节
    # 这条用例防的就是「以后有人把上限改小了」或「工具名忘了截断」—— 那会让
    # 审批请求被设备静默丢弃：屏幕什么都不显示，120 秒后超时拒绝，
    # 而 daemon 全程以为发送成功了，日志里一无所获。
    problems = []
    try:
        from bridge_daemon import MAX_FIELD_BYTES, MAX_TOOL_BYTES, clamp_bytes
        worst_tool = clamp_bytes("mcp__github__create_issue", MAX_TOOL_BYTES)
        worst_summary = clamp_bytes("汉" * 200, MAX_FIELD_BYTES)
        line = json.dumps({
            "type": "approval_request",
            "request_id": "01234567",
            "tool": worst_tool,
            "summary": worst_summary,
        }, ensure_ascii=False)
        size = len(line.encode("utf-8")) + 1        # +1 是行尾的换行符
        if size > 320:
            problems.append(
                f"最坏情况整行 {size} 字节 > 设备上限 320，会被整条丢弃")
        if len(worst_tool.encode("utf-8")) > MAX_TOOL_BYTES:
            problems.append(f"工具名没有被截到 {MAX_TOOL_BYTES} 字节以内")
        if len(worst_summary.encode("utf-8")) > MAX_FIELD_BYTES:
            problems.append(f"摘要没有被截到 {MAX_FIELD_BYTES} 字节以内")
    except ImportError as exc:
        problems.append(f"协议常量尚未实现: {exc}")
    record("整行长度预算（最坏情况 ≤ 320 字节）", problems)

    # —— 用例 14：审批卡在屏幕上时，状态更新不许把它顶掉 ——
    # 真实场景：一条 Bash 命令被批准执行完之后，Claude Code 会触发 PostToolUse，
    # 于是又一条状态（working / Bash 完成）发下来。如果那一刻屏幕上正停着**下一条**
    # 审批的卡片，这条状态就会把卡片刷掉 —— 用户根本没看见那条审批，
    # 它只能静默等到 120 秒超时、被当成拒绝。
    # （这正是「多条审批挤在一起时有的审批不显示」的根因。）
    async def drive_card_protection():
        transport = RecordingTransport()
        bridge = Bridge(transport, 1.0)
        task = asyncio.create_task(bridge.handle_approval(
            {"tool_name": "Bash", "tool_input": {"command": "echo A"}}))
        await asyncio.sleep(0.05)
        rid = bridge._pending_request_id
        if rid is None:
            return None, transport.sent
        before = len(transport.sent)
        await bridge.send_state("working", "Bash 完成")   # 模拟 PostToolUse
        during = transport.sent[before:]
        bridge.inject_button({"type": "button", "request_id": rid, "action": "approve"})
        await task
        return during, transport.sent

    problems = []
    try:
        during, _ = asyncio.run(drive_card_protection())
        if during is None:
            problems.append("审批没有进入等待状态，用例无法进行")
        elif any(m.get("type") == "state" for m in during):
            problems.append(
                "审批进行中仍下发了状态，会把屏幕上的审批卡顶掉，"
                f"用户看不到那条审批: {during}")
    except Exception as exc:
        problems.append(f"审批卡保护测试异常: {exc!r}")
    record("审批进行中不下发状态（卡片不被顶掉）", problems)

finally:
    daemon_proc.terminate()
    try:
        daemon_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        daemon_proc.kill()
    log_handle.close()

print("=" * 70)
failed = 0
for name, ok, problems in results:
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    for p in problems:
        print(f"        {p}")
    if not ok:
        failed += 1
print("=" * 70)
print(f"{len(results) - failed}/{len(results)} 通过")

if failed:
    print("\n———— daemon 日志 ————")
    print(open(log_path, encoding="utf-8").read())

os.unlink(log_path)
sys.exit(1 if failed else 0)
