#!/usr/bin/env bash
# Сборка и запуск всех self-test'ов LimVine (без внешних зависимостей).
set -u
FAILURES=0
cd "$(dirname "$0")"
CXX=${CXX:-g++}
FLAGS="-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -pthread -Iengine"
OUT=${OUT:-/tmp/limvine-tests}
mkdir -p "$OUT"

ENGINE_SRC=$(ls engine/lvscript/*.cpp engine/ecs/*.cpp engine/core/*.cpp engine/physics/*.cpp \
                  engine/physics/backends/*.cpp \
                  engine/input/*.cpp engine/audio/*.cpp engine/render/*.cpp engine/scripting/*.cpp \
                  engine/scripting/visual/*.cpp engine/asset/*.cpp engine/scene/*.cpp 2>/dev/null)

# --- Необязательный бэкенд Jolt Physics -------------------------------------
# Если указан JOLT_ROOT (исходники) и JOLT_LIB (собранная libJolt.a), тесты
# физики дополнительно прогоняются на настоящем Jolt. Без них собирается
# только встроенный симулятор — ни одна проверка при этом не пропадает,
# набор contract-тестов просто исполняется один раз вместо двух.
JOLT_FLAGS=""
JOLT_LIBS=""
if [ -n "${JOLT_ROOT:-}" ] && [ -n "${JOLT_LIB:-}" ] && [ -f "$JOLT_LIB" ]; then
  # ВНИМАНИЕ: Jolt проверяет в рантайме, что клиент собран с ТЕМ ЖЕ набором
  # дефайнов, что и сама библиотека, иначе аварийно завершается с
  # «Mismatching define ...» (структуры имеют разную раскладку). Значения по
  # умолчанию соответствуют сборке Jolt в конфигурации Release со штатными
  # опциями CMake; при другой конфигурации передайте свой JOLT_DEFS.
  JOLT_DEFS=${JOLT_DEFS:-"-DNDEBUG -DJPH_CROSS_PLATFORM_DETERMINISTIC -DJPH_USE_CPU_COMPUTE \
    -DJPH_DEBUG_RENDERER -DJPH_PROFILE_ENABLED -DJPH_OBJECT_STREAM \
    -DJPH_USE_AVX2 -DJPH_USE_AVX -DJPH_USE_SSE4_1 -DJPH_USE_SSE4_2 \
    -DJPH_USE_LZCNT -DJPH_USE_TZCNT -DJPH_USE_F16C \
    -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfpmath=sse"}
  JOLT_FLAGS="-DLV_WITH_JOLT -I$JOLT_ROOT $JOLT_DEFS"
  JOLT_LIBS="$JOLT_LIB"
  printf '>>> Jolt Physics: ON (%s)\n' "$JOLT_LIB"
else
  printf '>>> Jolt Physics: OFF (задайте JOLT_ROOT и JOLT_LIB, чтобы включить)\n'
fi

# Интеграционный тест линкует ВСЕ подсистемы в один TU-набор: на машинах с
# 1-2 ГБ ОЗУ -O2 может не хватить памяти для линковщика, поэтому для него
# используется -O1. На результат тестов это не влияет.
FLAGS_LINK_HEAVY="-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -pthread -Iengine"

run_suite () {
  local name="$1"; shift
  printf '\n>>> building %s\n' "$name"
  if ! $CXX $FLAGS_LINK_HEAVY -o "$OUT/$name" "$@"; then
    printf '!!! BUILD FAILED: %s\n' "$name"; FAILURES=$((FAILURES+1)); return
  fi
  printf '>>> running  %s\n' "$name"
  if ! "$OUT/$name"; then printf '!!! SUITE FAILED: %s\n' "$name"; FAILURES=$((FAILURES+1)); fi
}

simple_suite () {
  local name="$1"; shift
  printf '\n>>> building %s\n' "$name"
  if ! $CXX $FLAGS -o "$OUT/$name" "$@"; then
    printf '!!! BUILD FAILED: %s\n' "$name"; FAILURES=$((FAILURES+1)); return
  fi
  printf '>>> running  %s\n' "$name"
  if ! "$OUT/$name"; then printf '!!! SUITE FAILED: %s\n' "$name"; FAILURES=$((FAILURES+1)); fi
}

# LV Script зависит только от自身 подсистемы — собираем отдельно, чтобы
# ошибка в движке не мешала тестам языка.
LV_SRC=$(ls engine/lvscript/*.cpp)
simple_suite lvscript_tests  tests/lvscript_tests.cpp $LV_SRC
simple_suite ecs_tests       tests/ecs_tests.cpp engine/ecs/Registry.cpp
simple_suite render_tests    tests/render_tests.cpp engine/render/RenderTypes.cpp engine/render/Batcher.cpp engine/core/JobSystem.cpp
simple_suite input_tests     tests/input_tests.cpp engine/input/Input.cpp
simple_suite job_tests       tests/job_tests.cpp engine/core/JobSystem.cpp
simple_suite visualscript_tests tests/visualscript_tests.cpp engine/scripting/visual/VisualScript.cpp engine/scripting/visual/JsonMini.cpp $LV_SRC
# Стандартная библиотека LV Script (stdlib/*.lvs) проверяется отдельным сьютом:
# он грузит модули из рабочего каталога, поэтому запускается из корня репозитория.
simple_suite stdlib_tests    tests/stdlib_tests.cpp $LV_SRC
# Сериализация байткода (.lvc) и дисковый кэш: round-trip, переотображение
# таблицы имён между разными VM, устойчивость к повреждённым файлам.
simple_suite bytecode_tests  tests/bytecode_tests.cpp $LV_SRC
# Сборщик мусора: эквивалентность stop-the-world и инкрементального режимов,
# write-barrier, границы пауз, лимит памяти песочницы.
simple_suite gc_tests        tests/gc_tests.cpp $LV_SRC
# Иерархия сущностей: Parent/Children/WorldTransform, каскад world-матриц,
# защита от циклов и матричные хелперы (inverse/extract*).
simple_suite hierarchy_tests tests/hierarchy_tests.cpp engine/ecs/Registry.cpp \
                             engine/ecs/Hierarchy.cpp engine/ecs/World.cpp engine/core/JobSystem.cpp

# Сцены (.lvscene): round-trip всех типов полей, переотображение ссылок на
# сущности, восстановление иерархии и устойчивость к битым файлам.
run_suite scene_tests tests/scene_tests.cpp $ENGINE_SRC

# Геймплейные модули stdlib: CharacterController и InteractionSystem.
# Проверяется поведение (нормализация диагонали, гравитация, угол обзора,
# перекрытие стеной), а не факт компиляции.
run_suite gameplay_tests tests/gameplay_tests.cpp $ENGINE_SRC

# Физика: контрактный набор, который прогоняется на КАЖДОМ доступном бэкенде
# (встроенный симулятор и, если собрано с Jolt, настоящий Jolt). Так
# расхождения между реализациями ловятся в CI, а не в игре.
printf '\n>>> building physics_tests\n'
if ! $CXX $FLAGS_LINK_HEAVY $JOLT_FLAGS -o "$OUT/physics_tests" tests/physics_tests.cpp $ENGINE_SRC $JOLT_LIBS; then
  printf '!!! BUILD FAILED: physics_tests\n'; FAILURES=$((FAILURES+1))
else
  printf '>>> running  physics_tests\n'
  if ! "$OUT/physics_tests"; then printf '!!! SUITE FAILED: physics_tests\n'; FAILURES=$((FAILURES+1)); fi
fi

run_suite engine_integration_tests tests/engine_integration_tests.cpp $ENGINE_SRC

# Редактор: панели рисуются в TextUIDraw, поэтому ImGui не нужен.
run_suite editor_tests tests/editor_tests.cpp $ENGINE_SRC engine/editor/Panels.cpp engine/editor/EditorUI.cpp

# Шаблоны игр (templates/*) проверяются тем же headless-раннером, что используют
# редактор и CLI (tools/lvrun): тест «играет» в каждый шаблон — вспашка/посадка/
# рост/сбор урожая и смена дня; диалог -> квест -> бой и цепочка квестов;
# стрельба с разбросом, перезарядка, пикапы и движение от настоящей клавиатуры.
# Состояние читается через templateState(), а действия вызываются через
# TemplateRunner::callGlobalFn/callMethodOnGlobal (без эмуляции ввода по кадрам).
run_suite template_tests tests/template_tests.cpp $ENGINE_SRC

printf '\n=========================================\n'
if [ "$FAILURES" -eq 0 ]; then
  printf 'ALL LIMVINE SELF-TESTS PASSED\n'
  exit 0
else
  printf '%d SUITE(S) FAILED\n' "$FAILURES"
  exit 1
fi
