from __future__ import annotations

import os


def _f(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if raw is None or not str(raw).strip():
        return default
    return float(raw)


def _i(name: str, default: int) -> int:
    return int(_f(name, float(default)))


HOST = os.environ.get("MCP_GW_HOST", "0.0.0.0").strip() or "0.0.0.0"
PORT = _i("MCP_GW_PORT", 9080)
TOKEN = (os.environ.get("MCP_GW_TOKEN") or "").strip()
STALE_S = _f("MCP_GW_STALE_S", 90.0)
REFRESH_S = _f("MCP_GW_REFRESH_S", 30.0)
DEVICE_TIMEOUT_S = _f("MCP_GW_DEVICE_TIMEOUT_S", 15.0)

# mDNS：设备 browse `_mcp-gw._tcp` 拼出 http://{ip}:{port}{REGISTER_PATH}
MDNS_ENABLE = (os.environ.get("MCP_GW_MDNS", "1").strip().lower() not in ("0", "false", "no", "off"))
ADVERTISE_IP = (os.environ.get("MCP_GW_ADVERTISE_IP") or "").strip()
REGISTER_PATH = (os.environ.get("MCP_GW_REGISTER_PATH") or "/api/devices/register").strip() or "/api/devices/register"
if not REGISTER_PATH.startswith("/"):
    REGISTER_PATH = "/" + REGISTER_PATH

# 同机语音 bridge（设备 mDNS 发现后连此 URI）
VOICE_WS_PORT = _i("MCP_GW_VOICE_WS_PORT", 8765)
VOICE_WS_PATH = (os.environ.get("MCP_GW_VOICE_WS_PATH") or "/ws").strip() or "/ws"
if not VOICE_WS_PATH.startswith("/"):
    VOICE_WS_PATH = "/" + VOICE_WS_PATH

# 工具名前缀分隔符：{device_id}__{native_tool}
TOOL_SEP = "__"

PROTOCOL_VERSION = "2025-03-26"
SERVER_NAME = "mcp-sensors-gw"
SERVER_VERSION = "0.1.0"
