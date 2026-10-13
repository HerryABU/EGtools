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

# Pdf = Qt PDF（内置 PDFium，与 Chrome/Acrobat 同引擎）。
# 用途：预览按**目标分辨率矢量重绘**（任意缩放都清晰），取代"渲染固定倍率位图再放大"。
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Concurrent Pdf)

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
    src/pdfrender.h
    src/pdfrender.cpp
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

target_link_libraries(EGtoolsFrontend PRIVATE Qt6::Widgets Qt6::OpenGLWidgets Qt6::Concurrent Qt6::Pdf)

# ---------------------------------------------------------------------------
# PDFium 渲染探针（控制台；无 GUI，沙箱可跑）
#   pdfium_probe <pdf> [输出目录]
# 用数字确认三件事：scaledClipRect 语义（局部渲染==整页裁剪）、pagePointSize、
# 以及"矢量重绘 vs 1x 位图放大"的清晰度倍数 —— 改 GUI 之前先把事实钉死。
# ---------------------------------------------------------------------------
add_executable(pdfium_probe tools/pdfium_probe.cpp)
target_link_libraries(pdfium_probe PRIVATE Qt6::Pdf Qt6::Gui)
if(MSVC)
    target_compile_options(pdfium_probe PRIVATE /utf-8)
endif()

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

