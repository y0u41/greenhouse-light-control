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

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Charts Test)

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
# 单元测试基座（Qt Test）。默认开；只影响 build 目录里的测试目标，不进主程序。
option(GREENHOUSE_BUILD_TESTS "构建 Qt Test 单元测试" ON)

set(PROJECT_SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/config.cpp
    src/nodemodel.cpp
    src/nodeserver.cpp
    src/commandengine.cpp
    src/command_ack_guard.cpp
    src/csvlogger.cpp
    src/llmclient.cpp
    src/llmcontroller.cpp
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
    src/csvlogger.h
    src/llmclient.h
    src/llmcontroller.h
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

# =====================================================================
# 单元测试基座（Qt Test）
# ---------------------------------------------------------------------
# 独立可执行文件，测试源**不混进主程序 target**：主 exe 的源文件列表保持原样，
# 下面只挑选被测实现所需的翻译单元，避免为了跑纯函数校验而拖进 GUI/图表/语音。
#
# 覆盖 ctest：只需源目录里的 tests/ 与 tools/validate_cases.json，
# 因此 WORKING_DIRECTORY 固定为源目录，测试内定位工程根不依赖调用者 cwd。
# 一致性脚本用的 Python 解释器可通过环境变量 QTPY 覆盖。
# =====================================================================
if(GREENHOUSE_BUILD_TESTS)
    enable_testing()

    set(TEST_TARGET test_commandengine)

    # commandengine.cpp 的 submit() 引用 NodeModel::peek / NodeServer::sendCommand，
    # 因此这些实现必须一起编进来（是链接真实实现，不是把校验逻辑抄一份到测试里）。
    set(TEST_ENGINE_SOURCES
        tests/test_commandengine.cpp
        src/commandengine.cpp
        src/nodemodel.cpp
        src/nodeserver.cpp
        src/command_ack_guard.cpp
        src/csvlogger.cpp
        src/config.cpp
        src/protocol_actions.h
        src/commandengine.h
        src/nodemodel.h
        src/nodeserver.h
        src/command_ack_guard.h
        src/csvlogger.h
        src/config.h
    )

    qt_add_executable(${TEST_TARGET} ${TEST_ENGINE_SOURCES})

    target_include_directories(${TEST_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(${TEST_TARGET} PRIVATE
        QT_TEST_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_link_libraries(${TEST_TARGET} PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Network
    )

    add_test(NAME commandengine
             COMMAND ${TEST_TARGET}
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # 跨语言一致性守卫的显式入口：Python 侧单独跑一遍（给人看的 diff）。
    # 退出码：判定不一致 -> 1（ctest 变红）；仅 why 文案差异 -> 0（informational）。
    # 解释器默认用本机 DSH 运行时自带的 Python；可用 -DGREENHOUSE_PYTHON=... 覆盖。
    set(GREENHOUSE_PYTHON
        "C:/Users/t3205/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/python.exe"
        CACHE FILEPATH "一致性校验脚本使用的 Python 解释器")

    if(EXISTS "${GREENHOUSE_PYTHON}")
        add_test(NAME protocol_parity_python
                 COMMAND "${GREENHOUSE_PYTHON}"
                         ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_validate_parity.py
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        message(STATUS "测试: protocol_parity_python 使用 ${GREENHOUSE_PYTHON}")
    else()
        message(STATUS "测试: 未找到 Python（${GREENHOUSE_PYTHON}），protocol_parity_python 未注册；"
                       "Qt 侧 parity 用例仍会自行调用 qtapp/tests 里的脚本入口")
    endif()

    message(STATUS "测试: 已启用 target=${TEST_TARGET}（ctest 名称 commandengine）")
else()
    message(STATUS "测试: 已关闭（GREENHOUSE_BUILD_TESTS=OFF）")
endif()
