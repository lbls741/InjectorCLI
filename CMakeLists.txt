cmake_minimum_required(VERSION 3.24)

project(hybrid-inject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HYBRID_INJECT_SOURCES
    src/Main.cpp
    src/Injector.cpp
    src/CliOptions.cpp
    src/IniCompat.cpp
    src/WineCompat.cpp
)

add_executable(hybrid-inject ${HYBRID_INJECT_SOURCES})

add_executable(testpayload testpayload/testpayload.cpp)

set_target_properties(hybrid-inject PROPERTIES
    OUTPUT_NAME "hybrid-inject"
)

set_target_properties(testpayload PROPERTIES
    OUTPUT_NAME "testpayload"
)

# wmain 入口：MSVC 自动识别，MinGW 需要 -municode。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    target_compile_options(hybrid-inject PRIVATE
        /W3
        /permissive-
        /sdl
        /utf-8
    )
elseif(MINGW)
    target_compile_options(hybrid-inject PRIVATE
        -municode
        -Wall
        -static-libgcc
        -static-libstdc++
    )
    target_link_options(hybrid-inject PRIVATE
        -municode
        -static
    )
    target_compile_options(testpayload PRIVATE -Wall)
endif()

target_compile_definitions(hybrid-inject PRIVATE
    UNICODE
    _UNICODE
    _CONSOLE
)

target_compile_definitions(testpayload PRIVATE
    UNICODE
    _UNICODE
)

include(GNUInstallDirs)

install(TARGETS hybrid-inject testpayload
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 侧车 manifest：Windows 在 exe 同目录发现 <exe>.manifest 即生效；
# Wine/Proton 同样读取外部 manifest（requireAdministrator 被忽略）。
install(FILES app.manifest
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME "hybrid-inject.exe.manifest"
)
