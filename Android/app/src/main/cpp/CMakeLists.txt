# Sapple for Android: the same C engines and bridges the iOS / macOS targets compile (see project.yml), built into one
# JNI library. The engines' compiler flags are copied from project.yml / the Makefile and matter: several engines are
# only bit-exact with floating-point contraction off.
cmake_minimum_required(VERSION 3.22)
project(sapple C ASM)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

file(GLOB SAM_SRC     "${ROOT}/Engine/sam/*.c")
file(GLOB SAPI4_SRC   "${ROOT}/Engine/sapi4/src/*.c")
list(APPEND SAPI4_SRC "${ROOT}/Engine/sapi4/lib/sapi4tts.c" "${ROOT}/Engine/sapi4/lib/stubs.c"
                      "${ROOT}/Engine/sapi4/port/dllimage.c")
file(GLOB ANNA_SRC    "${ROOT}/Engine/anna/*.c")
file(GLOB ONECORE_SRC "${ROOT}/Engine/onecore/*.c")
set(BRIDGE_SRC
    "${ROOT}/Shared/Bridge/cv_bridge.c"
    "${ROOT}/Shared/Bridge/cv_resample.c"
    "${ROOT}/Shared/Bridge/cv4n_bridge.c"
    "${ROOT}/Shared/Bridge/cva_bridge.c"
    "${ROOT}/Shared/Bridge/cvo_bridge.c"
    "${ROOT}/Shared/Bridge/cvn_bridge.c")

set_source_files_properties(${SAM_SRC} PROPERTIES COMPILE_OPTIONS "-std=c99;-w")
set_source_files_properties(${BRIDGE_SRC} PROPERTIES COMPILE_OPTIONS "-std=c99")
set_source_files_properties(${SAPI4_SRC} PROPERTIES COMPILE_OPTIONS
    "-std=c11;-O2;-ffp-contract=off;-fno-builtin;-w")
set_source_files_properties(${ANNA_SRC} PROPERTIES COMPILE_OPTIONS "-std=c99;-ffp-contract=off;-w")
set_source_files_properties(${ONECORE_SRC} PROPERTIES COMPILE_OPTIONS "-std=c99;-ffp-contract=off;-fno-fast-math;-w")

set(ALL_SRC ${SAM_SRC} ${SAPI4_SRC} ${ANNA_SRC} ${ONECORE_SRC} ${BRIDGE_SRC} sapple_jni.c)

# TruVoice: its engine tables are Centigram's data and are compiled in, so they are only built when present
# (tools/truvoice_data_android.sh writes TruVoiceData/tvdata-elf.s from the pinned OpenTV upstream).
set(TVDATA "${ROOT}/TruVoiceData/tvdata-elf.s")
if(EXISTS "${TVDATA}")
    file(GLOB OPENTV_SRC "${ROOT}/Engine/opentv/src/engine/*.c")
    list(APPEND OPENTV_SRC "${ROOT}/Engine/opentv/src/port/tvtts.c" "${ROOT}/Engine/opentv/src/port/msvcrt.c"
                           "${ROOT}/Engine/opentv/src/port/stubs.c")
    set_source_files_properties(${OPENTV_SRC} PROPERTIES COMPILE_OPTIONS
        "-std=gnu11;-O2;-fwrapv;-fno-strict-aliasing;-w;-I${ROOT}/Engine/opentv/src;-I${ROOT}/Engine/opentv/generated")
    set_source_files_properties("${ROOT}/Shared/Bridge/cvt_bridge.c" PROPERTIES COMPILE_OPTIONS "-std=c99")
    list(APPEND ALL_SRC ${OPENTV_SRC} "${ROOT}/Shared/Bridge/cvt_bridge.c" "${TVDATA}")
    set(HAVE_TRUVOICE 1)
else()
    message(STATUS "Sapple: no ${TVDATA}, building without the TruVoice voices")
    set(HAVE_TRUVOICE 0)
endif()

add_library(sapple SHARED ${ALL_SRC})
target_compile_definitions(sapple PRIVATE CV_NO_DEBUG_ENV=1 SAPPLE_HAVE_TRUVOICE=${HAVE_TRUVOICE})
target_include_directories(sapple PRIVATE
    "${ROOT}/Engine/sam" "${ROOT}/Shared/Bridge" "${ROOT}/Engine/sapi4/lib" "${ROOT}/Engine/sapi4/src"
    "${ROOT}/Engine/sapi4/port" "${ROOT}/Engine/anna" "${ROOT}/Engine/onecore" "${ROOT}/Engine/opentv/include")
# dl: the neural bridge loads Microsoft's Speech SDK at run time; nothing of the SDK is linked here.
target_link_libraries(sapple PRIVATE m log dl)
# 16 KB pages (Android 15+ devices)
target_link_options(sapple PRIVATE "-Wl,-z,max-page-size=16384")
