file(GLOB SOURCES "*.c" "*.cpp")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "." "include"
    PRIV_REQUIRES
        nvs_flash
        esp_driver_gpio
        esp_timer
        esp_adc
        esp_matter
        espressif__button
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "-DCHIP_HAVE_CONFIG_H"
    "-std=gnu++17"
)
