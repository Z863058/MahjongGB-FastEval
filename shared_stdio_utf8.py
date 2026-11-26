#!/usr/bin/env python3
"""Windows 控制台 stdout/stderr UTF-8 自举（Cursor / GBK 终端防乱码）。

进程未继承 PYTHONUTF8 时，Python 3.12 在 Windows 上 stdout 常为 cp936；
中文 print 编成 GBK 字节，Cursor 集成终端按 UTF-8 解码 → U+FFFD（``）。
在入口脚本 import 后尽早调用 configure_stdio_utf8() 即可与终端代码页解耦。

mp 子进程重导入携带本模块的脚本时同样生效。
"""
from __future__ import annotations

import os
import sys

_CONFIGURED = False


def configure_stdio_utf8() -> None:
    """强制 stdout/stderr 为 UTF-8；仅 Windows 生效，可重复调用。"""
    global _CONFIGURED
    if sys.platform != "win32":
        return
    os.environ.setdefault("PYTHONUTF8", "1")
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    for name in ("stdout", "stderr"):
        stream = getattr(sys, name, None)
        if stream is None or not hasattr(stream, "reconfigure"):
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    _CONFIGURED = True


# 被 import 时即配置（eval / dashboard 等 `from shared_stdio_utf8 import configure_stdio_utf8` 路径）
configure_stdio_utf8()
