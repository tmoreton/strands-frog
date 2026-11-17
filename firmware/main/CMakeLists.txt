set(frog_frames "${CMAKE_CURRENT_SOURCE_DIR}/../assets/frog_frames.rgb565")
set(frog_theme_overlays "${CMAKE_CURRENT_SOURCE_DIR}/../assets/frog_theme_overlays.argb8888")
set(frog_frames_asm "${CMAKE_CURRENT_BINARY_DIR}/frog_frames.S")
idf_component_register(
    SRCS "main.c" "voice_client.c" "long_transcript.c" "board_ui.c" "frog_sprites.c" "wifi_setup.c" "portal_dns.c"
         "usb_setup.c" "${frog_frames_asm}"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "wifi_setup.html"
    EMBED_FILES "../assets/strands-frog.svg"
    PRIV_REQUIRES bootloader_support esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_event esp_hw_support
                  esp_http_client esp_http_server esp_netif esp_timer esp_wifi json lwip mbedtls nvs_flash
)

configure_file("frog_frames.S.in" "${frog_frames_asm}" @ONLY)
set_source_files_properties("${frog_frames_asm}" PROPERTIES OBJECT_DEPENDS "${frog_frames};${frog_theme_overlays}")

set(cloud_settings "${CMAKE_CURRENT_SOURCE_DIR}/cloud_settings.h")
if(NOT EXISTS "${cloud_settings}")
    message(FATAL_ERROR "Copy main/cloud_settings.example.h to main/cloud_settings.h, then set your HTTPS /voice URL and device token.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cloud_settings}")
file(STRINGS "${cloud_settings}" cloud_url REGEX "^[ \t]*#[ \t]*define[ \t]+STRANDS_CLOUD_URL[ \t]+")
if(NOT cloud_url MATCHES "^[ \t]*#[ \t]*define[ \t]+STRANDS_CLOUD_URL[ \t]+\"https://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._~%/-]+)?/voice\"[ \t]*(//.*)?$")
    message(FATAL_ERROR "Set STRANDS_CLOUD_URL in main/cloud_settings.h to the full HTTPS endpoint ending in /voice.")
endif()

string(TIMESTAMP STRANDS_BUILD_EPOCH "%s" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE STRANDS_BUILD_EPOCH=${STRANDS_BUILD_EPOCH})
