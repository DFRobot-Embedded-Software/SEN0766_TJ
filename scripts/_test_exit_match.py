# -*- coding: utf-8 -*-
import re
import sys

sys.path.insert(0, "scripts/_bridge_remote")
# Import without starting server: load module pieces by reading source
from pathlib import Path

src = Path("scripts/_bridge_remote/bridge.py").read_text(encoding="utf-8")
# Pull just the helpers by exec after minimal stubs
ns = {"Any": object, "Optional": lambda x=None: x}
# Execute from EXIT_PHRASES through parse helpers — stop before dashscope side effects by slicing
start = src.index("# 可选口令快路径")
end = src.index("def _tts_audio_format")
# Need imports for typing used in later - only take functions we need
chunk = src[start:end]
# Also need _normalize through _hermes_user - includes HERMES vars
# HERMES_SYSTEM is before _tts - included
# But chunk starts at EXIT - need HERMES_EXIT_REMINDER which is in chunk
exec(chunk, ns)

assert ns["is_exit_command"]("退下吧。"), "退下吧"
assert ns["is_exit_command"]("Bye bye."), "Bye bye"
assert ns["is_exit_command"]("拜拜"), "拜拜"
assert not ns["is_exit_command"]("现在湿度多少"), "湿度"
reply, exit_f = ns["parse_hermes_reply"]("EXIT_DIALOG\n好的，再见。")
assert exit_f and "再见" in reply
print("ok")
