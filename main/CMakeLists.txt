# main component: the ESP-IDF application. Pulls in the portable mesh core
# from src/mesh (registered as ESP-IDF component "mesh" via
# EXTRA_COMPONENT_DIRS in the top-level CMakeLists.txt) as a plain
# dependency -- that core has zero ESP-IDF deps itself, but here it needs
# to be linked into the firmware image.

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    # Note: esp_now.h is provided by the esp_wifi component in ESP-IDF
    # v5.2+ (there is no separate "esp_now" component to require -- it was
    # merged into esp_wifi). Requiring a nonexistent "esp_now" component
    # fails CMake's component resolution with "Failed to resolve component
    # 'esp_now'."
    REQUIRES mesh esp_wifi nvs_flash esp_event
)
