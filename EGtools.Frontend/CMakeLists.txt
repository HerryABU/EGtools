cmake_minimum_required(VERSION 3.21)
project(EGtoolsFrontend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ---------------------------------------------------------------------------
# 构建配置：交付物必须是 Release。
#
# Ninja 是单配置生成器，不指定 CMAKE_BUILD_TYPE 时**既不是 Debug 也不是 Release**
# （/O0 无优化、无 NDEBUG，等于一个"三不像"），这是最容易被忽略的一种"发了调试版"
# 的形态。所以：未指定时默认 Release；显式要求 Debug 时也拒绝并改为 Release
# （本工程以发布为目的；真需要调试请用 RelWithDebInfo 并自行加断点信息）。
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE 未指定 → 默认 Release")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "检测到 CMAKE_BUILD_TYPE=Debug —— 已强制改为 Release（本工程只发布 Release）。")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Concurrent)

# GUI subsystem on purpose (WIN32_EXECUTABLE): a console-subsystem binary makes
# Windows create a console window on launch, which for a windowed app means the
# user sees a black window **flash** past at startup. The self-test modes still
# print to stdout — main.cpp attaches to the parent console, but ONLY when the
# standard handles were not already provided (i.e. no redirection), so
# `--selftest > out.txt` and `... | Out-String` keep working unchanged.
add_executable(EGtoolsFrontend
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/pdfview.h
    src/pdfview.cpp
    src/pdfcanvas.h
    src/pdfcanvas.cpp
    src/pagerender.h
    src/pagerender.cpp
    src/iregioncanvas.h
    src/continuouspdfview.h
    src/continuouspdfview.cpp
    src/clirunner.h
    src/clirunner.cpp
    src/kernelclient.h
    src/kernelclient.cpp
    src/selftest.h
    src/selftest.cpp
    src/manualdialog.h
    src/manualdialog.cpp
    src/homepage.h
    src/homepage.cpp
    src/schemes.h
    src/schemes.cpp
    src/schemepage.h
    src/schemepage.cpp
    src/diffpage.h
    src/diffpage.cpp
    src/diffsettings.h
    src/diffsettings.cpp
    src/excelcomparepage.h
    src/excelcomparepage.cpp
    src/materialextractpage.h
    src/materialextractpage.cpp
    src/pagenavpanel.h
    src/pagenavpanel.cpp
    src/theme.h
    src/theme.cpp
    src/charsegmentpanel.h
    src/charsegmentpanel.cpp
    src/panels.h
    src/types.h
    res/app_icon.rc
)

set_target_properties(EGtoolsFrontend PROPERTIES WIN32_EXECUTABLE TRUE)

# 构建配置进二进制：Release 断言 + 一个常量，便于日志/诊断自证版本。
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(EGtoolsFrontend PRIVATE EGTOOLS_BUILD_CONFIG="Release")
else()
    target_compile_definitions(EGtoolsFrontend PRIVATE EGTOOLS_BUILD_CONFIG="${CMAKE_BUILD_TYPE}")
endif()

# Embed the in-app manual + the window/taskbar icon as Qt resources so they
# travel inside the binary.
# NOTE: must come AFTER add_executable (the target must already exist).
qt_add_resources(EGtoolsFrontend "appres"
    PREFIX "/"
    BASE "res"
    FILES res/manual.html
          res/app_icon.png
)

target_link_libraries(EGtoolsFrontend PRIVATE Qt6::Widgets Qt6::OpenGLWidgets Qt6::Concurrent)

# ---------------------------------------------------------------------------
# 宿主自己的 .NET 运行时配置（内嵌调用用）。
#
# 为什么必须给**宿主**（Qt exe）配一份，而不是直接拿内核的：
#   hostfxr 初始化时要吃一份 runtimeconfig.json。若拿内核那份（net9.0-windows），
#   等于把内核当成"宿主应用"，实测随后按路径加载内核程序集会失败
#   （COR_E_TYPELOAD 0x80131522）；而用宿主自己的配置时，内核 DLL 是**组件**身份
#   被加载（依赖由它自己的 deps.json 解析）—— 这才是 hostfxr 组件模型的正常用法。
#   rollForward=LatestMajor：宿主声明 net9.0，机器上只装了更高主版本时也能起
#   （与命令行侧 `dotnet --roll-forward LatestMajor` 等价，不需要额外环境变量）。
# ---------------------------------------------------------------------------
set(EGTOOLS_HOST_RUNTIMECONFIG
"{
  \"runtimeOptions\": {
    \"tfm\": \"net9.0\",
    \"framework\": { \"name\": \"Microsoft.NETCore.App\", \"version\": \"9.0.0\" },
    \"rollForward\": \"LatestMajor\"
  }
}
")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/EGtoolsFrontend.runtimeconfig.json" "${EGTOOLS_HOST_RUNTIMECONFIG}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kernelclient_test.runtimeconfig.json" "${EGTOOLS_HOST_RUNTIMECONFIG}")

# Embed the EXE icon (Explorer/taskbar/Start menu) on Windows.
if(WIN32)
    enable_language(RC)
endif()

if(MSVC)
    target_compile_options(EGtoolsFrontend PRIVATE /utf-8)
endif()

if(WIN32)
    target_compile_definitions(EGtoolsFrontend PRIVATE EGTOOLS_WIN32)
endif()

# ---------------------------------------------------------------------------
# 内核调用融合层的控制台测试（不需要 GUI，因此可在无头环境跑）
#   kernelclient_test <cliDir> <request.json>
# 对同一个请求分别跑【内嵌】(进程内调库)与【命令行】(QProcess)两条路，比对结果是否一致
# —— "两条路融合"这件事在 GUI 里没法自动化，所以单独出一个 exe 做证据。
# ---------------------------------------------------------------------------
add_executable(kernelclient_test
    tools/kernelclient_test.cpp
    src/kernelclient.h
    src/kernelclient.cpp
    src/clirunner.h
    src/clirunner.cpp
)
target_link_libraries(kernelclient_test PRIVATE Qt6::Core)
target_include_directories(kernelclient_test PRIVATE src)
if(MSVC)
    target_compile_options(kernelclient_test PRIVATE /utf-8)
endif()
