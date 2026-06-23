/**
 * @file web_fs.h
 * @brief SPIFFS 静态文件系统模块
 *
 * 负责挂载 Flash 中的 SPIFFS 分区，并向 HTTP 客户端发送静态 Web 资源。
 * 网页文件位于项目 spiffs/ 目录，编译时自动打包烧录：
 *
 * @code
 *   spiffs/
 *   ├── index.html        统一主页（设备数据 / 区域配置 / WiFi）
 *   ├── css/style.css     样式
 *   └── js\*.js           页面脚本
 * @endcode
 *
 * 修改 spiffs/ 下的文件后执行 idf.py build flash 或 idf.py spiffs-flash 即可更新。
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** @brief SPIFFS 挂载路径，文件访问格式：/spiffs/index.html */
#define WEB_FS_MOUNT_POINT "/spiffs"

/**
 * @brief 挂载 SPIFFS 分区
 *
 * 分区名 storage，首次挂载失败时自动格式化。
 * 必须在 web_server 启动前调用。
 */
esp_err_t web_fs_init(void);

/**
 * @brief 读取文件并通过 HTTP 响应发送给客户端
 * @param req      HTTP 请求句柄
 * @param filepath 完整路径，如 "/spiffs/index.html"
 * @return ESP_OK 发送成功
 */
esp_err_t web_fs_send_file(httpd_req_t *req, const char *filepath);
