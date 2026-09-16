# -*- coding: utf-8 -*-
"""bridge_daemon.py 冒烟测试 —— --no-ble 模式，不需要硬件。跑完即删的临时文件。

覆盖：审批闭环、request_id 匹配（F8）、超时降级、并发排队（F9）、
协议容错，以及一次 hook_client → daemon 的端到端联调。
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

sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")

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
    [sys.executable, DAEMON, "--no-ble", "--port", str(PORT),
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
            self._line_handler = None

        def set_line_handler(self, handler):
            self._line_handler = handler

        @property
        def connected(self):
            return True

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
