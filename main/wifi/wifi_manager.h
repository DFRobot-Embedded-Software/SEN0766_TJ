/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理模块
 *
 * 负责设备的 WiFi 生命周期管理，包括：
 * - 初始化 ESP-Hosted（P4 通过 C6 协处理器提供 WiFi）
 * - 首次启动时开启 SoftAP 热点（MCP-Sensors-Setup）供用户配网
 * - 将 WiFi 名称和密码保存到 NVS，下次启动自动连接
 * - STA 模式连接路由器，连接成功后通过回调通知上层（如启动 Web 服务）
 * - 连接失败时自动重试，超过次数后回退到 AP 配网模式
 *
 * 典型调用流程：
 * @code
 *   wifi_manager_init(on_connected, on_ap_started);
 *   if (!wifi_manager_is_configured()) {
 *       web_server_start_provision();  // AP 模式下启动配网页
 *   }
 * @endcode
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** @brief WiFi 连接成功并获取 IP 后的回调 */
typedef void (*wifi_on_connected_cb_t)(const char *ip_addr);

/** @brief 进入 AP 配网模式后的回调（用于启动配网 Web 服务） */
typedef void (*wifi_on_ap_started_cb_t)(void);

/** @brief 开始连接路由器（STA connecting） */
typedef void (*wifi_on_connecting_cb_t)(void);

/** @brief STA 连接失败并将回退 AP */
typedef void (*wifi_on_connect_failed_cb_t)(void);

/** @brief 初始化 NVS Flash（须在 app_main 最早期调用） */
esp_err_t nvs_storage_init(void);

/**
 * @brief 初始化 WiFi 子系统
 *
 * 初始化 NVS、网络接口和 WiFi 驱动。若 NVS 中已有保存的 WiFi 凭据则自动连接 STA；
 * 否则开启 AP 热点等待配网。
 *
 * @param on_connected  STA 获取 IP 后的回调
 * @param on_ap_started 进入 AP 模式后的回调
 */
esp_err_t wifi_manager_init(wifi_on_connected_cb_t on_connected, wifi_on_ap_started_cb_t on_ap_started);

/**
 * @brief 注册配网进度灯效用回调（可传 NULL）
 * @note 须在 wifi_manager_init 之后调用
 */
void wifi_manager_set_progress_cbs(wifi_on_connecting_cb_t on_connecting,
                                   wifi_on_connect_failed_cb_t on_failed);

/** @brief 开启 SoftAP 热点（SSID: MCP-Sensors-Setup，开放网络） */
esp_err_t wifi_manager_start_ap(void);

/**
 * @brief 保存凭据并以 STA 模式连接指定 WiFi
 * @param ssid     WiFi 名称
 * @param password WiFi 密码（可为空字符串）
 */
esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password);

/** @brief 检查 NVS 中是否已保存 WiFi 凭据 */
bool wifi_manager_is_configured(void);

/** @brief 进入 AP 配网模式（保留 NVS 中已保存的凭据，便于换网） */
esp_err_t wifi_manager_enter_reprovision(void);

/** @brief 清除 NVS 中的 WiFi 凭据（用于重新配网） */
esp_err_t wifi_manager_clear_config(void);

/** @brief 当前 WiFi 运行状态（供 Web API 使用） */
typedef struct {
    bool configured;  /**< NVS 中是否已保存凭据 */
    bool connected;   /**< STA 是否已获取 IP */
    bool connecting;  /**< STA 正在连接中 */
    bool ap_mode;     /**< 是否处于 AP 配网模式 */
    char ssid[33];
    char password[65];
    char ip[16];
} wifi_status_t;

/** @brief 扫描结果条目 */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
    char auth[12];
} wifi_scan_ap_t;

/** @brief 读取当前 WiFi 配置与连接状态 */
esp_err_t wifi_manager_get_status(wifi_status_t *status);

/** @brief 扫描附近 AP（阻塞，最多 max_count 条） */
esp_err_t wifi_manager_scan(wifi_scan_ap_t *out, uint16_t max_count, uint16_t *out_count);

/** @brief 断开 STA、清除凭据并回到 AP 配网 */
esp_err_t wifi_manager_disconnect(void);
