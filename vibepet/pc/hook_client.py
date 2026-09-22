#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VibePet Hook 客户端（电脑端 · 短生命周期进程）

由 Claude Code 的 Hook 唤起，每次事件启动一次进程：

    Claude Code ──stdin JSON──> hook_client.py ──本地 Socket──> bridge_daemon.py
    hook_client.py ──stdout JSON──> Claude Code（只有 PreToolUse 需要）

两类职责，行为完全不同：

  1. PreToolUse（审批）—— 阻塞等待物理按钮，向 stdout 输出 allow / deny 决策。
  2. 其余事件（状态）—— 把事件映射成设备状态上报，不阻塞、不输出任何内容。

铁律：stdout 只允许出现 PreToolUse 的那一行 Hook JSON，状态事件一个字都不输出。
      日志一律走 stderr —— 设计文档 8.1 把「Hook stdout 被日志污染」列为高影响风险。

降级：PreToolUse 任何异常都返回 deny（设计文档 8.3）；状态上报失败则静默放弃 ——
      设备没亮起来，绝不能拖慢 Claude Code。

手动测试：
    echo '{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls"}}' \
        | python hook_client.py
    echo '{"hook_event_name":"Stop"}' | python hook_client.py     # 状态上报，stdout 无输出

环境变量：
    VIBEPET_HOST             daemon 地址，默认 127.0.0.1
    VIBEPET_PORT             daemon 端口，默认 8765
    VIBEPET_TIMEOUT          等待按钮决策的秒数，默认 130（daemon 端超时 120 s）
    VIBEPET_CONNECT_TIMEOUT  连接 daemon 的秒数，默认 5
    VIBEPET_STATE_TIMEOUT    状态上报超时秒数，默认 1（必须短，不能拖慢 Claude Code）
    VIBEPET_DEBUG=1          打印调试日志到 stderr
