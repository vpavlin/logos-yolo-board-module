cmake_minimum_required(VERSION 3.20)
project(logos-yolo-board-module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)
set(CMAKE_AUTOMOC ON)

set(PLUGIN_TARGET yolo_board_module)

qt_add_plugin(${PLUGIN_TARGET} CLASS_NAME YoloBoardModule)

target_sources(${PLUGIN_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/yolo_board_module.cpp
)

set_property(TARGET ${PLUGIN_TARGET} PROPERTY PUBLIC_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/src/i_yolo_board_module.h
)

if(LOGOS_CPP_SDK_ROOT)
  target_include_directories(${PLUGIN_TARGET} PRIVATE
      ${LOGOS_CPP_SDK_ROOT}/include
      ${LOGOS_CPP_SDK_ROOT}/include/cpp
      ${LOGOS_CPP_SDK_ROOT}/include/core
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
else()
  target_include_directories(${PLUGIN_TARGET} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../logos-cpp-sdk
      ${CMAKE_CURRENT_SOURCE_DIR}/../logos-cpp-sdk/cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/../logos-cpp-sdk/core
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
endif()

if(LOGOS_CPP_SDK_ROOT)
    target_link_libraries(${PLUGIN_TARGET} PRIVATE
        Qt6::Core
        Qt6::Concurrent
        ${LOGOS_CPP_SDK_ROOT}/lib/liblogos_sdk.a
    )
else()
    target_link_libraries(${PLUGIN_TARGET} PRIVATE
        Qt6::Core
        Qt6::Concurrent
    )
endif()

target_compile_definitions(${PLUGIN_TARGET} PRIVATE
    YOLO_BOARD_MODULE_METADATA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/metadata.json"
)

if(APPLE)
    set_target_properties(${PLUGIN_TARGET} PROPERTIES
        BUILD_RPATH "@loader_path"
        INSTALL_RPATH "@loader_path"
    )
elseif(UNIX)
    set_target_properties(${PLUGIN_TARGET} PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

install(TARGETS ${PLUGIN_TARGET}
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include
)
