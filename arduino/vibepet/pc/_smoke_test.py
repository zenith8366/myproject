# -*- coding: utf-8 -*-
"""hook_client.py 冒烟测试 —— 用假 daemon 验证各条决策路径。跑完即删的临时文件。"""
import json
import os
import socket
import subprocess
import sys
import threading
import time

# 测试脚本自身也要用 UTF-8 输出，否则打印含替换字符的日志时会崩
# type: ignore —— typeshed 把 sys.stdout 标注为 TextIO 协议（无 reconfigure），
# 运行时实际是 TextIOWrapper，属类型存根局限。
sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore

CLIENT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook_client.py")
# 测试专用端口。**不能借用 8765** —— 那是真 daemon 的端口，而 daemon 现在是常驻的
# （装了 Hook 就得一直开着），占它会让这条测试直接报 WinError 10013 跑不起来。
# 另外两个冒烟测试也是同样的做法（8770 / 8771）。
PORT = 8790
# 故意带中文，验证 UTF-8 链路（中文 Windows 上 GBK 解码会在这里炸）
HOOK_INPUT = {
    "session_id": "test-session",
    "hook_event_name": "PreToolUse",
    "tool_name": "Bash",
    "tool_input": {"command": "rm -rf /tmp/构建缓存", "description": "删除构建缓存"},
}

# 非 Bash 审批（PermissionRequest 事件）的输入样本：模拟一次 Write 工具的
# 权限请求 —— 正是「屏幕上不显示审批卡」问题里那类事件。同样带中文验证编码。
# 注意 tool_input 里没有 command 字段 —— daemon 会退到 file_path 做摘要，
# 这里顺带验证「非 Bash 工具的参数也能原样转发、不丢字段」。
HOOK_INPUT_PERM = {
    "session_id": "test-session",
    "hook_event_name": "PermissionRequest",
    "tool_name": "Write",
    "tool_input": {"file_path": "E:/myproject/vibepet/临时文件.txt", "content": "hello"},
}


class FakeDaemon:
    """一次性 TCP 服务端，模拟 bridge_daemon 的应答。"""

    def __init__(self, response, reply=True):
        self.response = response
        self.reply = reply
        self.received = None
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", PORT))
        self.srv.listen(1)
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        self.srv.settimeout(5)
        try:
            conn, _ = self.srv.accept()
        except OSError:
            return  # 没人连进来（测「daemon 未运行」路径时属于预期）
        data = b""
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
        self.received = data.decode("utf-8")
        if self.reply:
            conn.sendall(json.dumps(self.response).encode("utf-8"))
        conn.close()

    def close(self):
        self.srv.close()


def run_client(env_extra=None, raw_input=None, hook_input=None):
    """raw_input 给「畸形输入」用例直接喂原始字节；hook_input 换用别的输入样本
    （比如 PermissionRequest）；两者都不给就走标准 HOOK_INPUT。"""
    env = os.environ.copy()
    # 让客户端连到测试自己的假 daemon，而不是本机那个真 daemon（8765）。
    # 放在 env_extra 之前：个别用例要用别的端口（比如「连不上」那条用 8799）能覆盖。
    env["VIBEPET_PORT"] = str(PORT)
    env.update(env_extra or {})
    if raw_input is None:
        sample = hook_input if hook_input is not None else HOOK_INPUT
        raw_input = json.dumps(sample, ensure_ascii=False).encode("utf-8")
    return subprocess.run(
        [sys.executable, CLIENT],
        input=raw_input,
        capture_output=True,
        env=env,
    )


results = []


