set(srcs
    "app/mcp_sensors.c"
    "wifi/wifi_manager.c"
    "wifi/captive_portal.c"
    "web/web_server.c"
    "web/web_fs.c"
    "sensor/sensor_data.c"
    "sensor/stcc4_sensor.c"
    "sensor/c4004_sensor.c"
    "mcp/mcp_server.c"
    "mcp/mcp_radar_events.c"
)

set(yamnet_requires "")

if(CONFIG_YAMNET_ENABLE)
    list(APPEND srcs
        "audio/capture/audio_capture.c"
        "audio/stream/audio_stream.c"
        "audio/yamnet/yamnet_mel.c"
        "audio/yamnet/yamnet_pcm_in.c"
        "audio/yamnet/yamnet_events.c"
        "audio/yamnet/yamnet_classifier.cc"
    )
    list(APPEND yamnet_requires
        espressif__esp-tflite-micro
        espressif__esp-dsp
        espressif__esp_codec_dev
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "app"
        "wifi"
        "web"
        "sensor"
        "mcp"
        "audio/capture"
        "audio/stream"
        "audio/yamnet"
    REQUIRES
        nvs_flash esp_wifi esp_wifi_remote esp_hosted esp_event esp_netif
        esp_http_server spiffs json mdns driver
        espressif__mcp-c-sdk
        esp_timer
        esp_system
    PRIV_REQUIRES ${yamnet_requires}
)
