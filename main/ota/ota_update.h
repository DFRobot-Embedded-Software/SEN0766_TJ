/**
 * @file ota_update.h
 * @brief OTA 固件升级：封装 esp_ota_ops，供 Web /api/ota 直传 .bin 使用
 *
 * 分区表为双槽（ota_0/ota_1 + otadata），配合 App Rollback：
 * 新固件联网成功前不会被标记为有效，异常时下次重启自动回退旧固件。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char version[32];        /**< esp_app_desc_t.version（当前运行固件） */
    char running_label[16];  /**< 当前运行分区名，如 ota_0/ota_1 */
    bool pending_verify;     /**< 是否处于回滚保护待验证状态 */
} ota_info_t;

/** @brief 开始一次 OTA：定位下一个可写分区并擦除 */
esp_err_t ota_update_begin(void);

/** @brief 写入一段固件数据（可多次调用） */
esp_err_t ota_update_write(const uint8_t *data, size_t len);

/** @brief 中止当前 OTA（写入出错或客户端断开时调用） */
void ota_update_abort(void);

/** @brief 校验并提交为下次启动分区；成功后调用方负责稍后重启设备 */
esp_err_t ota_update_end(void);

/** @brief 查询当前运行固件版本、分区与回滚待验证状态 */
esp_err_t ota_update_get_info(ota_info_t *out);

/** @brief 确认新固件工作正常，取消回滚保护（联网/服务启动成功后调用） */
void ota_update_mark_valid(void);

#ifdef __cplusplus
}
#endif
