/**
 * @file captive_portal.c
 * @brief DNS:53 与 mDNS:5353 都答 192.168.4.1。加入 224.0.0.251 IGMP 才能收到 mDNS 问询。
 */
#include "captive_portal.h"

#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/igmp.h"

static const char *TAG = "captive";
static TaskHandle_t s_dns_task = NULL;
static int s_dns_sock = -1;
static int s_mdns_sock = -1;
static char s_redirect[64] = "http://192.168.4.1/";

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
    httpd_resp_set_hdr(req, "Location", s_redirect);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, NULL, 0);
}

void captive_portal_set_redirect(const char *url)
{
    if (url && url[0]) {
        strncpy(s_redirect, url, sizeof(s_redirect) - 1);
        s_redirect[sizeof(s_redirect) - 1] = '\0';
    }
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

static int open_udp(int port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void dns_server_task(void *arg)
{
    (void)arg;
    uint8_t rx_buf[256];
    uint8_t tx_buf[300];
    struct sockaddr_in source_addr;

    while (s_dns_sock >= 0 || s_mdns_sock >= 0) {
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = -1;
        if (s_dns_sock >= 0) {
            FD_SET(s_dns_sock, &fds);
            maxfd = s_dns_sock;
        }
        if (s_mdns_sock >= 0) {
            FD_SET(s_mdns_sock, &fds);
            if (s_mdns_sock > maxfd) {
                maxfd = s_mdns_sock;
            }
        }
        if (maxfd < 0) {
            break;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        int n = select(maxfd + 1, &fds, NULL, NULL, &tv);
        if (n <= 0) {
            continue;
        }
        int socks[2] = {s_dns_sock, s_mdns_sock};
        for (int i = 0; i < 2; i++) {
            int s = socks[i];
            if (s < 0 || !FD_ISSET(s, &fds)) {
                continue;
            }
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(s, rx_buf, sizeof(rx_buf), 0,
                               (struct sockaddr *)&source_addr, &socklen);
            if (len <= 0) {
                continue;
            }
            int reply_len = 0;
            dns_build_response(rx_buf, len, tx_buf, &reply_len);
            if (reply_len > 0) {
                sendto(s, tx_buf, reply_len, 0, (struct sockaddr *)&source_addr, socklen);
            }
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

    s_dns_sock = open_udp(53);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        return ESP_FAIL;
    }
    s_mdns_sock = open_udp(5353);
    if (s_mdns_sock >= 0) {
        ip4_addr_t mcast;
        IP4_ADDR(&mcast, 224, 0, 0, 251);
        struct ip_mreq mreq = {0};
        mreq.imr_multiaddr.s_addr = mcast.addr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(s_mdns_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            ESP_LOGW(TAG, "mDNS IGMP join failed");
        }
    } else {
        ESP_LOGW(TAG, "mDNS hijack socket failed (DNS still on)");
    }

    BaseType_t ok = xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task);
    if (ok != pdPASS) {
        close(s_dns_sock);
        s_dns_sock = -1;
        if (s_mdns_sock >= 0) {
            close(s_mdns_sock);
            s_mdns_sock = -1;
        }
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "captive DNS:53 + mDNS:5353 -> 192.168.4.1 (AP only)");
    return ESP_OK;
}

void captive_portal_stop(void)
{
    int dns = s_dns_sock;
    int mdns = s_mdns_sock;
    s_dns_sock = -1;
    s_mdns_sock = -1;
    if (dns >= 0) {
        close(dns);
    }
    if (mdns >= 0) {
        close(mdns);
    }
    if (s_dns_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
        s_dns_task = NULL;
    }
}
