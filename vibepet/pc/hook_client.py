#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VibePet Hook 客户端（电脑端 · 短生命周期进程）

━━━━━━━━━━━━━━━ 新手导读：这个文件是干嘛的 ━━━━━━━━━━━━━━━

你在 Claude Code 里让它跑一条命令（比如删掉某个目录），它会先「问一下」
有没有人反对。这个「问」的动作就是把它调起来（Hook），并把事件详情写进
本进程的 stdin。本脚本要做的就三件事：

  1. 把这件事转告给桌面上那个小设备（经 daemon，走本地网络）；
  2. 卡住不动，等你在设备上按「批准」或「拒绝」；
  3. 把结果写成一行 JSON 从 stdout 吐回去，Claude Code 读了才知道该不该执行。

第 2 步「卡住不动」是关键：Claude Code 会一直等我们回话，所以等待期间
它是暂停的 —— 「用物理按钮审批」能生效，靠的正是这一点。

**两条差别很大的路**（同一个文件被挂在多个事件上，按事件名分派）：

  · PreToolUse ── 要审批：走上面那三步，会阻塞、会输出决策；
  · 其它事件 ── 只报状态：告诉设备「AI 正在忙 / 空闲了 / 出错了」，
    发完就走，不等回复，也绝不输出任何东西。

这个差异是**故意的**，不是疏忽：审批路径慢一点无所谓（本来就在等人按键），
状态路径必须飞快（它挂在每一次工具调用上，多耗一秒就是拖慢你一秒）。

**一条铁律**：stdout 只准出现审批的那一行 JSON。
Claude Code 把 stdout 当作决策来解析，混进一句调试日志它就读不懂了，
审批链路会直接断掉。所以本文件所有「打印」都写去 stderr（给人看），
只有 emit_decision() 才碰 stdout（给程序看）。

───────────────── 以下为技术细节 ─────────────────

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

# ─────────────── 先解决一个 Windows 特有的坑 ───────────────
#
# Python 在中文 Windows 上默认按 GBK 编码往屏幕上写字。只要日志里出现一个
# GBK 装不下的字符（emoji、某些生僻字），它就会抛 UnicodeEncodeError，
# 整个进程当场崩掉。而本进程崩掉又来不及输出决策的话，Claude Code 收不到
# 答复，审批链路就断了 —— 为一行日志赔上整条链路，太亏。
#
# 所以这里强制把 stderr 的编码改成 UTF-8（errors="backslashreplace" 是兜底：
# 万一还有装不下的字符，就写成 \xXX 而不是抛异常）。外面套 try 是因为极少数
# 环境下 stderr 可能不支持这样改 —— 改不了就算了，不能因此让脚本起不来。
try:
    # typeshed 把 sys.stderr 标注成 TextIO 协议（协议里没有 reconfigure），
    # 运行时实际是 TextIOWrapper，该方法一定存在 —— 属类型存根局限，
    # 故 type: ignore；真遇到不支持的流对象由 except 兜底。
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")  # type: ignore
except Exception:
    pass


# 打日志（给人看的）。
#
# 为什么每次都写 file=sys.stderr 这么麻烦？因为 print 默认打到 stdout，而
# stdout 是本进程的「协议通道」—— Claude Code 只认那里的决策 JSON，多一个字
# 都会让它解析失败。所以规矩是死的：给人看的走 stderr，给机器看的走 stdout。
#
# 外面再套一层 try：写日志失败（比如 stderr 被关掉了）也不能崩 —— 日志只是
# 副产品，不值得为它牺牲审批主流程。
def log(message):
    """日志一律走 stderr —— stdout 是 Claude Code 解析决策的通道。"""
    try:
        print(f"[vibepet] {message}", file=sys.stderr)
    except Exception:
        pass  # 日志失败绝不能影响审批决策


# 调试日志：只有设了环境变量 VIBEPET_DEBUG=1 时才真的打。
# 平时静默 —— 正常使用时不该被这些细节刷屏。
def debug(message):
    if DEBUG:
        log(message)


