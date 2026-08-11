from __future__ import annotations

import logging
import socket
from typing import Any

from . import config

log = logging.getLogger("mcp_gw.mdns")

SERVICE_TYPE = "_mcp-gw._tcp.local."
SERVICE_NAME = f"{config.SERVER_NAME}.{SERVICE_TYPE}"


def guess_lan_ip() -> str:
    forced = (config.ADVERTISE_IP or "").strip()
    if forced:
        return forced
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        if ip and not ip.startswith("127."):
            return ip
    except Exception:
        pass
    finally:
        s.close()
    return "127.0.0.1"


class MdnsAdvertiser:
    """asyncio 友好的 mDNS 广播（配合 uvicorn）。"""

    def __init__(self) -> None:
        self._azc: Any = None
        self._info: Any = None
        self.ip = ""
        self.enabled = False
        self.last_error = ""

    async def start(self) -> bool:
        if not config.MDNS_ENABLE:
            log.info("mDNS advertise disabled")
            return False
        try:
            from zeroconf import ServiceInfo
            from zeroconf.asyncio import AsyncZeroconf
        except ImportError:
            self.last_error = "zeroconf not installed"
            log.warning("%s; skip mDNS advertise", self.last_error)
            return False

        self.ip = guess_lan_ip()
        voice_ws = f"ws://{self.ip}:{config.VOICE_WS_PORT}{config.VOICE_WS_PATH}"
        props = {
            b"path": config.REGISTER_PATH.encode("utf-8"),
            b"mcp": b"/mcp",
            b"ws": voice_ws.encode("utf-8"),
            b"ver": b"1",
        }
        try:
            info = ServiceInfo(
                SERVICE_TYPE,
                SERVICE_NAME,
                addresses=[socket.inet_aton(self.ip)],
                port=int(config.PORT),
                properties=props,
                server=f"{config.SERVER_NAME}.local.",
            )
            azc = AsyncZeroconf()
            await azc.async_register_service(info)
            self._azc = azc
            self._info = info
            self.enabled = True
            self.last_error = ""
            log.info(
                "mDNS advertised %s at %s:%s path=%s voice=%s",
                SERVICE_NAME,
                self.ip,
                config.PORT,
                config.REGISTER_PATH,
                voice_ws,
            )
            return True
        except Exception as e:
            self.last_error = str(e)
            log.exception("mDNS advertise failed")
            await self.stop()
            return False

    async def stop(self) -> None:
        if self._azc and self._info:
            try:
                await self._azc.async_unregister_service(self._info)
            except Exception:
                pass
        if self._azc:
            try:
                await self._azc.async_close()
            except Exception:
                pass
        self._azc = None
        self._info = None
        self.enabled = False

    def status(self) -> dict:
        voice = ""
        if self.ip:
            voice = f"ws://{self.ip}:{config.VOICE_WS_PORT}{config.VOICE_WS_PATH}"
        return {
            "enabled": self.enabled,
            "service": SERVICE_TYPE.rstrip("."),
            "name": config.SERVER_NAME,
            "ip": self.ip,
            "port": config.PORT,
            "register_path": config.REGISTER_PATH,
            "voice_ws": voice,
            "last_error": self.last_error,
        }
