# MCP-сервер для Claude — модуль BreezeCAD: библиотека bz-mcp (сервер на
# 127.0.0.1) и мост bz-mcp-bridge (stdio ↔ HTTP для Claude Desktop),
# упакованные в .bzmod.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
#
# Пакет: build/packages/bz-mcp-<version>-<system>-<arch>.bzmod.
# id, version и имя библиотеки — из module.json.
cmake_minimum_required(VERSION 3.21)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/module.json BZ_MANIFEST)
string(JSON BZ_ID      GET "${BZ_MANIFEST}" id)
string(JSON BZ_VERSION GET "${BZ_MANIFEST}" version)
string(JSON BZ_LIBRARY GET "${BZ_MANIFEST}" library)

project(bz_module_${BZ_ID} VERSION ${BZ_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
find_package(Threads REQUIRED)

add_library(bz_json INTERFACE)
target_include_directories(bz_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)

if(MSVC)   # те же предупреждения, что у BreezeCAD (cmake/CompilerWarnings.cmake)
    set(BZ_WARNINGS /W4 /permissive- /utf-8)
else()
    set(BZ_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
                    -Woverloaded-virtual -Wconversion -Wsign-conversion -Wnull-dereference -Wno-unused-parameter)
endif()

set(BZ_STAGE ${PROJECT_BINARY_DIR}/stage/${BZ_ID})
function(bz_output target)
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${BZ_STAGE}
        RUNTIME_OUTPUT_DIRECTORY ${BZ_STAGE}
        ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)   # import-библиотека Windows — не в пакет
    foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${cfg} CFG)
        set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${CFG} ${BZ_STAGE} RUNTIME_OUTPUT_DIRECTORY_${CFG} ${BZ_STAGE})
    endforeach()
endfunction()

# --- Модуль: сервер -----------------------------------------------------------
add_library(bz_mcp_module SHARED src/McpModule.cpp src/Net.cpp src/Connection.cpp src/Protocol.cpp)
target_include_directories(bz_mcp_module PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
target_link_libraries(bz_mcp_module PRIVATE bz_json Threads::Threads)
target_compile_options(bz_mcp_module PRIVATE ${BZ_WARNINGS})
set_target_properties(bz_mcp_module PROPERTIES OUTPUT_NAME ${BZ_LIBRARY} CXX_VISIBILITY_PRESET hidden)
bz_output(bz_mcp_module)

# --- Мост для Claude Desktop ----------------------------------------------------
add_executable(bz_mcp_bridge src/Bridge.cpp src/Net.cpp src/Connection.cpp src/Protocol.cpp vendor/platform/Paths.cpp)
target_include_directories(bz_mcp_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
target_link_libraries(bz_mcp_bridge PRIVATE bz_json)
target_compile_options(bz_mcp_bridge PRIVATE ${BZ_WARNINGS})
set_target_properties(bz_mcp_bridge PROPERTIES OUTPUT_NAME bz-mcp-bridge)
bz_output(bz_mcp_bridge)

if(WIN32)
    set_target_properties(bz_mcp_module PROPERTIES PREFIX "")   # bz-mcp.dll — так ищет BreezeCAD
    target_link_libraries(bz_mcp_module PRIVATE ws2_32)
    target_link_libraries(bz_mcp_bridge PRIVATE ws2_32 shell32 ole32)
    if(MINGW)
        # Папка модуля не рядом с программой: среда C++ — внутри.
        target_link_options(bz_mcp_module PRIVATE -static)
        target_link_options(bz_mcp_bridge PRIVATE -static)
        target_sources(bz_mcp_module PRIVATE src/MingwCompat.c)   # см. комментарий в файле
    endif()
endif()

# --- Пакет .bzmod ----------------------------------------------------------------
if(WIN32)
    set(BZ_SYSTEM windows)
elseif(APPLE)
    set(BZ_SYSTEM macos)
else()
    set(BZ_SYSTEM linux)
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" BZ_ARCH)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(BZ_ARCH ${CMAKE_OSX_ARCHITECTURES})
endif()
if(BZ_ARCH MATCHES "^(amd64|x86_64|x64)$")
    set(BZ_ARCH x64)
elseif(BZ_ARCH MATCHES "^(arm64|aarch64)$")
    set(BZ_ARCH arm64)
endif()

set(BZ_PACKAGE ${PROJECT_BINARY_DIR}/packages/bz-${BZ_ID}-${BZ_VERSION}-${BZ_SYSTEM}-${BZ_ARCH}.bzmod)
add_custom_command(OUTPUT ${BZ_PACKAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/packages
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/module.json ${BZ_STAGE}/module.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${BZ_STAGE}/README.md
    COMMAND ${CMAKE_COMMAND} -E rm -f ${BZ_PACKAGE}
    COMMAND ${CMAKE_COMMAND} -E chdir ${PROJECT_BINARY_DIR}/stage ${CMAKE_COMMAND} -E tar cf ${BZ_PACKAGE} --format=zip ${BZ_ID}
    DEPENDS bz_mcp_module bz_mcp_bridge ${CMAKE_CURRENT_SOURCE_DIR}/module.json ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    COMMENT "Пакет модуля: ${BZ_PACKAGE}"
    VERBATIM)
add_custom_target(package_bzmod ALL DEPENDS ${BZ_PACKAGE})

# --- Тесты (без BreezeCAD: программа подменяется) ---------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(bz_mcp_tests tests/main.cpp tests/ProtocolTests.cpp tests/ModuleTests.cpp
                                src/Protocol.cpp src/Net.cpp src/Connection.cpp)
    target_include_directories(bz_mcp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/sdk
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)
    target_link_libraries(bz_mcp_tests PRIVATE bz_json Threads::Threads ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(bz_mcp_tests PRIVATE ws2_32)
    endif()
    add_dependencies(bz_mcp_tests package_bzmod)
    target_compile_definitions(bz_mcp_tests PRIVATE
        BZ_MCP_MODULE="$<TARGET_FILE:bz_mcp_module>"
        BZ_MCP_BRIDGE="$<TARGET_FILE:bz_mcp_bridge>"
        BZ_MCP_PACKAGE="${BZ_PACKAGE}")
    add_test(NAME mcp COMMAND bz_mcp_tests)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME mcp_package COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_package.py
                 ${BZ_PACKAGE} $<TARGET_FILE_NAME:bz_mcp_module> $<TARGET_FILE_NAME:bz_mcp_bridge> ${BZ_SYSTEM})
    endif()
endif()
