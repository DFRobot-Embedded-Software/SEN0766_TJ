from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Any, Optional


@dataclass
class DeviceRecord:
    id: str
    base_url: str
    mcp_path: str = "/mcp_server"
    name: str = ""
    fw: str = ""
    last_seen: float = field(default_factory=time.time)
    online: bool = True
    tools: list[dict[str, Any]] = field(default_factory=list)
    last_error: str = ""

    @property
    def mcp_url(self) -> str:
        path = self.mcp_path if self.mcp_path.startswith("/") else f"/{self.mcp_path}"
        return f"{self.base_url.rstrip('/')}{path}"

    def touch(self) -> None:
        self.last_seen = time.time()
        self.online = True


class DeviceRegistry:
    """线程安全的设备注册表。"""

    def __init__(self, stale_s: float = 90.0) -> None:
        self._stale_s = stale_s
        self._lock = threading.RLock()
        self._devices: dict[str, DeviceRecord] = {}

    def upsert(
        self,
        *,
        device_id: str,
        base_url: str,
        mcp_path: str = "/mcp_server",
        name: str = "",
        fw: str = "",
    ) -> DeviceRecord:
        device_id = (device_id or "").strip().lower()
        if not device_id:
            raise ValueError("id required")
        base_url = (base_url or "").strip().rstrip("/")
        if not base_url.startswith("http://") and not base_url.startswith("https://"):
            raise ValueError("base_url must be http(s) URL")
        mcp_path = (mcp_path or "/mcp_server").strip() or "/mcp_server"
        if not mcp_path.startswith("/"):
            mcp_path = "/" + mcp_path

        with self._lock:
            rec = self._devices.get(device_id)
            if rec is None:
                rec = DeviceRecord(
                    id=device_id,
                    base_url=base_url,
                    mcp_path=mcp_path,
                    name=(name or "").strip(),
                    fw=(fw or "").strip(),
                )
                self._devices[device_id] = rec
            else:
                rec.base_url = base_url
                rec.mcp_path = mcp_path
                if name:
                    rec.name = name.strip()
                if fw:
                    rec.fw = fw.strip()
                rec.touch()
                rec.last_error = ""
            return rec

    def remove(self, device_id: str) -> bool:
        with self._lock:
            return self._devices.pop((device_id or "").strip().lower(), None) is not None

    def get(self, device_id: str) -> Optional[DeviceRecord]:
        with self._lock:
            return self._devices.get((device_id or "").strip().lower())

    def list_all(self) -> list[DeviceRecord]:
        with self._lock:
            return list(self._devices.values())

    def list_online(self) -> list[DeviceRecord]:
        now = time.time()
        with self._lock:
            out: list[DeviceRecord] = []
            for rec in self._devices.values():
                if rec.online and (now - rec.last_seen) <= self._stale_s:
                    out.append(rec)
                elif (now - rec.last_seen) > self._stale_s:
                    rec.online = False
            return out

    def set_tools(self, device_id: str, tools: list[dict[str, Any]], error: str = "") -> None:
        with self._lock:
            rec = self._devices.get((device_id or "").strip().lower())
            if not rec:
                return
            rec.tools = tools
            rec.last_error = error
            if not error:
                rec.touch()
            else:
                rec.online = False

    def mark_error(self, device_id: str, error: str) -> None:
        with self._lock:
            rec = self._devices.get((device_id or "").strip().lower())
            if not rec:
                return
            rec.last_error = error
            rec.online = False

    def to_public(self, rec: DeviceRecord) -> dict[str, Any]:
        now = time.time()
        stale = (now - rec.last_seen) > self._stale_s
        return {
            "id": rec.id,
            "name": rec.name or rec.id,
            "base_url": rec.base_url,
            "mcp_path": rec.mcp_path,
            "mcp_url": rec.mcp_url,
            "fw": rec.fw,
            "online": rec.online and not stale,
            "last_seen": rec.last_seen,
            "tool_count": len(rec.tools),
            "last_error": rec.last_error,
        }