# 从环境变量里读一个数字，读不到或是读不懂，就用默认值。
#
# 为什么要这么小心？因为环境变量是用户手敲的，可能是 "abc"、可能是空串。
# 直接 int(raw) 会抛异常 → 进程崩 → 审批链路断。所以这里宁可退回默认值、
# 打一条日志提醒，也不让脚本倒下。
#
# 参数 cast 是「你要转成哪种数字类型」，调用时把 int 或 float 传进来。
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


# ─────────────── 下面这几个常量：想调行为，改这里 ───────────────

# daemon 在哪。默认 127.0.0.1 意思是「就在本机」—— daemon 和 Claude Code
# 跑在同一台电脑上，走回环地址不经过网卡，最快也最不容易出岔子。
DAEMON_HOST = os.environ.get("VIBEPET_HOST", "127.0.0.1")
DAEMON_PORT = _env_number("VIBEPET_PORT", 8765, int)

# 等用户按下按钮的最长时间。
#
# 这里是 130 秒，而 daemon 那边是 120 秒 —— 多出来的 10 秒是**故意的**：
# 我们希望「等太久了，按拒绝处理」这个判断由 daemon 来做（它是管审批的
# 那一方），客户端只是多等一会儿兜底。要是客户端抢先后悔，daemon 那边还
# 傻等着，屏幕上就会留一张其实已经作废的审批卡。
# daemon 端审批超时 120 s（设计文档 5.4）。
DECISION_TIMEOUT = _env_number("VIBEPET_TIMEOUT", 130.0, float)

# 连上 daemon 最多等多久。连不上就没戏了，早点失败、早点降级成拒绝。
CONNECT_TIMEOUT = _env_number("VIBEPET_CONNECT_TIMEOUT", 5.0, float)

# 上报状态最多等多久。这里必须是 1 秒这种很小的值：
# 状态上报挂在每一次工具调用之后，多拖一秒就是实打实地拖慢你每一次调用。
# 而屏幕上显示的状态只是锦上添花，为它牺牲流畅度不划算 —— 宁可不报。
STATE_TIMEOUT = _env_number("VIBEPET_STATE_TIMEOUT", 1.0, float)

# 设了 VIBEPET_DEBUG=1 就打开调试日志；其它任何值（含没设）都算关。
DEBUG = os.environ.get("VIBEPET_DEBUG", "").strip() not in ("", "0")


# ————————————————————————— 事件 → 状态映射 —————————————————————————
#
# 这一节回答一个问题：Claude Code 那边发生了某件事，屏幕上该显示成什么？
#
# 设备能显示六种状态，其中「失联」是设备自己判断的（收不到心跳就显示
# LOST），剩下五种在这里决定。设计文档 3.3。

# 大部分事件都是「一对一」的简单对应，直接查这张表就够了。
# 左边是 Claude Code 的事件名，右边是（状态, 屏幕上跟的那行小字）。
#
# 留意最后那个 Stop：它表示「AI 把这一轮任务干完了」，
# 所以对应的是 done（完成），不是 working。
SIMPLE_EVENT_MAP = {
    "SessionStart":     ("idle",    "会话开始"),
    "SessionEnd":       ("idle",    "会话结束"),
    "UserPromptSubmit": ("working", "思考中"),
    "SubagentStop":     ("working", "子任务完成"),
    "PreCompact":       ("working", "压缩上下文"),
    "Stop":             ("done",    "任务完成"),
}


# 猜一猜：这次工具调用是成功还是失败？
#
# 为什么说「猜」？因为 Claude Code 回给我们的 tool_response 结构五花八门 ——
# 跑命令的、读文件的、搜代码的，字段各不相同，没有一个稳定的「到底出没出错」
# 标志位。所以这里只认两个明确信号（is_error 为真、error 字段非空），
# 认不出来就一律按成功算。
#
# 这个「宁可漏报」的取舍是有理由的：把成功误报成红色 ERROR，用户会以为出了
# 事、跑去排查，结果白忙一场 —— 那比漏报更烦人。
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


