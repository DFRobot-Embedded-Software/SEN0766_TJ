/**
 * @file web_server.h
 * @brief HTTP Web 服务器模块
 *
 * 提供两类 Web 服务，静态页面由 SPIFFS 提供，动态数据通过 JSON API 返回：
 *
 * 统一主页（index.html），配网与监控模式共用：
 * - GET  /               → spiffs/index.html
 * - GET  /css/style.css  → 样式文件
 * - GET  /js/dashboard.js → 页面脚本
 * - GET  /api/data       → 传感器实时数据 JSON
 * - GET  /api/config     → 区域名称配置 JSON
 * - POST /api/config     → 保存区域配置
 * - POST /api/wifi       → 接收 JSON 保存 WiFi 并连接
 *
 * 使用前需先调用 web_fs_init() 挂载 SPIFFS 文件系统。
 */

#pragma once

#include "esp_err.h"

/** @brief 启动配网 Web 服务（AP 模式，启用 Captive Portal） */
esp_err_t web_server_start_provision(void);

/** @brief 启动监控 Web 服务（STA 模式） */
esp_err_t web_server_start_dashboard(void);

/** @brief 停止 HTTP 服务器 */
void web_server_stop(void);