def check(name, expected_decision, response=None, reply=True, env_extra=None,
          expect_forward=True, start_daemon=True, raw_input=None,
          hook_input=None, event="PreToolUse"):
    if hook_input is None:
        hook_input = HOOK_INPUT
    daemon = FakeDaemon(response, reply) if start_daemon else None
    time.sleep(0.35)
    try:
        proc = run_client(env_extra, raw_input, hook_input)
    finally:
        if daemon:
            daemon.close()
        time.sleep(0.3)  # 让假 daemon 线程收尾，避免解释器退出时线程仍阻塞

    out = proc.stdout.decode("utf-8")
    problems = []

    # 断言 1：stdout 必须恰好是一行合法 Hook JSON，且决策字段与事件类型匹配。
    # 两个审批事件的协议不同（PreToolUse 用 permissionDecision，
    # PermissionRequest 用 decision.behavior），串了 Claude Code 就解析不到决策，
    # 所以字段名对不上也算失败，而不是只看行为值。
    lines = [ln for ln in out.splitlines() if ln.strip()]
    decision = None
    if len(lines) != 1:
        problems.append(f"stdout 应为 1 行，实际 {len(lines)} 行: {out!r}")
    else:
        try:
            hso = json.loads(lines[0])["hookSpecificOutput"]
            if hso.get("hookEventName") != event:
                problems.append(
                    f"hookEventName 应为 {event!r}，实际 {hso.get('hookEventName')!r}")
            if event == "PermissionRequest":
                decision = hso["decision"]["behavior"]
                if "permissionDecision" in hso:
                    problems.append(f"PermissionRequest 输出混入 PreToolUse 字段: {lines[0]!r}")
                if decision == "deny" and not hso["decision"].get("message"):
                    problems.append("PermissionRequest deny 缺少 message 字段")
            else:
                decision = hso["permissionDecision"]
                if "decision" in hso:
                    problems.append(f"PreToolUse 输出混入 PermissionRequest 字段: {lines[0]!r}")
        except Exception as exc:
            problems.append(f"stdout 不是预期的 Hook JSON（{exc}）: {out!r}")

    # 断言 2：决策符合预期
    if decision is not None and decision != expected_decision:
        problems.append(f"决策应为 {expected_decision!r}，实际 {decision!r}")

    # 断言 3：请求被完整转发（中文不能损坏）
    if expect_forward and daemon is not None:
        if daemon.received is None:
            problems.append("daemon 未收到请求")
        else:
            try:
                forwarded = json.loads(daemon.received)
                if forwarded != hook_input:
                    problems.append(f"转发内容不一致: {forwarded!r}")
            except Exception as exc:
                problems.append(f"转发内容不是合法 JSON ({exc}): {daemon.received!r}")

    # 断言 4：退出码必须为 0，Hook 不能以错误码收场
    if proc.returncode != 0:
        problems.append(f"退出码应为 0，实际 {proc.returncode}")

    stderr_text = proc.stderr.decode("utf-8", errors="replace").strip()
    results.append((name, not problems, problems, stderr_text))


check("approve -> allow", "allow", {"action": "approve"})
check("deny -> deny", "deny", {"action": "deny"})
check("未知 action -> deny（fail-safe）", "deny", {"action": "maybe"})
check("daemon 不回包 -> 超时 deny", "deny", None, reply=False,
      env_extra={"VIBEPET_TIMEOUT": "2"}, expect_forward=False)
check("daemon 未运行 -> deny", "deny", env_extra={"VIBEPET_PORT": "8799"},
      expect_forward=False, start_daemon=False)

# —— PermissionRequest：非 Bash 工具的权限审批路径 ——
# 与 PreToolUse 走同一个进程、同一条审批链路，区别只在 stdout 的决策格式。
# 这组用例盯住两件事：格式没串（decision.behavior vs permissionDecision）、
# 降级路径（daemon 不在时）同样是 deny。真机之前先把这两条钉死。
check("PermissionRequest approve -> allow",
      "allow", {"action": "approve"},
      hook_input=HOOK_INPUT_PERM, event="PermissionRequest")
check("PermissionRequest deny -> deny（带 message）",
      "deny", {"action": "deny"},
      hook_input=HOOK_INPUT_PERM, event="PermissionRequest")
check("PermissionRequest daemon 未运行 -> deny",
      "deny", env_extra={"VIBEPET_PORT": "8799"},
      expect_forward=False, start_daemon=False,
      hook_input=HOOK_INPUT_PERM, event="PermissionRequest")

# —— 畸形输入不得让 Hook 崩溃 ——
# 崩溃（非零退出码 + stdout 零字节）等于「不输出决策」，而 Claude Code 对
# PreToolUse hook 的非零退出按非阻塞错误处理 = 放行，违反「降级一律拒绝」。
# 数组 / 标量输入曾让 main() 的 hook_input.get() 抛未捕获的 AttributeError。
check("非对象 JSON（数组）-> deny 且不崩溃", "deny", start_daemon=False,
      raw_input=b"[1,2,3]", expect_forward=False)
check("stdin 为空 -> deny 且不崩溃", "deny", start_daemon=False,
      raw_input=b"", expect_forward=False)

print("=" * 68)
failed = 0
for name, ok, problems, stderr_text in results:
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    for p in problems:
        print(f"        {p}")
    if stderr_text:
        print(f"        stderr: {stderr_text.splitlines()[0]}")
    if not ok:
        failed += 1
print("=" * 68)
print(f"{len(results) - failed}/{len(results)} 通过")
sys.exit(1 if failed else 0)
