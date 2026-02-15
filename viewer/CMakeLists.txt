cmake_minimum_required(VERSION 3.20)
project(notes_viewer C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LIBTEXCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../external/libtexce" CACHE PATH "Path to libtexce root")
set(CEDEV_TOOLCHAIN "${LIBTEXCE_ROOT}/cmake/CEdevToolchain.cmake")

if(NOT DEFINED CEDEV_ROOT AND DEFINED ENV{CEDEV})
  set(CEDEV_ROOT "$ENV{CEDEV}" CACHE PATH "Path to CEdev toolchain root")
endif()

if(NOT EXISTS "${CEDEV_TOOLCHAIN}")
  message(FATAL_ERROR "Missing external/libtexce submodule. Run: git submodule update --init --recursive")
endif()

include(${CEDEV_TOOLCHAIN})

set(TEX_CORE_SOURCES
  ${LIBTEXCE_ROOT}/src/tex/tex_util.c
  ${LIBTEXCE_ROOT}/src/tex/tex_pool.c
  ${LIBTEXCE_ROOT}/src/tex/tex_symbols.c
  ${LIBTEXCE_ROOT}/src/tex/tex_metrics.c
  ${LIBTEXCE_ROOT}/src/tex/tex_fonts.c
  ${LIBTEXCE_ROOT}/src/tex/tex_token.c
  ${LIBTEXCE_ROOT}/src/tex/tex_parse.c
  ${LIBTEXCE_ROOT}/src/tex/tex_measure.c
  ${LIBTEXCE_ROOT}/src/tex/tex_layout.c
  ${LIBTEXCE_ROOT}/src/tex/tex_renderer.c
  ${LIBTEXCE_ROOT}/src/tex/tex_draw.c
)

cedev_add_program(
  TARGET notes_viewer
  NAME "NOTES"
  DESCRIPTION "libtexce notes viewer"
  SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ntx_pack.c
    ${TEX_CORE_SOURCES}
  INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${LIBTEXCE_ROOT}/src
    ${LIBTEXCE_ROOT}/src/tex
    ${LIBTEXCE_ROOT}/include
  LIBLOAD
    graphx keypadc fileioc fontlibc
  COMPILE_OPTIONS
    -DTEX_USE_FONTLIB
    -DTEX_DIRECT_RENDER
)
