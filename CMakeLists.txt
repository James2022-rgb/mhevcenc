cmake_minimum_required(VERSION 3.22)

# --------------------------------------------------------------------------------
# Internal variables
#

set(TARGET_NAME "mhevcenc")
set(SRC_DIR "src")

# --------------------------------------------------------------------------------
# Project
#

project(${TARGET_NAME}
  VERSION 0.1.0.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=thread-safety")
endif()

# mhevcenc wraps Vulkan Video HEVC encode using mnexus. Unlike
# mhevcdec, the encode side does NOT depend on vidsynt -- the driver
# generates VPS/SPS/PPS for us via vkCreateVideoSessionParametersKHR
# rather than us parsing pre-existing NALs. We only need mnexus to be
# present (with MNEXUS_ENABLE_VIDEO_CODING=ON so the encode API
# entries route to real impls).
if(NOT TARGET mnexus)
  message(FATAL_ERROR "mhevcenc: mnexus target not defined. Add mnexus before mhevcenc.")
endif()

# --------------------------------------------------------------------------------
# project target
#

add_library(${TARGET_NAME} STATIC)
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

set(SOURCES_PRIVATE_ROOT_DIR "${SRC_DIR}/${TARGET_NAME}/private")
set(SOURCES_PUBLIC_ROOT_DIR  "${SRC_DIR}/${TARGET_NAME}/public")

set(SOURCES_MHEVCENC_PUBLIC
  ${SOURCES_PUBLIC_ROOT_DIR}/hevc_encode_session.h
)
source_group("Public" FILES ${SOURCES_MHEVCENC_PUBLIC})

set(SOURCES_MHEVCENC_PRIVATE
  ${SOURCES_PRIVATE_ROOT_DIR}/hevc_encode_session.cpp
)
source_group("Private" FILES ${SOURCES_MHEVCENC_PRIVATE})

target_sources(${TARGET_NAME} PRIVATE
  ${SOURCES_MHEVCENC_PUBLIC}
  ${SOURCES_MHEVCENC_PRIVATE}
)

target_include_directories(${TARGET_NAME} PUBLIC
  ${SRC_DIR}
)

target_include_directories(${TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SOURCES_PRIVATE_ROOT_DIR}
)

target_link_libraries(${TARGET_NAME} PUBLIC
  mbase
  mnexus
)
