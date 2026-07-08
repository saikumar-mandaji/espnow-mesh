# main component: the ESP-IDF application. Pulls in the portable mesh core
# from src/mesh (registered as ESP-IDF component "mesh" via
# EXTRA_COMPONENT_DIRS in the top-level CMakeLists.txt) as a plain
# dependency -- that core has zero ESP-IDF deps itself, but here it needs
# to be linked into the firmware image.

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES mesh esp_wifi esp_now nvs_flash esp_event
)
