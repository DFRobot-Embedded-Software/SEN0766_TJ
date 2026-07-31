/**
 * @file device_event_types.h
 * @brief 统一设备事件 type / category 常量（便于后续扩展 env/sound/heartbeat）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 全局事件（探测范围 / 无人等）使用的 area_id */
#define DEVICE_EVENT_AREA_ID_GLOBAL 255

/** category */
#define DEVICE_EVENT_CAT_MMWAVE    "mmwave"
#define DEVICE_EVENT_CAT_ENV       "env"
#define DEVICE_EVENT_CAT_SOUND     "sound"
#define DEVICE_EVENT_CAT_HEARTBEAT "heartbeat"

/** mmwave types（对齐端侧事件表） */
#define DEVICE_EVENT_RANGE_ENTER   "range_enter"
#define DEVICE_EVENT_RANGE_EXIT    "range_exit"
#define DEVICE_EVENT_ROOM_EMPTY    "room_empty"
#define DEVICE_EVENT_AREA_ENTER    "area_enter"
#define DEVICE_EVENT_AREA_EXIT     "area_exit"
#define DEVICE_EVENT_APPROACH      "approach"       /**< 靠近/远离标签：靠近 */
#define DEVICE_EVENT_AWAY          "away"           /**< 靠近/远离标签：远离 */
#define DEVICE_EVENT_PERSON_STATIC "person_static"
#define DEVICE_EVENT_PERSON_MOVING "person_moving"

/** env types（与 /api/sensors 阈值对齐） */
#define DEVICE_EVENT_TEMP_HIGH      "temp_high"
#define DEVICE_EVENT_HUMIDITY_HIGH  "humidity_high"
#define DEVICE_EVENT_HUMIDITY_LOW   "humidity_low"
#define DEVICE_EVENT_CO2_HIGH       "co2_high"
#define DEVICE_EVENT_LUX_DARK       "lux_dark"
#define DEVICE_EVENT_LUX_BRIGHT     "lux_bright"
#define DEVICE_EVENT_SOUND_DB_HIGH  "sound_db_high"

/** sound classification types（YAMNet） */
#define DEVICE_EVENT_SOUND_CRY   "sound_cry"
#define DEVICE_EVENT_SOUND_GLASS "sound_glass"
#define DEVICE_EVENT_SOUND_ALARM "sound_alarm"

#ifdef __cplusplus
}
#endif