# 总入口：给一个 Hook 事件，回答「屏幕上该显示成什么」。
#
# 返回值是二元组 (状态, 小字)；返回 None 表示「这件事不用上报」。
# None 很重要 —— Claude Code 的事件有几十种，我们只关心其中几种，
# 其余的安静跳过就行，别去打扰设备。
def resolve_state(hook_input):
    """把 Hook 事件映射成 (status, msg)；返回 None 表示该事件不上报。"""
    event = hook_input.get("hook_event_name", "")

    # PostToolUse 是「某个工具跑完了」，同一件事要分成两种情况：跑成功了
    # 显示 working（AI 还在继续干活），跑砸了显示 error。它有条件分支，
    # 所以没法塞进上面那张表，得单独处理。
    if event == "PostToolUse":
        # 工具名万一取不到，用「工具」二字兜底，免得屏幕上冒出 "None 完成"。
        tool = str(hook_input.get("tool_name") or "工具")
        if looks_like_error(hook_input.get("tool_response")):
            return "error", f"{tool} 出错"
        return "working", f"{tool} 完成"

    # 其余事件查表；表里没有的会返回 None（= 不上报）。
    return SIMPLE_EVENT_MAP.get(event)


# ————————————————————————— 与 daemon 通信 —————————————————————————
#
# 这一节是「传话筒」：把 Claude Code 给的事件递给 daemon，再把 daemon 的
# 答复拿回来。三个函数各管一段：读进来 → 问决策 → 报状态。


# 第一件事：把 Claude Code 塞进 stdin 的东西读出来，变成 Python 字典。
def read_hook_input():
    """从 stdin 读取 Hook 传入的 JSON。

    显式走 buffer + UTF-8 解码：中文 Windows 的 locale 编码多为 GBK，
    sys.stdin.read() 遇到带中文的 tool_input（中文路径、中文命令描述）
    会抛 UnicodeDecodeError，等于每次调用都被降级成 deny。
    """
    # 用 sys.stdin.buffer 拿到「原始字节」，再自己按 UTF-8 解码；而不是让
    # Python 按系统默认编码去猜 —— 中文 Windows 上默认是 GBK，一猜就错。
    raw = sys.stdin.buffer.read()
    if not raw.strip():
        # 一个字都没收到。抛出去，上层会把它降级成「拒绝」，安全第一。
        raise ValueError("stdin 为空，未收到 Hook 输入")
    data = json.loads(raw.decode("utf-8"))
    if not isinstance(data, dict):
        # 是合法 JSON，但不是我们要的「对象」（比如是个数组、一个数字）。
        # 必须在这里就拦掉：放过去的话，main() 里 data.get(...) 会抛
        # AttributeError，进程以退出码 1 崩掉 —— 而「崩掉、什么决策都没输出」
        # 在 Claude Code 眼里不等于拒绝，等于放行。这就违反「降级一律拒绝」。
        raise ValueError(f"Hook 输入必须是 JSON 对象，实际是 {type(data).__name__}")
    return data


