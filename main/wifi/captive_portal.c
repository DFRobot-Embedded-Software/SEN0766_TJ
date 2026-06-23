#include "captive_portal.h"

#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "captive";
static TaskHandle_t s_dns_task = NULL;
static int s_dns_sock = -1;

static const char *s_probe_paths[] = {
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/connecttest.txt",
    "/ncsi.txt",
    "/redirect",
    "/success.txt",
    "/canonical.html",
};

bool captive_portal_is_probe_uri(const char *uri)
{
    if (uri == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_probe_paths) / sizeof(s_probe_paths[0]); i++) {
        if (strcmp(uri, s_probe_paths[i]) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t captive_portal_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, NULL, 0);
}

static void dns_build_response(const uint8_t *query, int query_len, uint8_t *reply, int *reply_len)
{
    if (query_len < 12 || query_len > 256) {
        *reply_len = 0;
        return;
    }

    memcpy(reply, query, query_len);
    reply[2] = 0x81;
    reply[3] = 0x80;
    reply[6] = 0;
    reply[7] = 1;
    reply[8] = 0;
    reply[9] = 0;
    reply[10] = 0;
    reply[11] = 0;

    int pos = 12;
    while (pos < query_len && query[pos] != 0) {
        pos += query[pos] + 1;
    }
    pos += 5;
    if (pos > query_len) {
        *reply_len = 0;
        return;
    }

    reply[pos++] = 0xC0;
    reply[pos++] = 0x0C;
    reply[pos++] = 0x00;
    reply[pos++] = 0x01;
    reply[pos++] = 0x00;
    reply[pos++] = 0x01;
    reply[pos++] = 0x00;
    reply[pos++] = 0x00;
    reply[pos++] = 0x00;
    reply[pos++] = 0x3C;
    reply[pos++] = 0x00;
    reply[pos++] = 0x04;
    reply[pos++] = 192;
    reply[pos++] = 168;
    reply[pos++] = 4;
    reply[pos++] = 1;
    *reply_len = pos;
}

static void dns_server_task(void *arg)
{
    uint8_t rx_buf[256];
    uint8_t tx_buf[300];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    while (s_dns_sock >= 0) {
        int len = recvfrom(s_dns_sock, rx_buf, sizeof(rx_buf), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len <= 0) {
            if (s_dns_sock < 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int reply_len = 0;
        dns_build_response(rx_buf, len, tx_buf, &reply_len);
        if (reply_len > 0) {
            sendto(s_dns_sock, tx_buf, reply_len, 0,
                   (struct sockaddr *)&source_addr, socklen);
        }
    }

    s_dns_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_portal_start(void)
{
    if (s_dns_task != NULL) {
        return ESP_OK;
    }

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_dns_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_FAIL;
    }

    BaseType_t ok = xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 5, &s_dns_task);
    if (ok != pdPASS) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DNS captive portal started (all domains -> 192.168.4.1)");
    return ESP_OK;
}

void captive_portal_stop(void)
{
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
    if (s_dns_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
        s_dns_task = NULL;
    }
}
