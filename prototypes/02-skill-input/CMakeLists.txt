cmake_minimum_required(VERSION 3.28)
project(PoEProtoSkillInput VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

set(POE_BUILD_ROOT "" CACHE PATH "빌드 산출물을 둘 폴더(프리셋에서 지정)")
set(POE_PACKAGE_DIR "${POE_BUILD_ROOT}/package" CACHE PATH "모드 폴더 모양으로 묶은 산출물")
set(POE_COMMONLIB_DIR "" CACHE PATH "CommonLibSSE-NG(alandtse, v9) 체크아웃 폴더(프리셋에서 지정)")

if(NOT POE_BUILD_ROOT)
    message(FATAL_ERROR "POE_BUILD_ROOT가 비어 있습니다. CMakePresets.json의 프리셋으로 구성하세요.")
endif()
if(NOT EXISTS "${POE_COMMONLIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibSSE-NG를 찾지 못했습니다: '${POE_COMMONLIB_DIR}'. "
        "CMakePresets.json의 POE_COMMONLIB_DIR을 확인하세요.")
endif()

# 한 DLL로 SE 1.5.97과 AE 1.6.x를 함께 지원한다.
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

# 게임 타입에 기대지 않는 부분. 테스트가 이것만 링크한다.
add_library(poe_proto_core STATIC
    src/Core/Config.cpp
    src/Core/EventNames.cpp
    src/Core/Spin.cpp
)
target_include_directories(poe_proto_core PUBLIC src)
target_compile_options(poe_proto_core PUBLIC /utf-8)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "PoEProtoSkillInput"
    VERSION "${PROJECT_VERSION}"
    USE_ADDRESS_LIBRARY
    SOURCES
        src/main.cpp
)
target_sources(${PROJECT_NAME} PRIVATE
    src/Runtime/Channel.cpp
    src/Runtime/Hooks.cpp
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# CommonLib v9가 <Windows.h>를 끌어오므로 min/max 매크로를 막는다.
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
target_link_libraries(${PROJECT_NAME} PRIVATE poe_proto_core)
target_include_directories(${PROJECT_NAME} PRIVATE src)

add_custom_target(package_mod
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POE_PACKAGE_DIR}/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PoEProtoSkillInput>"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/PoEProtoSkillInput.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/PoEProtoSkillInput.ini"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/PoEProtoSkillInput.ini"
    DEPENDS PoEProtoSkillInput
)

if(BUILD_TESTING)
    foreach(test_name ConfigTests EventNamesTests SpinTests)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE poe_proto_core)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
