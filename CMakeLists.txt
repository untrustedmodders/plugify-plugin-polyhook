cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW) # Don't add -W3 warning level by default.
endif()


file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_FILE_CONTENTS)
string(STRIP "${VERSION_FILE_CONTENTS}" VERSION_FILE_CONTENTS)
set(POLYHOOK_VERSION "${VERSION_FILE_CONTENTS}" CACHE STRING "Set version name")
set(POLYHOOK_PACKAGE "polyhook" CACHE STRING "Set package name")
string(REPLACE "v" "" POLYHOOK_VERSION "${POLYHOOK_VERSION}")
string(REGEX REPLACE "[.+-]" ";" POLYHOOK_VERSION_LIST ${POLYHOOK_VERSION})
list(GET POLYHOOK_VERSION_LIST 0 POLYHOOK_VERSION_MAJOR)
list(GET POLYHOOK_VERSION_LIST 1 POLYHOOK_VERSION_MINOR)
list(GET POLYHOOK_VERSION_LIST 2 POLYHOOK_VERSION_PATCH)

project(plugify-plugin-polyhook
		VERSION "${POLYHOOK_VERSION_MAJOR}.${POLYHOOK_VERSION_MINOR}.${POLYHOOK_VERSION_PATCH}"
		DESCRIPTION "PolyHook Plugin" 
		HOMEPAGE_URL "https://github.com/untrustedmodders/plugify-plugin-polyhook"
		LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>")

if(NOT CMAKE_BUILD_TYPE MATCHES "Debug|Devel|MinSizeRel|RelWithDebInfo|Release")
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release.")
    set(CMAKE_BUILD_TYPE Release)
endif()

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

#
# Polyhook & DynLibUtils
#
include(FetchPolyhook)
#include(FetchDynlibUtils)

#
# Plugin
#
file(GLOB_RECURSE PLUGIN_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "src/*.cpp")

add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/plugify/include")

target_link_libraries(${PROJECT_NAME} PRIVATE PolyHook_2)
target_include_directories(${PROJECT_NAME} PRIVATE ${PolyHook_2_SOURCE_DIR})

if(MSVC)
    target_compile_options(asmjit PUBLIC /wd5054)
elseif(MINGW)
    target_compile_options(asmjit PUBLIC -Wno-deprecated-enum-enum-conversion)
else()
    target_compile_options(asmjit PUBLIC -Wno-deprecated-anon-enum-enum-conversion -Wno-deprecated-enum-enum-conversion)
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor $<$<CONFIG:Release>:/Zi>) # /W4 /WX 
    target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:-g1>) # -Wextra -Wshadow -Wconversion -Wpedantic -Werror
endif()

if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/sym/exported_symbols.lds")
elseif(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

include(GenerateExportHeader)
generate_export_header(${PROJECT_NAME} EXPORT_MACRO_NAME PLUGIN_API EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/exports/plugin_export.h)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/exports)

if(LINUX)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE -static-libstdc++ -static-libgcc)
    #target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>: -fsanitize=address -fsanitize=leak -fsanitize=undefined>)
    #target_link_libraries(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>: -fsanitize=address -fsanitize=leak -fsanitize=undefined>)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
        PLUGIFY_FORMAT_SUPPORT=$<BOOL:${COMPILER_SUPPORTS_FORMAT}> 
        PLUGIFY_IS_DEBUG=$<STREQUAL:${CMAKE_BUILD_TYPE},Debug>
        PLUGIFY_IS_RELEASE=$<STREQUAL:${CMAKE_BUILD_TYPE},Release>
)
if(NOT COMPILER_SUPPORTS_FORMAT)
    #target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt-header-only)
endif()

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/plugify-plugin-polyhook.pplugin.in
        ${CMAKE_CURRENT_BINARY_DIR}/plugify-plugin-polyhook.pplugin
)