# ===========================================================================
# Linux 打包（deb / rpm）—— 由仓库根的 build_linux.sh 驱动：
#   ① 先 `dotnet publish -r linux-x64 --self-contained` 把三个内核出到 DIST/<name>/
#   ② 再配置并构建本工程 → ③ `cpack -G DEB` / `-G RPM`
#
# 布局（装到 /opt/egtools）必须让 `KernelClient` 的自动发现命中：内核放在
# **前端同级的 <name>/<name>**（与 Windows 的 DIST 布局一致，见 mainwindow 的 cliExe 探测）。
#
# ⚠️⚠️ 依赖**不要**交给 dpkg-shlibdeps / rpm autoreq 自动推（2026-10-13 实测踩坑）：
#    本包是 **self-contained 捆绑**（三个 .NET 内核自带整套运行时与私有 .so，
#    还有 libOpenCvSharpExtern.so / libMuPDFWrapper.so）。自动推会把**捆绑内的私有库**
#    也当成"外部依赖"去找，于是 `dpkg-shlibdeps: error: cannot find library
#    liblttng-ust.so.0` ⇒ **CPackDeb 直接报错、包都出不来**。
#    正确模型：捆绑的东西不算依赖，只有**必须由发行版提供**的（Qt6 运行库）才算。
#    所以由 build_linux.sh 从**直接 DT_NEEDED**（readelf -d，不是 ldd 的传递闭包）
#    反推，并在构建机上用 `dpkg -S` 解出**本发行版的真实包名**后通过 -D 传进来
#    （如 libqt6widgets6t64 —— t64 后缀随发行版变，手写必错）。
#    RPM 侧直接声明 soname 能力（libQt6Widgets.so.6()(64bit)）—— 这正是 rpm 自报的形式，
#    天然跨发行版，不必知道对方把 Qt 装在哪个包名里。
# ===========================================================================
if(UNIX AND NOT APPLE)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/egtools")
    set(CPACK_PACKAGE_NAME "egtools")
    set(CPACK_PACKAGE_VENDOR "HerryABU")
    set(CPACK_PACKAGE_CONTACT "HerryABU")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "工程图纸材料表提取与对比工具集（PDF 材料表 / 图纸差异 / 等轴测图比对）")
    set(CPACK_PACKAGE_VERSION "4.3.0")
    set(CPACK_GENERATOR "DEB;RPM")

    # rpath 用 $ORIGIN：让二进制优先从**自己所在目录**解析 Qt 库（随包分发时必需）。
    set_target_properties(EGtoolsFrontend PROPERTIES INSTALL_RPATH "$ORIGIN")

    install(TARGETS EGtoolsFrontend RUNTIME DESTINATION .)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/EGtoolsFrontend.runtimeconfig.json" DESTINATION .)

    # 三个内核（self-contained 目录，含各自的 .so 与 .runtimeconfig.json）。
    # ⚠️ 源目录由 build_linux.sh 通过 -DEGTOOLS_LINUX_DIST=... 传入（默认 DIST-linux/）：
    #    与 Windows 的 DIST/ **刻意分开**，否则跨平台产物同名互覆。
    if(NOT DEFINED EGTOOLS_LINUX_DIST)
        set(EGTOOLS_LINUX_DIST "${CMAKE_CURRENT_SOURCE_DIR}/../DIST-linux")
    endif()
    foreach(_k EGpdf2excel EGexcel2df EGpdf2df)
        if(EXISTS "${EGTOOLS_LINUX_DIST}/${_k}")
            install(DIRECTORY "${EGTOOLS_LINUX_DIST}/${_k}" DESTINATION .)
        else()
            message(WARNING
                "${EGTOOLS_LINUX_DIST}/${_k} 不存在 —— 该内核不会进包。请先运行 build_linux.sh（它会 publish 内核）。")
        endif()
    endforeach()

    # ⚠️⚠️ 内核的可执行文件（apphost）**必须有 exec 位**，这里显式 chmod，不能指望源目录：
    #    `install(DIRECTORY)` **默认不保留源权限**，而内核目录是 Windows 侧交叉发布的产物
    #    （NTFS 没有 exec 位）⇒ 实测装完后 `/opt/egtools/EGpdf2excel/EGpdf2excel` 是 0644，
    #    直接执行报 126（Permission denied），前端也会因此走不成命令行通道。
    #    用 install(CODE)（括号实参，免转义）而不是 install(PROGRAMS)：后者要逐个列文件且会丢目录结构。
    install(CODE [=[
        # ⚠️ 必须带上 $ENV{DESTDIR}：CPack 是"把 DESTDIR 指向暂存目录 + 前缀仍是 /opt/egtools"
        #    这样安装的，直接用 CMAKE_INSTALL_PREFIX 会去 chmod 真实的 /opt/egtools
        #    ⇒ 非 root 时 file() 直接报 "Failed to set permissions"（实测）。
        set(_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
        set(_kernels EGpdf2excel EGexcel2df EGpdf2df)
        foreach(_k IN LISTS _kernels)
            set(_bin "${_root}/${_k}/${_k}")
            if(EXISTS "${_bin}")
                file(CHMOD "${_bin}"
                     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                 GROUP_READ GROUP_EXECUTE
                                 WORLD_READ WORLD_EXECUTE)
            endif()
        endforeach()
    ]=])
    # 桌面集成 + 图标（启动器符号链接 /usr/bin/egtools 由 postinst/post 创建）。
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/egtools.desktop"
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/res/app_icon.png"
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
            RENAME egtools.png)

    # 依赖：**关掉**自动推导（理由见本节顶部的长注释），只认外部传入的实测结果。
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
    # 兜底默认值（仅在直接跑 cpack、没经 build_linux.sh 时生效）：
    # 这是 Ubuntu 24.04 上的实测结果，其它发行版请用 build_linux.sh 生成。
    if(NOT DEFINED CPACK_DEBIAN_PACKAGE_DEPENDS)
        set(CPACK_DEBIAN_PACKAGE_DEPENDS
            "libc6,libgcc-s1,libstdc++6,libqt6core6t64,libqt6gui6t64,libqt6widgets6t64,libqt6openglwidgets6t64,libqt6pdf6")
        message(WARNING
            "未提供实测的 CPACK_DEBIAN_PACKAGE_DEPENDS，使用 Ubuntu 24.04 的兜底值。"
            "换发行版请改走 build_linux.sh（它会按本机实测反推包名）。")
    endif()
    if(NOT DEFINED CPACK_RPM_PACKAGE_REQUIRES)
        set(CPACK_RPM_PACKAGE_REQUIRES
            "libQt6Core.so.6()(64bit),libQt6Gui.so.6()(64bit),libQt6Widgets.so.6()(64bit),libQt6OpenGLWidgets.so.6()(64bit),libQt6Pdf.so.6()(64bit)")
    endif()
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm")
    set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/post")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postun")
    set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE")
    include(CPack)
endif()
