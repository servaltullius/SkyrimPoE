cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(POE_BUILD_ROOT "" CACHE PATH "빌드 산출물을 둘 폴더(프리셋에서 지정)")
set(POE_PACKAGE_DIR "${POE_BUILD_ROOT}/package" CACHE PATH "모드 폴더 모양으로 묶은 산출물")
set(POE_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../external/CommonLibSSE-NG" CACHE PATH "저장소의 CommonLibSSE-NG v16.4.0 서브모듈")
# 지도 팩(R1 도구 산출물). 묶을 때 DLL과 같은 모드 폴더에 넣는다.
set(POE_MAP_PACK_DIR "G:/skyrim-build/SkyrimPoE-maps" CACHE PATH "SkyrimPoE_Maps.esp와 .json이 있는 폴더")

if(NOT POE_BUILD_ROOT)
    message(FATAL_ERROR "POE_BUILD_ROOT가 비어 있습니다. CMakePresets.json의 프리셋으로 구성하세요.")
endif()
if(NOT EXISTS "${POE_COMMONLIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibSSE-NG를 찾지 못했습니다: '${POE_COMMONLIB_DIR}'.")
endif()

project(PoEProtoMapExperiment VERSION 0.1.0 LANGUAGES CXX)

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REX_OPTION_INI OFF CACHE BOOL "" FORCE)
set(REX_OPTION_JSON OFF CACHE BOOL "" FORCE)
set(REX_OPTION_TOML OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
add_subdirectory("${POE_COMMONLIB_DIR}" "${CMAKE_BINARY_DIR}/CommonLibSSE-NG" EXCLUDE_FROM_ALL)
include("${POE_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")

find_package(nlohmann_json CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "PoEProtoMapExperiment"
    VERSION "${PROJECT_VERSION}"
    COMPATIBLE_RUNTIMES "1.6.1170"
    SOURCES
        src/main.cpp
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W4)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
target_link_options(${PROJECT_NAME} PRIVATE /MAP)

add_custom_target(package_mod
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POE_PACKAGE_DIR}/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PoEProtoMapExperiment>"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/PoEProtoMapExperiment.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/PoEProtoMapExperiment.ini"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/PoEProtoMapExperiment.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_MAP_PACK_DIR}/SkyrimPoE_Maps.esp"
        "${POE_PACKAGE_DIR}/SkyrimPoE_Maps.esp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_MAP_PACK_DIR}/SkyrimPoE_Maps.json"
        "${POE_PACKAGE_DIR}/SkyrimPoE_Maps.json"
    DEPENDS PoEProtoMapExperiment
    VERBATIM
)
