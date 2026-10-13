# The Valence client library is header-only (MIT): an include path is the
# whole of consuming it. The sibling Valence checkout, at the sha in valence.pin.
if(DEFINED ENV{VALENCE_ROOT})
    set(VALENCE_ROOT "$ENV{VALENCE_ROOT}")
endif()
if(NOT DEFINED VALENCE_ROOT)
    set(VALENCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../Valence")
endif()
if(NOT DEFINED GARNET_BOARD)
    set(GARNET_BOARD waveshare_s3_43)
endif()

idf_component_register(SRCS "main.cpp"
                            "bsp/${GARNET_BOARD}.cpp"
                            "link/ws_link.cpp" "link/tether_link.cpp" "link/udp.cpp"
                            "app/session.cpp" "app/catalog_model.cpp"
                            "app/mapping.cpp" "app/console.cpp"
                            "ui/ui.cpp"
                            "net/wifi.cpp"
                       INCLUDE_DIRS "." "${VALENCE_ROOT}/lib/valence/include"
                       EMBED_TXTFILES "../boards/${GARNET_BOARD}/manifest.json"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag
                                     esp_lcd esp_timer esp_event esp_netif esp_wifi
                                     esp_http_server lwip nvs_flash json mbedtls vfs)

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b> -Wno-missing-field-initializers)
# The Client's catalog reassembly budget: 256 chunks = 48 KB (the Nucleus
# 0.1.44 catalog measured 28,611 B). Model::kMaxBytes must hold the same.
target_compile_definitions(${COMPONENT_LIB} PRIVATE GARNET_BOARD_${GARNET_BOARD}=1 VALENCE_CLIENT_CATALOG_CHUNKS=256)
