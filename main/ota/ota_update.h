/**
 * @file ota_update.h
 * @brief OTA：App 双槽固件 + SPIFFS 网页分区（storage）在线升级
 *
 * App：ota_0/ota_1 + otadata，支持回滚保护。
 * UI：直接擦写 data/spiffs 分区 label=storage（build/storage.bin）。
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
    uint32_t storage_size;   /**< storage 分区字节数（网页镜像上限） */
} ota_info_t;

/** @brief 开始一次 App OTA：定位下一个可写分区并擦除 */
esp_err_t ota_update_begin(void);

/** @brief 写入一段 App 固件数据（可多次调用） */
esp_err_t ota_update_write(const uint8_t *data, size_t len);

/** @brief 中止当前 App OTA */
void ota_update_abort(void);

/** @brief 校验并提交为下次启动分区；成功后调用方负责稍后重启 */
esp_err_t ota_update_end(void);

/**
 * @brief 开始网页分区升级：卸载 SPIFFS 并擦除 storage
 * @param image_size 上传镜像总长度（须 ≤ 分区大小）
 */
esp_err_t ota_update_spiffs_begin(size_t image_size);

/** @brief 写入一段 storage.bin 数据 */
esp_err_t ota_update_spiffs_write(const uint8_t *data, size_t len);

/** @brief 中止网页升级（分区可能已损坏，需 USB 重刷 SPIFFS） */
void ota_update_spiffs_abort(void);

/** @brief 写完后重新挂载 SPIFFS */
esp_err_t ota_update_spiffs_end(void);

/** @brief 查询当前固件与 storage 分区信息 */
esp_err_t ota_update_get_info(ota_info_t *out);

/** @brief 确认新固件工作正常，取消回滚保护 */
void ota_update_mark_valid(void);

#ifdef __cplusplus
}
#endif
