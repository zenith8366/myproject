# -*- coding: utf-8 -*-
"""hook_client.py 状态事件映射冒烟测试。跑完即删的临时文件。

核心断言：状态事件必须向 stdout 输出「零个字节」—— 那是一条会流回
Claude Code 的通道，任何多余输出都可能被当成 Hook 决策解析。
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time

# type: ignore —— typeshed 把 sys.stdout 标注为 TextIO 协议（无 reconfigure），
# 运行时实际是 TextIOWrapper，属类型存根局限。
sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore

PC_DIR = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.join(PC_DIR, "hook_client.py")
PORT = 8771


class CollectingDaemon:
    """接受任意多个连接，记录每条 payload，回 {"ok":true}。"""

    def __init__(self, port):
        self.received = []
        self._stop = False
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(16)
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        self.srv.settimeout(0.3)
        while not self._stop:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                continue
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        try:
            data = b""
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            if data.strip():
                self.received.append(json.loads(data.decode("utf-8")))
            conn.sendall(b'{"ok": true}\n')
        except Exception:
            pass
        finally:
            conn.close()

    def close(self):
        self._stop = True
        self.srv.close()


def run_client(hook_input, env_extra=None, timeout=15):
    env = os.environ.copy()
    env["VIBEPET_PORT"] = str(PORT)
    env.update(env_extra or {})
    return subprocess.run(
        [sys.executable, CLIENT],
        input=json.dumps(hook_input, ensure_ascii=False).encode("utf-8"),
        capture_output=True, env=env, timeout=timeout,
    )


# 事件 → 期望的 (status, msg)。None 表示该事件不该产生任何上报。
CASES = [
    ({"hook_event_name": "SessionStart"},                 "idle",    "会话开始"),
    ({"hook_event_name": "SessionEnd"},                   "idle",    "会话结束"),
    ({"hook_event_name": "UserPromptSubmit"},             "working", "思考中"),
    ({"hook_event_name": "Stop"},                         "done",    "任务完成"),
    ({"hook_event_name": "SubagentStop"},                 "working", "子任务完成"),
    ({"hook_event_name": "PreCompact"},                   "working", "压缩上下文"),
    ({"hook_event_name": "PostToolUse", "tool_name": "Bash",
      "tool_response": {"stdout": "ok"}},                 "working", "Bash 完成"),
    ({"hook_event_name": "PostToolUse", "tool_name": "Read",
      "tool_response": {}},                               "working", "Read 完成"),
    ({"hook_event_name": "PostToolUse", "tool_name": "Bash",
      "tool_response": {"is_error": True}},               "error",   "Bash 出错"),
    ({"hook_event_name": "PostToolUse", "tool_name": "Bash",
      "tool_response": {"error": "command not found"}},   "error",   "Bash 出错"),
    # 无法判断成功失败时按成功处理，不该误报红色 ERROR
    ({"hook_event_name": "PostToolUse", "tool_name": "Bash",
      "tool_response": "some plain string"},              "working", "Bash 完成"),
    # 不认识的事件：一个字都不发
    ({"hook_event_name": "Notification"},                 None,      None),
    ({"hook_event_name": "WhateverFutureEvent"},          None,      None),
]

daemon = CollectingDaemon(PORT)
time.sleep(0.3)
results = []

try:
    for i, (hook_input, expect_status, expect_msg) in enumerate(CASES, 1):
        event = hook_input.get("hook_event_name")
        name = f"{event}" + (f"/{hook_input.get('tool_name')}"
                             if hook_input.get("tool_name") else "")
        label = f"#{i} {name} -> {expect_status or '不上报'}"

        before = len(daemon.received)
        proc = run_client(hook_input)

        # 等 daemon 收到（若该上报）
        deadline = time.time() + 3
        while time.time() < deadline and len(daemon.received) == before:
            time.sleep(0.05)
        new = daemon.received[before:]

        problems = []

        # 断言 1（最关键）：状态事件的 stdout 必须为 0 字节
        if proc.stdout != b"":
            problems.append(f"stdout 必须为空，实际 {proc.stdout!r}")

        # 断言 2：退出码 0
        if proc.returncode != 0:
            problems.append(f"退出码应为 0，实际 {proc.returncode}")

        # 断言 3：上报内容符合映射
        if expect_status is None:
            if new:
                problems.append(f"不该上报，实际收到 {new}")
        else:
            if not new:
                problems.append("未上报任何状态")
            else:
                msg = new[0]
                if msg.get("type") != "state":
                    problems.append(f"type 应为 state，实际 {msg.get('type')!r}")
                if msg.get("status") != expect_status:
                    problems.append(f"status 应为 {expect_status!r}，实际 {msg.get('status')!r}")
                if msg.get("msg") != expect_msg:
                    problems.append(f"msg 应为 {expect_msg!r}，实际 {msg.get('msg')!r}")

        results.append((label, not problems, problems))

    # —— 用例：daemon 未运行时状态上报必须快速放弃，不能拖慢 Claude Code ——
    daemon.close()
    time.sleep(0.2)
    started = time.time()
    proc = run_client({"hook_event_name": "Stop"}, timeout=15)
    elapsed = time.time() - started
    problems = []
    if proc.stdout != b"":
        problems.append(f"stdout 必须为空，实际 {proc.stdout!r}")
    if proc.returncode != 0:
        problems.append(f"退出码应为 0，实际 {proc.returncode}")
    if elapsed > 2.0:
        problems.append(f"耗时 {elapsed:.2f}s，状态上报失败必须快速放弃（应 < 2s）")
    results.append((f"daemon 未运行时状态上报快速失败（{elapsed:.2f}s）", not problems, problems))

finally:
    try:
        daemon.close()
    except Exception:
        pass

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
sys.exit(1 if failed else 0)
