/**
 * @file mqtt_hub.h
 * @brief MQTT：设备 → Broker 的事件广播。HA 自定义集成与 mcp_gw 各自订阅，不经对方转发。
 *
 * 方向：仅设备发布，不下发命令。读写快照/改阈值/改雷达走 CoAP；PCM 走 WebSocket。
 *
 * Topic（<id> = MAC 后 3 字节小写 hex，见 device_id.h）：
 *   mcp_sensors/<id>/event
 *     方向：设备 → HA / 脚本
 *     QoS 1，retain=0（瞬时告警，禁止 retain，避免 HA 重启重放进门/哭声）
 *     载荷：UTF-8 JSON，与 device_event_emit 写入环形日志的对象相同，常见字段：
 *       type       必填，如 range_enter / area_enter / sound_cry / temp_high
 *       category   必填，如 mmwave / env / sound
 *       timestamp  可读时间或 uptime+Ns
 *       area_id    区域编号；全局事件为特殊值
 *       area       区域显示名（可缺）
 *       message    可读一句（可缺）
 *       person_id / active / static / payload  按事件类型可选
 *   mcp_sensors/<id>/availability
 *     方向：设备 → 订阅方（可选在线探测）
 *     连接后发 "online"（retain=1）；LWT "offline"（retain=1）
 *     这是存活，不是传感器状态。禁止在此 topic 放温度/占用 JSON。
 *
 * 禁止：
 *   homeassistant/.../config（MQTT Discovery）
 *   mcp_sensors/<id>/state 或任何 retained 快照
 *   .../set、.../command、订阅后改灯改阈值
 *   二进制 PCM、整包 snapshot（那是 CoAP GET /sensors/snapshot）
 *
 * Broker 只来自 _mcp-gw TXT mqtt=，不是 MQTT 自己做设备发现。
 * 叶子角色见 CONFIG_COMM_ROLE_LEAF：publish 时短连再断。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 发现 broker（mDNS）后长连；尚未发现则后台重试，不报失败 */
esp_err_t mqtt_hub_start(void);
void mqtt_hub_stop(void);

/**
 * 发布一条 event JSON。
 * 调用方：device_event_emit 成功写入本地日志之后。
 * 未连接：先排队，连上后再发（网页 recent_events 是本地缓存，不代表已上 MQTT）。
 */
void mqtt_hub_publish_event(const char *json);

bool mqtt_hub_is_connected(void);
/** 已发现或正在使用的 broker URI；未发现则为空串 */
const char *mqtt_hub_broker_uri(void);
/** 例如 mcp_sensors/f20e08/event；尚未算出 id 则为空串 */
const char *mqtt_hub_event_topic(void);

#ifdef __cplusplus
}
#endif