# 第二件事：把事件转给 daemon，然后卡住，等用户按按钮。
#
# 为什么不自己直接连蓝牙？两个原因：
#   1. 每次 Hook 触发都会新起一个本进程，而蓝牙连接要花好几秒才连得上，
#      每次都重连根本来不及；
#   2. 蓝牙连接同时只能有一个「主人」，多个进程抢会乱套。
# 所以真正抱着蓝牙的是常驻的 daemon，我们只负责给它发个请求、等回话。
#
# 返回 'approve' 或 'deny'。出任何错都不在这里处理，直接往上抛给 main()，
# 由它统一降级成 deny。
def request_decision(hook_input):
    """把 Hook 输入转发给 daemon，阻塞等待物理按钮决策。

    返回 'approve' 或 'deny'；异常向上抛给 main() 统一降级。
    """
    # 转成 UTF-8 字节再发。ensure_ascii=False 的作用是让中文原样发出去，
    # 而不是变成 中文 这种转义（两者都合法，但前者更短、抓包时也好读）。
    # 末尾补一个 \n，因为协议规定「一条消息占一行」。
    payload = json.dumps(hook_input, ensure_ascii=False).encode("utf-8") + b"\n"

    # 连上 daemon。连不上会抛异常（最多等 CONNECT_TIMEOUT = 5 秒），
    # 交给上层降级成拒绝。
    with socket.create_connection((DAEMON_HOST, DAEMON_PORT), timeout=CONNECT_TIMEOUT) as sock:
        # 连上之后把超时放宽成「等按钮」的时长 —— 刚才那 5 秒只管「连得上吗」，
        # 现在这 130 秒管的是「等你慢悠悠地伸手按按钮」。
        sock.settimeout(DECISION_TIMEOUT)
        sock.sendall(payload)

        # ↓↓ 这一行是整条链路的关键，也是最容易被忽略的一行 ↓↓
        #
        # 它叫「半关闭」：只关掉「我这边往外发」这一个方向，等于告诉对方
        # 「我的话说完了」，但**保留接收能力**。
        #
        # 为什么非要有它？因为 daemon 那头是这么读请求的：
        #     raw = await reader.read(4096)
        # read() 的脾气是「读不到结尾（EOF）就不返回」。我们不发这个
        # 「说完了」的信号，daemon 就一直等我们继续说；我们又在等它回答 ——
        # 两边互等，审批卡死到超时才解开。
        # （消息末尾那个 \n 是给另一种实现留的余地：用 readline() 读的实现
        #  靠 \n 就认为读完了，不需要半关闭也能正常工作。）
        sock.shutdown(socket.SHUT_WR)

        # 开始收 daemon 的回复。
        #
        # 网络数据是一段一段到的，不保证一次收全，所以要「攒一攒再试」：
        # 每收到一段就拼到 buf 后面，然后试着按 JSON 解析 —— 解析成功说明
        # 攒够了，解析失败说明还差一截，接着收。
        buf = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:  # 收到空 = 对方把连接关了，不会再有数据
                break
            buf += chunk
            try:
                json.loads(buf.decode("utf-8"))
                break  # 收全了一条完整 JSON
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue  # 数据分片，继续读

    if not buf:
        raise ConnectionError("daemon 未返回任何数据")

    # 取回复里的 action 字段。看不懂的、或者不是 approve/deny 的，一律按
    # deny 处理 —— 「拿不准就拒绝」是贯穿整个项目的原则。
    action = json.loads(buf.decode("utf-8")).get("action")
    if action not in ("approve", "deny"):
        log(f"daemon 返回未知决策 {action!r}，按 deny 处理")
        return "deny"
    return action


# 第三件事：上报状态（就是导读里说的那条「只报状态、不输出」的路）。
#
# 这个函数的性格是「怎么失败都无所谓」：连不上 daemon？跳过。超时？跳过。
# 发到一半断了？跳过。**全程安静**，一个异常都不往外抛。
#
# 为什么能这么无所谓？因为它挂在每一次工具调用之后。要是为了给屏幕更新
# 一次状态、害你多等一秒，那就本末倒置了 —— 屏幕只是锦上添花。
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
            sock.shutdown(socket.SHUT_WR)   # 同 request_decision：告诉对方「说完了」
            sock.recv(4096)                 # 读掉回应，让 daemon 那边能干净收尾
        debug(f"状态已上报: {status} / {msg}")
    except Exception as exc:
        # 注意这里只调 debug（要开 VIBEPET_DEBUG 才看得见），不是 log。
        # 因为 daemon 没开着是常事（很多人只想用 Claude Code，不摆设备），
        # 每次工具调用都吼一行「连不上」会把人烦死。
        debug(f"状态上报失败（daemon 未运行？）: {exc}")


