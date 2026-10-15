#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Codex Hook adapter for the existing VibePet daemon.

Codex and Claude share the approval event shape used by hook_client.py. Codex
also expects JSON output for Stop, so this adapter keeps the Claude client
unchanged and emits an empty JSON result for Codex lifecycle events.
"""

import sys

import hook_client


STATUS_EVENTS = {
    "SessionStart",
    "UserPromptSubmit",
    "PostToolUse",
    "Stop",
}


def emit_empty_json():
    """Return a valid no-op result, required by Codex Stop hooks."""
    sys.stdout.write("{}\n")
    sys.stdout.flush()


def main():
    try:
        hook_input = hook_client.read_hook_input()
    except Exception as exc:
        # Malformed input cannot safely identify an approval request. Emit no
        # decision so Codex retains its normal user-facing approval prompt.
        hook_client.log(f"读取 Codex Hook 输入失败: {exc}")
        emit_empty_json()
        return 0

    event = hook_input.get("hook_event_name", "")
    if event == "PermissionRequest":
        return hook_client.handle_approval_event(event, hook_input)

    if event in STATUS_EVENTS:
        hook_client.handle_status_event(event, hook_input)
    else:
        # Do not attach VibePet approvals to PreToolUse: Codex invokes it for
        # each matching tool call, not only for calls requiring permission.
        hook_client.debug(f"忽略未配置的 Codex 事件: {event!r}")

    emit_empty_json()
    return 0


if __name__ == "__main__":
    sys.exit(main())
