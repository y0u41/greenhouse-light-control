cmake_minimum_required(VERSION 3.16)

project(GreenhouseLightControl VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt 安装路径：默认 E:/Qt/6.12.0/mingw_64，可用 -DCMAKE_PREFIX_PATH 覆盖
if(NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "E:/Qt/6.12.0/mingw_64" CACHE PATH "Qt 安装路径")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Charts)

# TTS 依赖 Qt TextToSpeech 模块。本机常见未安装（需 MaintenanceTool 补装），
# 因此改为"探测后自动决定默认值"：装了默认开、没装默认关，保证开箱即可构建。
find_package(Qt6 QUIET COMPONENTS TextToSpeech)
if(Qt6TextToSpeech_FOUND)
    set(GREENHOUSE_TTS_DEFAULT ON)
    message(STATUS "TTS: 检测到 Qt TextToSpeech，默认启用")
else()
    set(GREENHOUSE_TTS_DEFAULT OFF)
    message(STATUS "TTS: 未检测到 Qt TextToSpeech，默认关闭（补装后重新 configure 即自动开启；"
                   "或 -DGREENHOUSE_ENABLE_TTS=ON 强制开启）")
endif()
option(GREENHOUSE_ENABLE_TTS "编译 TTS 播报（需 Qt TextToSpeech）" ${GREENHOUSE_TTS_DEFAULT})
# ASR 走 Windows PowerShell + System.Speech，无需额外 Qt 模块
option(GREENHOUSE_ENABLE_ASR "编译语音识别（Windows）" ON)
# 调试期保留控制台输出
option(GREENHOUSE_CONSOLE "构建为控制台程序（便于看 qDebug）" OFF)

set(PROJECT_SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/config.cpp
    src/nodemodel.cpp
    src/nodeserver.cpp
    src/commandengine.cpp
    src/command_ack_guard.cpp
    src/llmclient.cpp
    src/chartwidget.cpp
    src/controlpanel.cpp
    src/ttsservice.cpp
    src/asrservice.cpp
)

set(PROJECT_HEADERS
    src/mainwindow.h
    src/config.h
    src/nodemodel.h
    src/nodeserver.h
    src/commandengine.h
    src/command_ack_guard.h
    src/llmclient.h
    src/chartwidget.h
    src/controlpanel.h
    src/ttsservice.h
    src/asrservice.h
    src/protocol_actions.h
)

qt_add_executable(${PROJECT_NAME}
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

if(NOT GREENHOUSE_CONSOLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE ON)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Charts
)

if(GREENHOUSE_ENABLE_TTS)
    if(NOT Qt6TextToSpeech_FOUND)
        find_package(Qt6 REQUIRED COMPONENTS TextToSpeech)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::TextToSpeech)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GREENHOUSE_ENABLE_TTS=1)
    message(STATUS "TTS: 启用")
else()
    message(STATUS "TTS: 关闭（GREENHOUSE_ENABLE_TTS=OFF）—— 播报按钮将置灰，其余功能不受影响")
endif()

if(GREENHOUSE_ENABLE_ASR)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GREENHOUSE_ENABLE_ASR=1)
    message(STATUS "ASR: 启用")
else()
    message(STATUS "ASR: 关闭（GREENHOUSE_ENABLE_ASR=OFF）")
endif()
