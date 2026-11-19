cmake_minimum_required(VERSION 3.16)

project(wsjt_hackrf_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#
# libhackrf privada.
#
# Debe indicarse explícitamente:
#
#   -DHACKRF_ROOT=/ruta/al/prefix/hackrf
#
set(HACKRF_ROOT "" CACHE PATH "Private HackRF installation prefix")

if(NOT HACKRF_ROOT)
    message(FATAL_ERROR "HACKRF_ROOT must point to the private HackRF installation")
endif()

find_path(
    HACKRF_INCLUDE
    NAMES libhackrf/hackrf.h
    PATHS "${HACKRF_ROOT}/include"
    NO_DEFAULT_PATH
)

find_library(
    HACKRF_LIB
    NAMES hackrf
    PATHS "${HACKRF_ROOT}/lib"
    NO_DEFAULT_PATH
)

if(NOT HACKRF_INCLUDE OR NOT HACKRF_LIB)
    message(FATAL_ERROR
        "Private libhackrf not found under ${HACKRF_ROOT}; "
        "global fallback is intentionally disabled")
endif()

find_package(Threads REQUIRED)

#
# Modulador USB utilizado por la rama TX.
#
add_library(
    usb-tx-modulator STATIC
    src/usb_tx_modulator.cpp
)

target_include_directories(
    usb-tx-modulator
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

#
# Bridge final WSJT-X <-> HackRF.
#
add_executable(
    wsjtx-hackrf-bridge
    src/cat_rx_cycle_rf.cpp
    src/rigctl_server_cycle.cpp
    src/ptt_cycle.cpp
    src/usb_demod.cpp
    src/txsim_worker_cycle.cpp
    src/tx_iq_queue.cpp
)

target_include_directories(
    wsjtx-hackrf-bridge
    PRIVATE
    "${HACKRF_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    wsjtx-hackrf-bridge
    PRIVATE
    "${HACKRF_LIB}"
    Threads::Threads
    usb-tx-modulator
)

#
# Durante el build utilizamos la libhackrf privada absoluta.
# Una vez instalado:
#
#   <prefix>/bridge/bin/wsjtx-hackrf-bridge
#   <prefix>/hackrf/lib/libhackrf.so
#
# por lo que el RUNPATH relativo es:
#
#   ../../hackrf/lib
#
set_target_properties(
    wsjtx-hackrf-bridge
    PROPERTIES
    BUILD_RPATH "${HACKRF_ROOT}/lib"
    INSTALL_RPATH "\$ORIGIN/../../hackrf/lib"
    INSTALL_RPATH_USE_LINK_PATH FALSE
)

install(
    TARGETS wsjtx-hackrf-bridge
    RUNTIME DESTINATION bin
)

message(STATUS "Private HackRF include: ${HACKRF_INCLUDE}")
message(STATUS "Private HackRF library: ${HACKRF_LIB}")
