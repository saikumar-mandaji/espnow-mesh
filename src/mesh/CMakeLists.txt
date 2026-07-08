# ESP-IDF component wrapper around the portable mesh core.
#
# The core itself (mesh.c / mesh.h) is plain C99 with zero ESP-IDF
# dependencies -- this file exists only so the ESP-IDF build system can
# compile and link it into the firmware image as component
# "espnow_mesh_core". The exact same mesh.c is also compiled standalone by
# tests/host/Makefile with a plain host `cc`, completely bypassing this
# file and the rest of the ESP-IDF build.

idf_component_register(
    SRCS "mesh.c"
    INCLUDE_DIRS "../../include"
)
