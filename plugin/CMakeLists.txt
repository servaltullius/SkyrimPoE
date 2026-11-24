cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(POE_BUILD_ROOT "" CACHE PATH "빌드 산출물을 둘 폴더(프리셋에서 지정)")
set(POE_PACKAGE_DIR "${POE_BUILD_ROOT}/package" CACHE PATH "모드 폴더 모양으로 묶은 산출물")
set(POE_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/CommonLibSSE-NG" CACHE PATH "저장소의 CommonLibSSE-NG v16.4.0 서브모듈")
set(POE_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../data" CACHE PATH "데이터 팩·언어 팩 폴더")
set(POE_ESP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../esp/SkyrimPoE.esp" CACHE FILEPATH "SkyrimPoE.esp 경로")
set(POE_MAPS_PACK_DIR "G:/skyrim-build/SkyrimPoE-maps" CACHE PATH "지도 팩 ESP와 목록 파일 폴더")
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
    src/core/CharmSystem.cpp
    src/core/AilmentSystem.cpp
    src/core/MonsterModSystem.cpp
    src/core/WorldStampLink.cpp
    src/core/MapSystem.cpp
    src/core/MapPack.cpp
    src/core/MapTravel.cpp
    src/core/EnemyEsHud.cpp
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
        src/runtime/MapDeviceRuntime.cpp
        src/runtime/MapPackRuntime.cpp
        src/runtime/MonsterRuntime.cpp
        src/runtime/AilmentRuntime.cpp
        src/runtime/ProjectileRuntime.cpp
        src/runtime/ProxySpellRuntime.cpp
        src/runtime/CombatHooks.cpp
        src/runtime/CycloneRuntime.cpp
        src/runtime/DeathListener.cpp
        src/runtime/SimpleImeBridge.cpp
        src/runtime/UiBridge.cpp
        src/runtime/TrueHudBridge.cpp
        src/runtime/AutoTest.cpp
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W4)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE skyrimpoe_core bcrypt)
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
    tests/test_spirit_breakdown.cpp
    tests/test_items.cpp
    tests/test_stage5.cpp
    tests/test_world_stamp_link.cpp
    tests/test_stage6.cpp
    tests/test_map_difficulty.cpp
    tests/test_map_pack.cpp
    tests/test_map_auto_exits.cpp
    tests/test_map_save.cpp
    tests/test_map_runtime_rules.cpp
    tests/test_map_kill_rewards.cpp
    tests/test_map_travel.cpp
    tests/test_map_spawn_disable_wait.cpp
    tests/test_enemy_es_hud.cpp
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
# 결정 169, R4: package_mod 실행 시점에 ESP·목록이 한 쌍인지 확인한다.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/VerifyMapsPack.cmake" CONTENT [=[
# esp·manifest: 검사할 ESP와 목록 파일 경로(원본 팩 폴더와 패키지 양쪽에 쓴다)
foreach(path IN ITEMS "${esp}" "${manifest}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "지도 팩 파일을 찾을 수 없습니다: ${path}")
    endif()
endforeach()
file(READ "${manifest}" json)
string(JSON format_type ERROR_VARIABLE format_error TYPE "${json}" manifestFormat)
if(NOT format_error)
    string(JSON format ERROR_VARIABLE format_error GET "${json}" manifestFormat)
endif()
if(format_error OR NOT format_type STREQUAL "NUMBER" OR NOT format STREQUAL "4")
    message(FATAL_ERROR "지도 목록 형식 4가 필요합니다: ${manifest}")
endif()
# 형식 4: 최상위 숫자 exitZoneRadius(결정 177 ③ 재보완)가 있어야 런타임이 팩을 받는다.
string(JSON radius_type ERROR_VARIABLE radius_error TYPE "${json}" exitZoneRadius)
if(radius_error OR NOT radius_type STREQUAL "NUMBER")
    message(FATAL_ERROR "지도 목록에 숫자 exitZoneRadius가 없습니다: ${manifest}")
endif()
# 런타임(core/MapPack.h kMaxExitZoneRadius)과 같은 범위여야 설치 뒤 팩 전체가 거부되지 않는다.
string(JSON radius GET "${json}" exitZoneRadius)
if(NOT radius GREATER 0 OR radius GREATER 10000)
    message(FATAL_ERROR "지도 목록의 exitZoneRadius(${radius})는 0보다 크고 10000 이하여야 합니다: ${manifest}")
endif()
# 형식 3부터: 던전마다 숫자 navmeshMinZ가 있어야 런타임이 팩을 받는다(결정 177 ③).
string(JSON dungeon_count ERROR_VARIABLE dungeons_error LENGTH "${json}" dungeons)
if(dungeons_error OR dungeon_count EQUAL 0)
    message(FATAL_ERROR "지도 목록의 dungeons를 읽지 못했습니다: ${manifest}")
endif()
math(EXPR last_dungeon "${dungeon_count} - 1")
foreach(index RANGE ${last_dungeon})
    string(JSON z_type ERROR_VARIABLE z_error TYPE "${json}" dungeons ${index} navmeshMinZ)
    if(z_error OR NOT z_type STREQUAL "NUMBER")
        message(FATAL_ERROR "지도 목록의 던전 ${index}에 숫자 navmeshMinZ가 없습니다: ${manifest}")
    endif()
endforeach()
string(JSON expected ERROR_VARIABLE hash_error GET "${json}" espSha256)
if(hash_error)
    message(FATAL_ERROR "지도 목록의 espSha256을 읽지 못했습니다: ${manifest}")
endif()
string(TOLOWER "${expected}" expected)
file(SHA256 "${esp}" actual)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "지도 팩 ESP와 목록 SHA-256 불일치. 목록: ${expected}, ESP: ${actual}")
endif()
]=])
add_custom_target(package_mod
    COMMAND ${CMAKE_COMMAND}
        "-Desp=${POE_MAPS_PACK_DIR}/SkyrimPoE_Maps.esp"
        "-Dmanifest=${POE_MAPS_PACK_DIR}/SkyrimPoE_Maps.json"
        -P "${CMAKE_CURRENT_BINARY_DIR}/VerifyMapsPack.cmake"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/packs"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/maps"
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
        "${POE_MAPS_PACK_DIR}/SkyrimPoE_Maps.esp"
        "${POE_PACKAGE_DIR}/SkyrimPoE_Maps.esp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_MAPS_PACK_DIR}/SkyrimPoE_Maps.json"
        "${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/maps/SkyrimPoE_Maps.json"
    # 복사한 두 파일이 여전히 한 쌍인지 패키지에서 다시 확인한다(검사와 복사 사이에 팩이 바뀐 경우).
    COMMAND ${CMAKE_COMMAND}
        "-Desp=${POE_PACKAGE_DIR}/SkyrimPoE_Maps.esp"
        "-Dmanifest=${POE_PACKAGE_DIR}/SKSE/Plugins/SkyrimPoE/maps/SkyrimPoE_Maps.json"
        -P "${CMAKE_CURRENT_BINARY_DIR}/VerifyMapsPack.cmake"
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POE_UI_DIR}/maps.js"
        "${POE_PACKAGE_DIR}/PrismaUI/views/SkyrimPoE/maps.js"
    COMMAND "${Python3_EXECUTABLE}" "${POE_OAR_PACKAGER}" --package-dir "${POE_PACKAGE_DIR}"
    DEPENDS SkyrimPoE
    VERBATIM
)
