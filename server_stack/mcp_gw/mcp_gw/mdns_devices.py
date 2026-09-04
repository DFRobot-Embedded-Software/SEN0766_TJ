"""浏览设备 mDNS：服务 _mcp-sensors._udp.local.，端口即 CoAP 5683。

TXT：id（必填）、ver、name。不读温度。
长驻 AsyncZeroconf，不用每 8 秒新建再 sleep。优先 IPv4。
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from .device_mqtt import push_if_missing
from .registry import DeviceRegistry

log = logging.getLogger("mcp_gw.mdns_dev")

SERVICE = "_mcp-sensors._udp.local."


def _ipv4(addrs: list[str]) -> str:
    for a in addrs:
        if ":" not in a:
            return a
    return addrs[0] if addrs else ""


class MdnsDeviceBrowser:
    def __init__(self, registry: DeviceRegistry) -> None:
        self.registry = registry
        self._azc: Any = None
        self._browser: Any = None

    async def start(self) -> None:
        try:
            from zeroconf import ServiceStateChange
            from zeroconf.asyncio import AsyncServiceBrowser, AsyncZeroconf
        except ImportError:
            log.warning("zeroconf not installed; skip device browse")
            return

        self._azc = AsyncZeroconf()

        def on_change(*_args: Any, **kwargs: Any) -> None:
            # python-zeroconf 用关键字 zeroconf= 回调；旧版才是位置参数
            state_change = kwargs.get("state_change")
            service_type = kwargs.get("service_type") or ""
            name = kwargs.get("name") or ""
            if len(_args) >= 4:
                state_change = _args[3]
                service_type = _args[1]
                name = _args[2]
            if state_change not in (ServiceStateChange.Added, ServiceStateChange.Updated):
                return
            if not service_type or not name:
                return
            try:
                loop = asyncio.get_running_loop()
            except RuntimeError:
                return
            loop.create_task(self._resolve(service_type, name), name=f"mdns-res-{name}")

        self._browser = AsyncServiceBrowser(
            self._azc.zeroconf,
            [SERVICE],
            handlers=[on_change],
        )
        log.info("browsing %s", SERVICE)

    async def stop(self) -> None:
        if self._browser:
            try:
                await self._browser.async_cancel()
            except Exception:
                pass
            self._browser = None
        if self._azc:
            try:
                await self._azc.async_close()
            except Exception:
                pass
            self._azc = None

    async def _resolve(self, service_type: str, name: str) -> None:
        if not self._azc:
            return
        try:
            from zeroconf.asyncio import AsyncServiceInfo
        except ImportError:
            return
        info = AsyncServiceInfo(service_type, name)
        await info.async_request(self._azc.zeroconf, 3000)
        addrs = list(info.parsed_addresses() or [])
        ip = _ipv4(addrs)
        if not ip:
            return
        props: dict[str, str] = {}
        for k, v in (info.properties or {}).items():
            key = k.decode() if isinstance(k, bytes) else str(k)
            val = v.decode() if isinstance(v, bytes) else ("" if v is None else str(v))
            props[key] = val
        did = str(props.get("id") or "").strip().lower()
        if not did:
            log.warning("mDNS %s missing TXT id", name)
            return
        port = int(info.port or 5683)
        disp = str(props.get("name") or did)
        self.registry.upsert(
            device_id=did,
            base_url=f"http://{ip}",
            name=disp,
            fw=f"{ip}:{port}",
            coap_host=ip,
            coap_port=port,
        )
        log.info("mDNS hub id=%s coap=%s:%s", did, ip, port)
        asyncio.create_task(push_if_missing(ip, force=False), name=f"mqtt-push-{did}")
