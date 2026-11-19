cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(POE_BUILD_ROOT "" CACHE PATH "빌드 산출물을 둘 폴더(프리셋에서 지정)")
set(POE_PACKAGE_DIR "${POE_BUILD_ROOT}/package" CACHE PATH "모드 폴더 모양으로 묶은 산출물")
set(POE_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/CommonLibSSE-NG" CACHE PATH "저장소의 CommonLibSSE-NG v16.4.0 서브모듈")
set(POE_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../data" CACHE PATH "데이터 팩·언어 팩 폴더")
set(POE_ESP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../esp/SkyrimPoE.esp" CACHE FILEPATH "SkyrimPoE.esp 경로")
set(POE_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui" CACHE PATH "Prisma UI 화면 폴더")
set(POE_OAR_PACKAGER "${CMAKE_CURRENT_SOURCE_DIR}/../tools/package_oar.py" CACHE FILEPATH "사이클론 OAR 패키징 스크립트")
set(POE_TREE_COMPILER "${CMAKE_CURRENT_SOURCE_DIR}/../tools/compiler/compile_tree.py" CACHE FILEPATH "패시브 트리 컴파일러/래퍼 스크립트")

if(NOT POE_BUILD_ROOT)
    message(FATAL_ERROR "POE_BUILD_ROOT가 비어 있습니다. CMakePresets.json의 프리셋으로 구성하세요.")
endif()
if(NOT EXISTS "${POE_COMMONLIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibSSE-NG를 찾지 못했습니다: '${POE_COMMONLIB_DIR}'. "
        "저장소의 서브모듈 또는 POE_COMMONLIB_DIR 설정을 확인하세요.")
endif()

project(SkyrimPoE VERSION 0.1.0 LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

# 1. 엔진 비의존 순수 코어 정적 라이브러리 (DLL과 ctest가 공용)
add_library(skyrimpoe_core STATIC
    src/core/Config.cpp
    src/core/DataPack.cpp
    src/core/CommandQueue.cpp
    src/core/StatEngine.cpp
    src/core/DamageConversion.cpp
    src/core/EnergyShield.cpp
    src/core/CycloneState.cpp
    src/core/SkillSystem.cpp
    src/core/KillTracker.cpp
    src/core/GemTransaction.cpp
    src/core/PassiveTreeData.cpp
    src/core/PassiveTree.cpp
    src/core/PassiveStatParser.cpp
    src/core/ItemData.cpp
    src/core/ItemAffixParser.cpp
    src/core/ItemCrafting.cpp
    src/core/ItemFormatter.cpp
    src/core/LootTable.cpp
)
target_include_directories(skyrimpoe_core PUBLIC src)
target_compile_definitions(skyrimpoe_core PUBLIC NOMINMAX)
target_compile_options(skyrimpoe_core PRIVATE /utf-8 /W4)
target_link_libraries(skyrimpoe_core PUBLIC nlohmann_json::nlohmann_json)

# 2. SKSE 플러그인 DLL (CommonLibSSE-NG v16.4.0, SE+AE 겸용 틀 + AE 1.6.1170 우선)
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

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "SkyrimPoE"
    VERSION "${PROJECT_VERSION}"
    COMPATIBLE_RUNTIMES "1.6.1170" "1.5.97"
    SOURCES
        src/main.cpp
        src/runtime/MainUpdateHook.cpp
        src/runtime/UniqueIdUtil.cpp
        src/runtime/GemInventory.cpp
        src/runtime/ItemTracker.cpp
        src/runtime/EquipmentSync.cpp
        src/runtime/CraftingRuntime.cpp
        src/runtime/WorldDropTracker.cpp
        src/runtime/SaveLoad.cpp
        src/runtime/SkillExecution.cpp
        src/runtime/CombatHooks.cpp
        src/runtime/CycloneRuntime.cpp
        src/runtime/DeathListener.cpp
        src/runtime/SimpleImeBridge.cpp
        src/runtime/UiBridge.cpp
        src/runtime/AutoTest.cpp
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W4)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE skyrimpoe_core)
target_link_options(${PROJECT_NAME} PRIVATE /MAP)

# 3. 엔진 비의존 단위 테스트 (ctest)
enable_testing()
add_executable(skyrimpoe_tests
    tests/test_main.cpp
    tests/test_datapack.cpp
    tests/test_conversion.cpp
    tests/test_command_queue.cpp
    tests/test_cyclone_coc.cpp
    tests/test_kill_tracker.cpp
    tests/test_gem_transaction.cpp
    tests/test_passive_tree.cpp
    tests/test_passive_stats.cpp
    tests/test_items.cpp
)
target_compile_options(skyrimpoe_tests PRIVATE /utf-8 /W4)
target_compile_definitions(skyrimpoe_tests PRIVATE
    POE_TEST_DATA_DIR="${POE_DATA_DIR}"
    POE_TEST_PLUGIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(skyrimpoe_tests PRIVATE skyrimpoe_core)
add_test(NAME skyrimpoe_core_tests COMMAND skyrimpoe_tests)

# 4. 모드 폴더 모양 패키징 (DLL + INI + ESP + 데이터/언어 팩 + Prisma UI + 사이클론 OAR)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(package_mod
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/packs"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/lang"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SkyrimPoE>"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/SkyrimPoE.ini"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_ESP_FILE}"
        "${POE_PACKAGE_DIR}/SkyrimPoE.esp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_DATA_DIR}/packs/core_3_30.json"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/packs/core_3_30.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_DATA_DIR}/packs/tree_3_30.json"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/packs/tree_3_30.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_DATA_DIR}/packs/items_3_30.json"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/packs/items_3_30.json"
    COMMAND "${Python3_EXECUTABLE}" "${POE_TREE_COMPILER}"
        --wrap-only-from "${POE_DATA_DIR}/packs/tree_3_30.json"
        --js-out "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE/tree-data.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_DATA_DIR}/lang/ko-KR.json"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/lang/ko-KR.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_DATA_DIR}/lang/en-US.json"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/lang/en-US.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_UI_DIR}/index.html"
        "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_UI_DIR}/style.css"
        "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE/style.css"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_UI_DIR}/app.js"
        "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE/app.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_UI_DIR}/tree.js"
        "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE/tree.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_UI_DIR}/gear.js"
        "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE/gear.js"
    COMMAND "${Python3_EXECUTABLE}" "${POE_OAR_PACKAGER}" --package-dir "${POE_PACKAGE_DIR}"
    DEPENDS SkyrimPoE
    VERBATIM
)