# 最后一步：把决策交给 Claude Code。
#
# 这是**本进程里唯一允许写 stdout 的地方**。格式由 Claude Code 规定：
# 外层 hookSpecificOutput 里放 hookEventName（固定写 "PreToolUse"）、
# permissionDecision（allow 还是 deny），再加一句给用户看的原因。
def emit_decision(allow, reason):
    """向 stdout 输出唯一一行 Hook JSON —— 这是本进程 stdout 的全部内容。"""
    result = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "allow" if allow else "deny",
            "permissionDecisionReason": reason,
        }
    }
    # 又是编码问题：不直接 print，而是拿到底层字节流、自己按 UTF-8 写。
    # 这样无论系统默认编码是什么，发出去的一定是 UTF-8 字节。
    line = json.dumps(result, ensure_ascii=False) + "\n"
    sys.stdout.buffer.write(line.encode("utf-8"))
    # 立刻推出去，别留在缓冲区里 —— 本进程马上就要结束了。
    sys.stdout.buffer.flush()


# ————————————————————————— 两条处理路径 —————————————————————————


# 路径一：审批。会一直卡在这里，直到用户按了按钮（或者超时、出错）。
#
# 这个函数的核心是「再糟也要给个答复」：不管中途发生什么，最后都一定会调用
# 一次 emit_decision()。因为对 Claude Code 来说，「不说话」等于放行，而我们
# 宁可错杀不可放过 —— 所以下面每个 except 分支的结局都是 emit_decision(False)。
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
        # 连不上 daemon —— 最常见的情况是「忘了启动 daemon」。
        log(f"daemon 通信失败（{DAEMON_HOST}:{DAEMON_PORT}）: {exc}")
        emit_decision(False, "VibePet daemon unreachable")
        return 0
    except Exception as exc:
        # 其它意料之外的错误。照样要给答复，不能沉默。
        log(f"未预期错误: {exc!r}")
        emit_decision(False, "VibePet: internal error")
        return 0

    # 走到这里说明顺利拿到决策了，把结果告诉 Claude Code。
    # （注意：只有这里才可能输出 allow —— 上面所有失败分支一律是 deny。）
    if action == "approve":
        log(f"审批通过: {tool_name}")
        emit_decision(True, "Approved by VibePet")
    else:
        log(f"审批拒绝: {tool_name}")
        emit_decision(False, "Denied by VibePet")
    return 0


# 路径二：只报状态。发完就返回 —— 不等回复、不输出任何东西。
def handle_status_event(event, hook_input):
    """状态路径：上报设备状态，不阻塞、不输出。"""
    resolved = resolve_state(hook_input)
    if resolved is None:
        # 这个事件我们不关心（比如 Notification），安静跳过就行。
        debug(f"事件 {event!r} 没有对应状态，忽略")
        return 0
    report_state(*resolved)   # 把 (状态, 小字) 摊开，当成两个参数传进去
    return 0


# 程序入口：先判断这次是被「要审批」叫起来的、还是被「报个状态」叫起来的，
# 再分派给上面两条路径之一。
def main():
    # 有人在终端里直接敲了 python hook_client.py（没有喂输入）。
    # 给一段使用说明，免得一脸茫然。（isatty 是问「这是不是个交互终端」）
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
        # 输入读不出来（空的、坏的、不是 JSON 对象……）。此时我们并不知道
        # 这是个什么事件 —— 但输出一行「拒绝」是安全的：万一它其实是审批
        # 请求，正好拒绝对了；万一它是状态事件，Claude Code 对多出来的这行
        # 输出一般也不予理会。反过来「什么都不说」才是危险的（等于放行）。
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


# Python 的固定写法：只有「直接运行本文件」时才执行 main()。
# 被别的程序 import 时不会自动跑起来（测试里就是这么用的）。
if __name__ == "__main__":
    sys.exit(main())   # 用 main() 的返回值（0 / 1）作为进程退出码
