# ============================================================================
#  LimVine — игровой 3D-движок (C++20)
#
#  Сборка по умолчанию НЕ требует внешних зависимостей: Vulkan/OpenGL, Jolt,
#  OpenAL и ImGui подключаются опциями LV_WITH_*, а вместо них работают
#  Null-бэкенды, поэтому дерево собирается и проходит все self-test'ы headless.
#
#  Быстрый старт:
#      cmake -B build -G Ninja
#      cmake --build build -j
#      ctest --test-dir build --output-on-failure
#
#  С опциональными бэкендами:
#      cmake -B build -DLV_WITH_VULKAN=ON -DLV_WITH_JOLT=ON -DLV_WITH_OPENAL=ON
#
#  Примечание про память: на машинах с 1–2 ГБ ОЗУ линковка тяжёлых целей с -O2
#  может завершиться без сообщения (collect2/ld). Поэтому по умолчанию -O1 для
#  тестовых экзешников; для библиотеки оптимизация задаётся стандартным
#  CMAKE_BUILD_TYPE.
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(LimVine
        VERSION 0.1.0
        DESCRIPTION "LimVine: low-poly 3D engine with ECS, job system and LV Script"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  Опции
# ---------------------------------------------------------------------------
option(LV_BUILD_TESTS      "Собирать self-test'ы и регистрировать их в ctest" ON)
option(LV_BUILD_TOOLS      "Собирать CLI-утилиты (lvrun)"                     ON)
option(LV_BUILD_EDITOR     "Собирать редактор (требует LV_WITH_IMGUI)"        OFF)
option(LV_WERROR           "Трактовать предупреждения как ошибки"             OFF)

option(LV_WITH_VULKAN      "Vulkan-бэкенд рендера (иначе NullGraphicsAPI)"    OFF)
option(LV_WITH_OPENGL      "OpenGL-бэкенд рендера"                            OFF)
option(LV_WITH_JOLT        "Физика Jolt (иначе встроенный детерминированный симулятор)" OFF)
option(LV_WITH_OPENAL      "Звук OpenAL (иначе NullAudioBackend)"             OFF)
option(LV_WITH_IMGUI       "ImGui для редактора и отладочных панелей"         OFF)
option(LV_BUILD_SHARED     "Собирать движок как shared-библиотеку (по умолчанию статически)" OFF)

# ---------------------------------------------------------------------------
#  Базовые настройки компилятора
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    # production-сборка под Windows: CRT только динамическая (MD/MDd).
    # Статический CRT (/MT) в статической библиотеке движка запрещён самим
    # MSVC при линковке с /GL-объектами других конфигураций и ломает
    # единообразие аллокаторов в DLL-потребителях.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # Отключённые предупреждения (осознанно, не глобальный /W0):
    #   /wd4100 unreferenced formal parameter — аналог -Wno-unused-parameter
    #     выше; сигнатуры интерфейсов (UIDraw, IPhysicsBackend) часто шире
    #     конкретных реализаций.
    #   /wd4505 unreferenced local function has been removed — static-хелперы
    #     в тестах, используемые не во всех сборочных конфигурациях.
    add_compile_options(/wd4100 /wd4505)
    if(LV_WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-result)
    if(LV_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
#  Библиотека движка
# ---------------------------------------------------------------------------
file(GLOB_RECURSE LV_ENGINE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp)

# Редактор: панели и UI-прослойка входят в ядро (они не зависят от ImGui —
# рисование идёт через UIDraw, а TextUIDraw позволяет тестировать их headless).
# Файлы, требующие самого ImGui, и точка входа редактора собираются отдельно.
list(FILTER LV_ENGINE_SOURCES EXCLUDE REGEX ".*/engine/editor/ImGuiBackend\\.cpp$")
list(FILTER LV_ENGINE_SOURCES EXCLUDE REGEX ".*/engine/editor/main\\.cpp$")

if(LV_BUILD_SHARED)
    add_library(limvine SHARED ${LV_ENGINE_SOURCES})
    # Макрос LV_BUILD_SHARED нужен заголовку core/Api.h, чтобы отличать
    # сборку DLL от статической; PUBLIC — чтобы потребители видели dllimport.
    target_compile_definitions(limvine PUBLIC LV_BUILD_SHARED=1)
else()
    add_library(limvine STATIC ${LV_ENGINE_SOURCES})
    target_compile_definitions(limvine PUBLIC LIMVINE_STATIC=1)
endif()
add_library(limvine::limvine ALIAS limvine)

set_target_properties(limvine PROPERTIES OUTPUT_NAME "limvine")

# Внутренние символы движка не должны «протекать» в consumers: у MSVC их и
# так нет (нет __declspec(dllexport)), а на GCC/Clang скрываем всё, кроме
# явно экспортируемого API (см. LV_API в engine/core/Api.h).
set_target_properties(limvine PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_include_directories(limvine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine>
    $<INSTALL_INTERFACE:include/limvine>)

target_compile_features(limvine PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(limvine PUBLIC Threads::Threads)

# Макросы опциональных бэкендов: код внутри #ifdef LV_WITH_* подключает
# настоящие реализации, иначе остаются Null-бэкенды.
# Макрос LV_WITH_<OPT> выставляется ТОЛЬКО если зависимость реально найдена:
# иначе код начинал бы включать отсутствующие заголовки (<imgui.h>, <vulkan/…>).
# JOLT сознательно не в этом списке: его макрос выставляется ниже и ТОЛЬКО
# после того, как библиотека действительно найдена. Иначе JoltBackend.cpp
# начинал бы включать <Jolt/Jolt.h>, которого нет, и сборка падала бы вместо
# аккуратного отката на встроенный симулятор.
foreach(opt VULKAN OPENGL OPENAL)
    if(LV_WITH_${opt})
        target_compile_definitions(limvine PUBLIC LV_WITH_${opt}=1)
    endif()
endforeach()

if(LV_WITH_VULKAN)
    find_package(Vulkan REQUIRED)
    target_link_libraries(limvine PUBLIC Vulkan::Vulkan)
endif()
if(LV_WITH_OPENGL)
    find_package(OpenGL REQUIRED)
    target_link_libraries(limvine PUBLIC OpenGL::GL)
endif()
if(LV_WITH_OPENAL)
    find_package(OpenAL REQUIRED)
    target_link_libraries(limvine PUBLIC ${OPENAL_LIBRARY})
    target_include_directories(limvine PUBLIC ${OPENAL_INCLUDE_DIR})
endif()
# --- Jolt Physics ----------------------------------------------------------
# Два способа подключения:
#   1) установленный пакет:  cmake -B build -DLV_WITH_JOLT=ON -DJolt_DIR=/путь/к/JoltConfig.cmake
#   2) исходники подпроектом: cmake -B build -DLV_WITH_JOLT=ON -DLV_JOLT_SOURCE_DIR=/путь/к/JoltPhysics
#
# Вариант с целью Jolt::Jolt предпочтителен: Jolt проверяет В РАНТАЙМЕ, что
# клиентский код собран с тем же набором макросов (JPH_CROSS_PLATFORM_DETERMINISTIC,
# JPH_PROFILE_ENABLED, JPH_DEBUG_RENDERER, JPH_OBJECT_STREAM и набор SIMD).
# При несовпадении процесс аварийно завершается с «Mismatching define ...»,
# потому что раскладка структур различается. Импортированная цель приносит эти
# макросы через INTERFACE_COMPILE_DEFINITIONS, и рассинхронизация невозможна.
set(LV_JOLT_SOURCE_DIR "" CACHE PATH "Каталог исходников JoltPhysics (сборка подпроектом)")
if(LV_WITH_JOLT)
    set(LV_JOLT_OK FALSE)
    if(LV_JOLT_SOURCE_DIR AND EXISTS "${LV_JOLT_SOURCE_DIR}/Build/CMakeLists.txt")
        add_subdirectory("${LV_JOLT_SOURCE_DIR}/Build" ${CMAKE_BINARY_DIR}/_jolt EXCLUDE_FROM_ALL)
        target_link_libraries(limvine PUBLIC Jolt)
        set(LV_JOLT_OK TRUE)
    else()
        find_package(Jolt CONFIG QUIET)
        if(Jolt_FOUND)
            target_link_libraries(limvine PUBLIC Jolt::Jolt)
            set(LV_JOLT_OK TRUE)
        endif()
    endif()

    if(LV_JOLT_OK)
        target_compile_definitions(limvine PUBLIC LV_WITH_JOLT=1)
        message(STATUS "LimVine: физический бэкенд Jolt включён")
    else()
        message(WARNING
            "LV_WITH_JOLT=ON, но Jolt не найден — остаётся встроенный симулятор. "
            "Укажите -DJolt_DIR=... или -DLV_JOLT_SOURCE_DIR=...")
    endif()
endif()
if(LV_WITH_IMGUI)
    find_package(imgui CONFIG QUIET)
    if(imgui_FOUND)
        target_compile_definitions(limvine PUBLIC LV_WITH_IMGUI=1)
        target_link_libraries(limvine PUBLIC imgui::imgui)
        set(LV_IMGUI_OK TRUE)
    else()
        set(LV_IMGUI_OK FALSE)
        message(WARNING "LV_WITH_IMGUI=ON, но imgui не найден: ImGui-бэкенд редактора "
                        "останется пустой трансляционной единицей (панели работают через TextUIDraw)")
    endif()
endif()

# ---------------------------------------------------------------------------
#  CLI-утилиты
# ---------------------------------------------------------------------------
if(LV_BUILD_TOOLS)
    add_executable(lvrun tools/lvrun.cpp)
    target_link_libraries(lvrun PRIVATE limvine)
    # Шаблоны ищутся относительно рабочего каталога, поэтому по умолчанию
    # запускаем из корня репозитория.
    set_target_properties(lvrun PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    # «Повар» контента: прогоняет все .lvs и шаблоны через парсер+компилятор
    # (код возврата 1 при первой же ошибке) и умеет собирать шаблон в один файл.
    add_executable(lvcook tools/lvcook.cpp)
    target_link_libraries(lvcook PRIVATE limvine)
    set_target_properties(lvcook PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
#  Редактор
# ---------------------------------------------------------------------------
if(LV_BUILD_EDITOR)
    # ImGui не обязателен: без него редактор работает в headless-режиме
    # (--dump-panels) и печатает панели текстом. Оконный цикл требует
    # LV_WITH_IMGUI=ON и найденного imgui.
    if(LV_WITH_IMGUI AND NOT LV_IMGUI_OK)
        message(WARNING "LV_BUILD_EDITOR=ON без найденного imgui: собирается только headless-режим")
    endif()
    add_executable(limvine-editor
        engine/editor/main.cpp
        engine/editor/ImGuiBackend.cpp)
    target_link_libraries(limvine-editor PRIVATE limvine)
endif()

# ---------------------------------------------------------------------------
#  Self-test'ы
#
#  Каждый сьют — отдельный экзешник без внешнего тестового фреймворка: они
#  печатают PASS/FAIL и возвращают код 0/1, поэтому ctest работает «из коробки».
#  Сьюты stdlib и templates читают файлы из корня репозитория, поэтому их
#  WORKING_DIRECTORY фиксирован.
# ---------------------------------------------------------------------------
if(LV_BUILD_TESTS)
    enable_testing()

    file(GLOB LV_LVSCRIPT_SOURCES CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/engine/lvscript/*.cpp)

    # Сьюты, которым нужна вся библиотека движка.
    set(LV_HEAVY_SUITES
        engine_integration_tests
        template_tests
        editor_tests
        # Сериализация сцен: использует рефлексию BindingRegistry, поэтому
        # тянет рендер, физику и звук ради их компонентов.
        scene_tests
        # Геймплейные модули stdlib (character/interaction): им нужны живые
        # биндинги движка — spawn/getPosition/raycast.
        gameplay_tests
        # Контракт физики: один и тот же набор проверок прогоняется на каждом
        # доступном бэкенде (встроенный + Jolt, если он включён).
        physics_tests)

    # Сьюты, которым хватает части исходников (быстрее собираются и линкуются).
    add_executable(lvscript_tests     tests/lvscript_tests.cpp ${LV_LVSCRIPT_SOURCES})
    add_executable(ecs_tests          tests/ecs_tests.cpp engine/ecs/Registry.cpp)
    add_executable(render_tests       tests/render_tests.cpp engine/render/RenderTypes.cpp
                                       engine/render/Batcher.cpp engine/core/JobSystem.cpp)
    add_executable(input_tests        tests/input_tests.cpp engine/input/Input.cpp)
    add_executable(job_tests          tests/job_tests.cpp engine/core/JobSystem.cpp)
    add_executable(visualscript_tests tests/visualscript_tests.cpp
                                       engine/scripting/visual/VisualScript.cpp
                                       engine/scripting/visual/JsonMini.cpp
                                       ${LV_LVSCRIPT_SOURCES})
    add_executable(stdlib_tests       tests/stdlib_tests.cpp ${LV_LVSCRIPT_SOURCES})
    # Сериализация байткода и дисковый кэш .lvc.
    add_executable(bytecode_tests     tests/bytecode_tests.cpp ${LV_LVSCRIPT_SOURCES})
    # Сборщик мусора: корректность, трёхцветная инварианта, инкрементальный режим.
    add_executable(gc_tests           tests/gc_tests.cpp ${LV_LVSCRIPT_SOURCES})
    # Иерархия сущностей: Parent/Children/WorldTransform + матричные хелперы.
    add_executable(hierarchy_tests    tests/hierarchy_tests.cpp
                                       engine/ecs/Registry.cpp
                                       engine/ecs/Hierarchy.cpp
                                       engine/ecs/World.cpp
                                       engine/core/JobSystem.cpp)

    foreach(suite lvscript_tests ecs_tests render_tests input_tests job_tests
                  visualscript_tests stdlib_tests bytecode_tests gc_tests hierarchy_tests)
        target_include_directories(${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)
        target_link_libraries(${suite} PRIVATE Threads::Threads)
        add_test(NAME ${suite} COMMAND ${suite}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    if(LV_BUILD_TOOLS)
        # Контент-тест: все stdlib/*.lvs и все шаблоны обязаны компилироваться.
        add_test(NAME lvcook_check COMMAND lvcook check --quiet
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    foreach(suite ${LV_HEAVY_SUITES})
        add_executable(${suite} tests/${suite}.cpp)
        target_include_directories(${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)
        target_link_libraries(${suite} PRIVATE limvine)
        # -O1: на машинах с 1–2 ГБ ОЗУ линковка с -O2 падает без сообщения.
        if(NOT MSVC)
            target_compile_options(${suite} PRIVATE -O1)
        endif()
        add_test(NAME ${suite} COMMAND ${suite}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
endif()

# ---------------------------------------------------------------------------
#  Экспорт CMake (production): потребитель находит движок одной строкой
#      find_package(LimVine REQUIRED)
#      target_link_libraries(mygame PRIVATE limvine::limvine)
#  Конфигурация генерируется из LimVineConfig.cmake.in и устанавливается
#  вместе с целями, поэтому сборка работает и из build-дерева, и после
#  install в системный префикс.
#
#  Важно: экспорт limvine возможен только при статической сборке — у
#  INTERFACE-зависимостей без найденных пакетов (Null-бэкенды) нет рантайм-
#  артефактов, которые можно было бы прописать в LimVineTargets.cmake.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT LV_BUILD_SHARED)
    install(TARGETS limvine EXPORT LimVineTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(LV_BUILD_TOOLS)
        install(TARGETS lvrun lvcook EXPORT LimVineTargets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(EXPORT LimVineTargets
            FILE LimVineTargets.cmake
            NAMESPACE limvine::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LimVine)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LimVineConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/LimVineConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LimVine)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/LimVineConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/LimVineConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/LimVineConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LimVine)
else()
    # Для shared-сборки цели движка и инструментов устанавливаются в те же
    # стандартные каталоги (bin/lib), но без CMake-экспорта: потребитель
    # подключает их вручную через find_library/find_path.
    install(TARGETS limvine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(LV_BUILD_TOOLS)
        install(TARGETS lvrun lvcook RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

install(DIRECTORY engine/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/limvine
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY stdlib templates DESTINATION ${CMAKE_INSTALL_DATADIR}/limvine)

# ---------------------------------------------------------------------------
#  CPack: готовые инсталляторы (Windows — NSIS .exe, Linux — TGZ/DEB)
#  Подробнее в packaging/README.md; все переменные переопределяются с -D.
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "LimVine")
set(CPACK_PACKAGE_VENDOR "LimVine Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "LimVine: low-poly 3D game engine with ECS, job system and LV Script")
set(CPACK_PACKAGE_CONTACT "https://github.com/limvine/limvine")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "LimVine")
string(TIMESTAMP LV_BUILD_DATE "%Y%m%d")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_RESOURCE_FILE_README ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
set(CPACK_SOURCE_IGNORE_FILES "/build/;/\\\\.git/;dist/")
if(WIN32)
    # Многоязычный NSIS-мастер установки + uninstall.exe, ярлыки, PATH.
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "LimVine ${PROJECT_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME "LimVine")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\lvrun.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH ON)
    set(CPACK_NSIS_MUI_ICON ${CMAKE_CURRENT_SOURCE_DIR}/packaging/limvine.ico)
    set(CPACK_NSIS_MUI_UNIICON ${CMAKE_CURRENT_SOURCE_DIR}/packaging/limvine.ico)
    set(CPACK_NSIS_MENU_LINKS
        "bin/lvrun.exe" "LimVine Runner (lvrun)"
        "bin/lvcook.exe" "LimVine Content Cooker (lvcook)")
    # Язык мастера: русский по умолчанию, английский как запасной.
    set(CPACK_NSIS_LANGUAGE_LIST "Russian English")
    # CPack сам прописывает в реестр ключи Add/Remove Programs (UninstallString,
    # DisplayIcon, DisplayVersion) и генерирует uninstall.exe; ярлыки меню
    # «Пуск» создаются из CPACK_NSIS_MENU_LINKS выше.
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
include(CPack)

message(STATUS "LimVine ${PROJECT_VERSION}: tests=${LV_BUILD_TESTS} tools=${LV_BUILD_TOOLS} "
               "editor=${LV_BUILD_EDITOR} vulkan=${LV_WITH_VULKAN} opengl=${LV_WITH_OPENGL} "
               "jolt=${LV_WITH_JOLT} openal=${LV_WITH_OPENAL} imgui=${LV_WITH_IMGUI}")