"""

import json
import os
import socket
import sys

# Windows 上 Python 的文本流默认跟随系统 locale（中文系统即 GBK），遇到非 GBK
# 字符会抛 UnicodeEncodeError。日志里一旦出现这类字符，整个 Hook 进程就会崩掉，
# 而 Hook 崩溃等于审批链路中断。显式改为 UTF-8，与 Hook JSON 的编码保持一致。
try:
    # typeshed 把 sys.stderr 标注成 TextIO 协议（协议里没有 reconfigure），
    # 运行时实际是 TextIOWrapper，该方法一定存在 —— 属类型存根局限，
    # 故 type: ignore；真遇到不支持的流对象由 except 兜底。
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore
except Exception:
    pass


def log(message):
    """日志一律走 stderr —— stdout 是 Claude Code 解析决策的通道。"""
    try:
        print(f"[vibepet] {message}", file=sys.stderr)
    except Exception:
        pass  # 日志失败绝不能影响审批决策


def debug(message):
    if DEBUG:
        log(message)


def _env_number(name, default, cast):
    """读取数值型环境变量；值非法时回退默认值，而不是让整个 Hook 崩溃。"""
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return cast(raw)
    except ValueError:
        log(f"环境变量 {name}={raw!r} 不是合法数值，回退默认值 {default}")
        return default


DAEMON_HOST = os.environ.get("VIBEPET_HOST", "127.0.0.1")
DAEMON_PORT = _env_number("VIBEPET_PORT", 8765, int)
# daemon 端审批超时 120 s（设计文档 5.4）。客户端留 10 s 余量，
# 让「超时拒绝」这个判断由 daemon 做出，而不是客户端抢先放弃。
DECISION_TIMEOUT = _env_number("VIBEPET_TIMEOUT", 130.0, float)
CONNECT_TIMEOUT = _env_number("VIBEPET_CONNECT_TIMEOUT", 5.0, float)
# 状态上报必须快速失败：它挂在每个工具的 PostToolUse 上，
# 拖一秒就是实打实地拖慢每一次工具调用。
STATE_TIMEOUT = _env_number("VIBEPET_STATE_TIMEOUT", 1.0, float)
DEBUG = os.environ.get("VIBEPET_DEBUG", "").strip() not in ("", "0")


# ————————————————————————— 事件 → 状态映射 —————————————————————————
#
# 设计文档 3.3 定义了六种状态，其中 heartbeat_lost 由设备端看门狗自行判断，
# 其余五种在这里闭环。static 事件直接查表，PostToolUse 需要看工具结果。

SIMPLE_EVENT_MAP = {
    "SessionStart":     ("idle",    "会话开始"),
    "SessionEnd":       ("idle",    "会话结束"),
    "UserPromptSubmit": ("working", "思考中"),
    "SubagentStop":     ("working", "子任务完成"),
    "PreCompact":       ("working", "压缩上下文"),
    "Stop":             ("done",    "任务完成"),
}


def looks_like_error(tool_response):
    """启发式判断工具是否失败。

    各工具的 tool_response 结构并不统一，Claude Code 也没有稳定的通用错误字段，
    因此这里只认几个明确信号，判断不出的按成功处理 —— 宁可不报错，
    也不要把正常结果误判成红色 ERROR 吓人。
    """
    if not isinstance(tool_response, dict):
        return False
    if tool_response.get("is_error") is True:
        return True
    error = tool_response.get("error")
    return isinstance(error, str) and bool(error.strip())


def resolve_state(hook_input):
    """把 Hook 事件映射成 (status, msg)；返回 None 表示该事件不上报。"""
    event = hook_input.get("hook_event_name", "")

    if event == "PostToolUse":
        tool = str(hook_input.get("tool_name") or "工具")
        if looks_like_error(hook_input.get("tool_response")):
            return "error", f"{tool} 出错"
        return "working", f"{tool} 完成"

    return SIMPLE_EVENT_MAP.get(event)


# ————————————————————————— 与 daemon 通信 —————————————————————————


def read_hook_input():
    """从 stdin 读取 Hook 传入的 JSON。

    显式走 buffer + UTF-8 解码：中文 Windows 的 locale 编码多为 GBK，
    sys.stdin.read() 遇到带中文的 tool_input（中文路径、中文命令描述）
    会抛 UnicodeDecodeError，等于每次调用都被降级成 deny。
    """
    raw = sys.stdin.buffer.read()
    if not raw.strip():
        raise ValueError("stdin 为空，未收到 Hook 输入")
    data = json.loads(raw.decode("utf-8"))
    if not isinstance(data, dict):
        # 合法 JSON 但不是对象（数组 / 字符串 / 数字）。必须在 read_hook_input
        # 里拦掉：放过去的话 main() 的 hook_input.get() 会抛 AttributeError，
        # 进程以退出码 1 崩掉 —— 崩溃 = 不输出任何决策 = 放行，违反
        # 「降级方向一律是拒绝」这条铁律（CLAUDE.md 硬性约束）。
        raise ValueError(f"Hook 输入必须是 JSON 对象，实际是 {type(data).__name__}")
    return data


def request_decision(hook_input):
    """把 Hook 输入转发给 daemon，阻塞等待物理按钮决策。

    返回 'approve' 或 'deny'；异常向上抛给 main() 统一降级。
    """
    payload = json.dumps(hook_input, ensure_ascii=False).encode("utf-8") + b"\n"

    with socket.create_connection((DAEMON_HOST, DAEMON_PORT), timeout=CONNECT_TIMEOUT) as sock:
        sock.settimeout(DECISION_TIMEOUT)
        sock.sendall(payload)
        # 半关闭写方向。daemon 端按设计文档用 reader.read(4096) 读请求，
        # 该方法要等到 EOF 才返回；不发 FIN 的话 daemon 会一直等到连接超时，
        # 审批链路直接卡死。末尾的 \n 则兼容改用 readline() 的实现。
        sock.shutdown(socket.SHUT_WR)

        buf = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:  # daemon 关闭了连接
                break
            buf += chunk
            try:
                json.loads(buf.decode("utf-8"))
                break  # 收全了一条完整 JSON
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue  # 数据分片，继续读

    if not buf:
        raise ConnectionError("daemon 未返回任何数据")

    action = json.loads(buf.decode("utf-8")).get("action")
    if action not in ("approve", "deny"):
        log(f"daemon 返回未知决策 {action!r}，按 deny 处理")
        return "deny"
    return action


def report_state(status, msg):
    """向 daemon 上报设备状态。任何失败都静默吞掉 —— 状态显示是锦上添花，
    绝不能因为它没跑起来就拖慢或弄挂 Claude Code 的工具调用。"""
    payload = json.dumps({"type": "state", "status": status, "msg": msg},
                         ensure_ascii=False).encode("utf-8") + b"\n"
    try:
        with socket.create_connection((DAEMON_HOST, DAEMON_PORT),
                                      timeout=STATE_TIMEOUT) as sock:
            sock.settimeout(STATE_TIMEOUT)
            sock.sendall(payload)
            sock.shutdown(socket.SHUT_WR)
            sock.recv(4096)  # 读掉响应，让 daemon 侧干净收尾
        debug(f"状态已上报: {status} / {msg}")
    except Exception as exc:
        debug(f"状态上报失败（daemon 未运行？）: {exc}")


def emit_decision(allow, reason):
    """向 stdout 输出唯一一行 Hook JSON —— 这是本进程 stdout 的全部内容。"""
    result = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "allow" if allow else "deny",
            "permissionDecisionReason": reason,
        }
    }
    # 同样绕开文本层的编码问题，直接用 UTF-8 字节写出
    line = json.dumps(result, ensure_ascii=False) + "\n"
    sys.stdout.buffer.write(line.encode("utf-8"))
    sys.stdout.buffer.flush()


# ————————————————————————— 两条处理路径 —————————————————————————


def handle_pre_tool_use(hook_input):
    """审批路径：阻塞等按钮，输出决策。"""
    tool_name = hook_input.get("tool_name", "")
    debug(f"收到审批请求: tool={tool_name}")

    try:
        action = request_decision(hook_input)
    except (TimeoutError, socket.timeout):
        # Python 3.10 起 socket.timeout 是 TimeoutError 的别名，但 3.9 上它只是
        # OSError 的子类（CLAUDE.md 声明支持 ≥3.9）。若只写 TimeoutError，3.9 的
        # 超时会落到下面的 OSError 分支，日志被打成一文不对题的「通信失败」。
        log(f"等待 daemon 决策超时（{DECISION_TIMEOUT:g} s）")
        emit_decision(False, "VibePet: approval timed out")
        return 0
    except OSError as exc:
        log(f"daemon 通信失败（{DAEMON_HOST}:{DAEMON_PORT}）: {exc}")
        emit_decision(False, "VibePet daemon unreachable")
        return 0
    except Exception as exc:
        log(f"未预期错误: {exc!r}")
        emit_decision(False, "VibePet: internal error")
        return 0

    if action == "approve":
        log(f"审批通过: {tool_name}")
        emit_decision(True, "Approved by VibePet")
    else:
        log(f"审批拒绝: {tool_name}")
        emit_decision(False, "Denied by VibePet")
    return 0


def handle_status_event(event, hook_input):
    """状态路径：上报设备状态，不阻塞、不输出。"""
    resolved = resolve_state(hook_input)
    if resolved is None:
        debug(f"事件 {event!r} 没有对应状态，忽略")
        return 0
    report_state(*resolved)
    return 0


def main():
    if sys.stdin.isatty():
        log("本脚本由 Claude Code 的 Hook 调用，不接受交互式输入。")
        log("手动测试：")
        log('  echo \'{"hook_event_name":"PreToolUse","tool_name":"Bash",'
            '"tool_input":{"command":"ls"}}\' | python hook_client.py')
        log('  echo \'{"hook_event_name":"Stop"}\' | python hook_client.py')
        return 1

    try:
        hook_input = read_hook_input()
    except Exception as exc:
        log(f"读取 Hook 输入失败: {exc}")
        emit_decision(False, "VibePet: invalid hook input")
        return 0

    event = hook_input.get("hook_event_name", "")
    if not event:
        # 兼容手动测试与不带事件名的输入：有 tool_name 就按审批处理
        event = "PreToolUse" if hook_input.get("tool_name") else ""

    if event == "PreToolUse":
        return handle_pre_tool_use(hook_input)

    return handle_status_event(event, hook_input)


if __name__ == "__main__":
    sys.exit(main())
