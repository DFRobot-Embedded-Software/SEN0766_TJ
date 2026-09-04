"""Home Assistant 自定义集成：与 Agent 共用设备通道，不经 mcp_gw 建实体。

发现：zeroconf _mcp-sensors._udp.local. ，TXT id 作 unique_id（用户确认后入库）。
读数：CoAP GET /sensors/snapshot（约 30s），见 coordinator.py。
事件：订 mcp_sensors/<id>/event（与脚本相同），见 __init__._subscribe_mqtt。
控制：开发者工具 → 服务 mcp_sensors.set_thresholds / set_config（CoAP PUT）。

禁止：MQTT Discovery、command topic、WS 传传感器、设备侧 MCP。
"""

DOMAIN = "mcp_sensors"
CONF_HOST = "host"  # CoAP 对端 IP
CONF_PORT = "port"  # 默认 5683
CONF_DEVICE_ID = "device_id"  # 6 位 hex，与固件 device_id 相同
CONF_NAME = "name"

# 固件 mqtt_hub：mcp_sensors/<id>/event 不 retain；availability 可选
MQTT_PREFIX = "mcp_sensors"

PLATFORMS = ["sensor", "binary_sensor", "event"]
