set(srcs
    "app/mcp_sensors.c"
    "wifi/wifi_manager.c"
    "wifi/captive_portal.c"
    "web/web_server.c"
    "web/web_fs.c"
    "web/web_log.c"
    "sensor/env_sensors.c"
    "sensor/radar_area.c"
    "sensor/radar_events.c"
        "sensor/board_snapshot.c"
        "sensor/sensor_settings.c"
        "sensor/stcc4_sensor.c"
    "sensor/ltr303_sensor.c"
    "sensor/sc7a20h_sensor.c"
    "sensor/c4004_sensor.c"
    "events/device_event_bus.c"
    "events/env_event_detector.c"
    "mcp/core/mcp_core.c"
    "mcp/providers/mcp_provider_env.c"
    "mcp/providers/mcp_provider_radar.c"
    "mcp/mcp_server.c"
    "ota/ota_update.c"
    "led/ws2812_led.c"
    "led/ws2812_fx.c"
    "led/rgb_ui.c"
    "audio/amp/ns4168_amp.c"
)

set(audio_requires "")
set(led_requires "")

if(CONFIG_WS2812_ENABLE)
    list(APPEND led_requires espressif__led_strip)
endif()

if(CONFIG_YAMNET_ENABLE OR CONFIG_ENV_SOUND_ENABLE)
    list(APPEND srcs "audio/capture/audio_capture.c")
    list(APPEND audio_requires espressif__esp_codec_dev)
endif()

if(CONFIG_ENV_SOUND_ENABLE)
    list(APPEND srcs "audio/sound/env_sound.c")
endif()

if(CONFIG_YAMNET_ENABLE)
    list(APPEND srcs
        "audio/stream/audio_stream.c"
        "audio/wake/esp_sr_wake.c"
        "audio/yamnet/yamnet_mel.c"
        "audio/yamnet/yamnet_pcm_in.c"
        "audio/yamnet/yamnet_events.c"
        "audio/yamnet/yamnet_classifier.cc"
    )
    list(APPEND audio_requires
        espressif__esp-tflite-micro
        espressif__esp-dsp
    )
    if(CONFIG_ESP_SR_WAKE_ENABLE)
        list(APPEND audio_requires espressif__esp-sr)
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "app"
        "wifi"
        "web"
        "sensor"
        "events"
        "mcp"
        "mcp/core"
        "mcp/providers"
        "ota"
        "led"
        "audio/amp"
        "audio/capture"
        "audio/stream"
        "audio/wake"
        "audio/yamnet"
        "audio/sound"
    REQUIRES
        nvs_flash esp_wifi esp_wifi_remote esp_hosted esp_event esp_netif
        esp_http_server spiffs json mdns driver
        espressif__mcp-c-sdk
        esp_timer
        esp_system
        app_update
        esp_partition
    PRIV_REQUIRES ${audio_requires} ${led_requires}
)
