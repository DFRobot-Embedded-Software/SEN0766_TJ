/**
 * @file coap_hub.h
 * @brief CoAP Server：HA 与脚本都是 Client。问答题，不做 Observe（事件走 MQTT）。
 *
 * 传输：UDP :5683。Content-Format 应用/JSON（UTF-8）。叶子设备不实现本 Server。
 * 方向：GET 读现状，PUT 改配置。设备不应主动推包。
 *
 * 资源：
 *   GET  /id
 *     出：{"id":"<6位hex>"}  配网前即可算，与 mDNS TXT id 相同
 *   GET  /sensors/snapshot
 *     出：当前读数（温度/湿/CO2/lux/areas/sound_* /thresholds）
 *     用途：HA 轮询实体、Agent 工具 get_snapshot。不是事件流。
 *   GET  /sensors/thresholds
 *     出：sensor_settings JSON（与网页 POST /api/sensors 的 sensors 对象同形）
 *   PUT  /sensors/thresholds
 *     入：上述对象，或外包一层 {"sensors":{...}}
 *     出：2.04 + {"ok":true}
 *   GET  /sensors/config
 *     出：雷达 tags[] / detection_range（坐标单位米）
 *   PUT  /sensors/config
 *     入：与网页 POST /api/config 相同：
 *       {"action":"set_range","coords":[[x,y]x4]}
 *       {"action":"add_tag"|"update_tag","id"?, "strategy","name","coords"}
 *       {"action":"remove_tag","id":n}
 *   GET  /sensors/events
 *     出：本地环形日志（补拉）。即时告警仍应订 MQTT，不要靠轮询本资源当门铃。
 *   GET  /.well-known/core
 *     出：link-format 资源列表
 *
 * 禁止：Observe；传 PCM；用 PUT 当 MQTT command 的替身以外的新 RPC；
 *       在路径里塞温度（那是 snapshot 字段）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t coap_hub_start(void);
void coap_hub_stop(void);

#ifdef __cplusplus
}
#endif
