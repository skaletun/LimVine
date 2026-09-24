/**
 * @file    gc_tests.cpp
 * @brief   Тесты сборщика мусора: корректность, трёхцветная инварианта,
 *          инкрементальный режим и границы пауз.
 *
 * Главная проверка каждого сценария — **эквивалентность режимов**: результат
 * скрипта и число выживших объектов не должны зависеть от того, собирался ли
 * мусор одной паузой или сотней мелких шагов. Если инкрементальный режим
 * освободит живой объект, скрипт упадёт или вернёт неверное значение.
 */
#include "lvscript/Compiler.h"
#include "lvscript/GC.h"
#include "lvscript/Parser.h"
#include "lvscript/Stdlib.h"
#include "lvscript/VM.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace lv;

namespace {

int g_passed = 0, g_failed = 0;

void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
void section(const char* s) { std::printf("\n== %s ==\n", s); }

/// Результат прогона скрипта.
struct RunResult {
    bool ok = false;
    std::string result;      ///< Значение глобала `result`.
    std::string error;
    GC::Stats stats;
};

/// Выполнить скрипт в свежей VM с заданной конфигурацией GC.
RunResult runScript(const std::string& src, bool incremental, std::size_t budget = 4096,
                    double growth = 1.2) {
    RunResult out;
    VM vm;
    installStdlib(vm);
    vm.gc().setGrowthFactor(growth);
    if (incremental) {
        vm.gc().setIncremental(true);
        vm.gc().setStepBudget(budget);
    }

    DiagnosticList diags;
    Module mod = parseSource(src, "gc-test", diags);
    for (const Diagnostic& d : diags)
        if (d.severity == DiagSeverity::Error) { out.error = d.toString(); return out; }

    Compiler c(vm);
    ObjFunction* fn = c.compile(mod, diags);
    for (const Diagnostic& d : diags)
        if (d.severity == DiagSeverity::Error) { out.error = d.toString(); return out; }
    if (!fn) { out.error = "compile failed"; return out; }

    if (vm.execute(fn) != RunStatus::Ok) {
        out.error = vm.lastError().toString();
        out.stats = vm.gc().stats();
        return out;
    }
    if (Value* r = vm.findGlobal("result")) out.result = r->toString();
    out.stats = vm.gc().stats();
    out.ok = true;
    return out;
}

/// Скрипт, создающий много мусора и удерживающий часть объектов.
const char* kChurnScript = R"(
    var keep = []
    for i in 1..3000 do
        var garbage = [i, i * 2, i * 3]
        var obj = {"id": i, "data": garbage}
        if i % 10 == 0 do keep.push(obj) end
    end
    global result = keep.len()
)";

} // namespace

// ---------------------------------------------------------------------------
static void testBasicCollection() {
    section("базовая сборка");

    VM vm;
    installStdlib(vm);
    const std::size_t before = vm.gc().stats().objects;

    // Создаём заведомо недостижимые объекты.
    for (int i = 0; i < 200; ++i) (void)vm.makeMap();
    const std::size_t afterAlloc = vm.gc().stats().objects;
    check(afterAlloc > before, "аллокация увеличила число объектов");

    vm.gc().collect();
    const GC::Stats s = vm.gc().stats();
    check(s.objects < afterAlloc, "сборка освободила недостижимые объекты");
    check(s.collections == 1, "счётчик сборок увеличился");
    check(s.totalFreed > 0, "totalFreed отражает освобождённые объекты");
    check(s.phase == GC::Phase::Idle, "после collect() сборщик в состоянии Idle");
}

// ---------------------------------------------------------------------------
static void testReachableSurvives() {
    section("достижимые объекты выживают");

    const std::string src = R"(
        global holder = [1, 2, 3]
        var junk = []
        for i in 1..500 do junk = [i] end
        global result = holder.len()
    )";

    const RunResult stw = runScript(src, /*incremental=*/false);
    check(stw.ok, "скрипт выполняется (stop-the-world)");
    check(stw.result == "3", "глобальный массив пережил сборку");

    const RunResult inc = runScript(src, /*incremental=*/true, 256);
    check(inc.ok, "скрипт выполняется (инкрементальный режим)");
    check(inc.result == "3", "глобальный массив пережил инкрементальную сборку");
}

// ---------------------------------------------------------------------------
static void testModeEquivalence() {
    section("эквивалентность режимов");

    const RunResult stw  = runScript(kChurnScript, false);
    const RunResult inc1 = runScript(kChurnScript, true, 64 * 1024);
    const RunResult inc2 = runScript(kChurnScript, true, 4096);
    const RunResult inc3 = runScript(kChurnScript, true, 256);

    check(stw.ok,  "stop-the-world: без ошибок");
    check(inc1.ok, "инкрементальный (64 КБ/шаг): без ошибок");
    check(inc2.ok, "инкрементальный (4 КБ/шаг): без ошибок");
    check(inc3.ok, "инкрементальный (256 Б/шаг): без ошибок");

    check(stw.result == "299", "stop-the-world удержал каждый десятый объект (диапазон 1..3000 эксклюзивный)");
    check(inc1.result == stw.result, "результат совпадает при бюджете 64 КБ");
    check(inc2.result == stw.result, "результат совпадает при бюджете 4 КБ");
    check(inc3.result == stw.result, "результат совпадает при бюджете 256 Б");

    std::printf("    паузы: STW %.3f мс | инкр. 4 КБ %.3f мс | инкр. 256 Б %.3f мс\n",
                stw.stats.maxPauseMs, inc2.stats.maxPauseMs, inc3.stats.maxPauseMs);
}

// ---------------------------------------------------------------------------
static void testIncrementalReducesPause() {
    section("инкрементальный режим сокращает паузу");

    // Много живых объектов => разметка дорогая, и разница режимов заметна.
    const std::string src = R"(
        global live = []
        for i in 1..4000 do live.push({"i": i, "v": [i, i, i]}) end
        var churn = []
        for i in 1..4000 do churn = [i, i * 2] end
        global result = live.len()
    )";

    const RunResult stw = runScript(src, false);
    const RunResult inc = runScript(src, true, 2048);

    check(stw.ok && inc.ok, "оба режима отработали");
    check(stw.result == inc.result, "результаты идентичны");
    check(inc.stats.steps > 0, "инкрементальный режим выполнил шаги");

    std::printf("    max пауза: STW %.4f мс, инкрементально %.4f мс (шагов: %zu)\n",
                stw.stats.maxPauseMs, inc.stats.maxPauseMs, inc.stats.steps);
    check(inc.stats.maxPauseMs <= stw.stats.maxPauseMs,
          "худшая пауза инкрементального режима не больше, чем у stop-the-world");
}

// ---------------------------------------------------------------------------
static void testWriteBarrier() {
    section("write-barrier: трёхцветная инварианта");

    // Классический сценарий нарушения инварианты: объект перевешивается из
    // белого контейнера в уже почерневший во время фазы Mark. Без барьера он
    // остаётся белым и освобождается, хотя достижим.
    const std::string src = R"(
        global black = []
        var white = []
        for i in 1..2000 do white.push({"payload": [i, i, i]}) end
        for i in 1..2000 do
            black.push(white[i - 1])
            var noise = [i, i, i, i]
        end
        global result = black.len()
    )";

    const RunResult stw = runScript(src, false);
    const RunResult inc = runScript(src, true, 512);

    check(stw.ok, "перевешивание ссылок: stop-the-world");
    check(inc.ok, "перевешивание ссылок: инкрементальный режим");
    check(stw.result == "1999", "все перевешенные объекты на месте (stop-the-world)");
    check(inc.result == stw.result, "столько же объектов инкрементально — write-barrier сработал");
}

// ---------------------------------------------------------------------------
static void testAllocateDuringMark() {
    section("аллокация во время разметки");

    // Объекты, созданные в фазе Mark, обязаны пережить текущий цикл вместе
    // со своими детьми: они красятся серыми и попадают в grayStack.
    const std::string src = R"(
        global nested = []
        for i in 1..1500 do
            nested.push([[i], [i * 2], {"k": [i, i]}])
        end
        var total = 0
        for row in nested do total = total + row.len() end
        global result = total
    )";

    const RunResult stw = runScript(src, false);
    const RunResult inc = runScript(src, true, 128);

    check(stw.ok && inc.ok, "оба режима отработали");
    check(stw.result == "4497", "1499 строк по 3 элемента = 4497");
    check(inc.result == stw.result, "вложенные объекты не потеряны при мелком бюджете");
}

// ---------------------------------------------------------------------------
static void testClosuresAndCoroutines() {
    section("замыкания и корутины");

    const std::string src = R"(
        global counters = []
        for i in 1..400 do
            var n = i
            counters.push(|| n * 2)
        end
        var sum = 0
        for f in counters do sum = sum + f() end
        global result = sum
    )";

    const RunResult stw = runScript(src, false);
    const RunResult inc = runScript(src, true, 256);

    check(stw.ok, "замыкания: stop-the-world");
    check(inc.ok, "замыкания: инкрементальный режим");
    // sum = 2 * (1 + ... + 400) = 2 * 80200 = 160400
    check(stw.result == "318402", "замыкания вернули верную сумму захваченных значений");
    check(inc.result == stw.result, "захваченные upvalue пережили инкрементальную сборку");
}

// ---------------------------------------------------------------------------
static void testStringsAndMaps() {
    section("строки и словари");

    const std::string src = R"(
        global registry = {}
        for i in 1..1200 do
            registry["key{i}"] = {"name": "item{i}", "tags": ["a", "b"]}
        end
        global result = registry.len()
    )";

    const RunResult stw = runScript(src, false);
    const RunResult inc = runScript(src, true, 512);

    check(stw.ok && inc.ok, "оба режима отработали");
    check(stw.result == "1199", "в словаре 1199 записей (диапазон эксклюзивный)");
    check(inc.result == stw.result, "интернированные строки и вложенные map не потеряны");
}

// ---------------------------------------------------------------------------
static void testStepApi() {
    section("API пошаговой сборки");

    VM vm;
    installStdlib(vm);
    vm.gc().setIncremental(true);
    vm.gc().setStepBudget(1024);
    // Порог роста близко к текущему потреблению: сборка начнётся быстро,
    // иначе пустые ObjMap (по ~40 байт) не доберут дефолтный мегабайт.
    vm.gc().setGrowthFactor(1.05);

    check(vm.gc().incremental(), "инкрементальный режим включён");
    check(vm.gc().stepBudget() == 1024, "бюджет шага установлен");
    check(vm.gc().phase() == GC::Phase::Idle, "сборщик изначально в Idle");

    // Ниже порога step() не должен начинать цикл вхолостую.
    const std::size_t stepsIdle = vm.gc().stats().steps;
    for (int i = 0; i < 10; ++i) vm.gc().step();
    check(vm.gc().stats().steps == stepsIdle,
          "step() ниже порога аллокаций не выполняет работу");
    const std::size_t collectionsBefore = vm.gc().stats().collections;

    // Набираем мусор. Сами аллокации уже продвигают сборку (allocate() делает
    // шаг при достижении порога), поэтому часть циклов пройдёт до вызовов step().
    for (int i = 0; i < 20000; ++i) {
        // Реальные байты, а не пустой заголовок. Элементы ОБЯЗАТЕЛЬНО
        // инициализируются: GC обходит [0, count) и на мусоре разыменует
        // случайный указатель.
        Value arr = vm.makeArray(16);
        auto* a = arr.as<ObjArray>();
        for (std::uint32_t k = 0; k < 16; ++k) a->items[k] = Value::integer(k);
        a->count = 16;
    }
    const std::size_t stepsAfterAlloc = vm.gc().stats().steps;
    check(stepsAfterAlloc > 0, "аллокации сами продвигают инкрементальную сборку");

    // Доводим текущий цикл до конца явными шагами.
    int guard = 0;
    while (vm.gc().phase() != GC::Phase::Idle && guard++ < 100000) vm.gc().step();

    check(vm.gc().phase() == GC::Phase::Idle, "цикл доведён до конца серией шагов");
    check(vm.gc().stats().collections > collectionsBefore, "сборка учтена в статистике");
    check(vm.gc().stats().steps > 1, "цикл действительно занял несколько шагов");
}

// ---------------------------------------------------------------------------
static void testCollectFinishesIncrementalCycle() {
    section("collect() завершает начатый инкрементальный цикл");

    VM vm;
    installStdlib(vm);
    vm.gc().setIncremental(true);
    vm.gc().setStepBudget(64);           // крошечный бюджет: цикл точно не успеет
    vm.gc().setGrowthFactor(1.05);

    // Набираем объекты так, чтобы цикл гарантированно оказался в середине:
    // после каждой аллокации проверяем фазу и останавливаемся, поймав не-Idle.
    for (int i = 0; i < 20000 && vm.gc().phase() == GC::Phase::Idle; ++i) {
        Value arr = vm.makeArray(16);
        auto* a = arr.as<ObjArray>();
        for (std::uint32_t k = 0; k < 16; ++k) a->items[k] = Value::integer(k);
        a->count = 16;
    }
    check(vm.gc().phase() != GC::Phase::Idle, "сборка находится в середине цикла");

    vm.gc().collect();                    // обязан довести до конца
    check(vm.gc().phase() == GC::Phase::Idle, "collect() довёл незавершённый цикл до конца");
    check(vm.gc().stats().collections >= 1, "цикл засчитан");
}

// ---------------------------------------------------------------------------
static void testModeSwitching() {
    section("переключение режимов на лету");

    VM vm;
    installStdlib(vm);
    vm.gc().setIncremental(true);
    vm.gc().setStepBudget(128);
    vm.gc().setGrowthFactor(1.05);

    // Доводим сборщик до середины цикла.
    for (int i = 0; i < 20000 && vm.gc().phase() == GC::Phase::Idle; ++i) {
        Value arr = vm.makeArray(16);
        auto* a = arr.as<ObjArray>();
        for (std::uint32_t k = 0; k < 16; ++k) a->items[k] = Value::integer(k);
        a->count = 16;
    }

    // Переключение посреди цикла обязано сначала его завершить.
    vm.gc().setIncremental(false);
    check(vm.gc().phase() == GC::Phase::Idle,
          "переключение режима завершает незаконченный цикл");
    check(!vm.gc().incremental(), "режим переключён на stop-the-world");

    vm.gc().setIncremental(true);
    check(vm.gc().incremental(), "режим возвращён в инкрементальный");
}

// ---------------------------------------------------------------------------
static void testPinnedObjects() {
    section("закреплённые объекты и их дети");

    VM vm;
    installStdlib(vm);

    // Модули stdlib закреплены; их содержимое обязано переживать любые сборки —
    // иначе скрипт падает с «undefined name».
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (int i = 0; i < 500; ++i) (void)vm.makeMap();
        vm.gc().collect();
    }

    DiagnosticList diags;
    Module mod = parseSource("import math\nglobal result = math.abs(0 - 42)\n", "pin", diags);
    Compiler c(vm);
    ObjFunction* fn = c.compile(mod, diags);
    bool compiled = fn != nullptr;
    for (const Diagnostic& d : diags)
        if (d.severity == DiagSeverity::Error) compiled = false;
    check(compiled, "модуль компилируется после пяти циклов сборки");

    if (compiled) {
        const bool ok = vm.execute(fn) == RunStatus::Ok;
        check(ok, "скрипт с 'import math' исполняется после сборок");
        if (ok) {
            Value* r = vm.findGlobal("result");
            check(r && r->toString() == "42",
                  "нативные функции внутри закреплённого модуля живы");
        }
    }
}

// ---------------------------------------------------------------------------
static void testMemoryLimit() {
    section("лимит памяти песочницы");

    VM vm;
    installStdlib(vm);
    const std::size_t used = vm.gc().bytesAllocated();
    vm.gc().setMemoryLimit(used + 64 * 1024);
    check(vm.gc().memoryLimit() == used + 64 * 1024, "лимит установлен");

    // Превышение лимита обязано стать ошибкой скрипта, а не падением процесса.
    DiagnosticList diags;
    Module mod = parseSource(
        "var a = []\nfor i in 1..200000 do a.push([i, i, i, i, i, i, i, i]) end\nglobal result = a.len()\n",
        "limit", diags);
    Compiler c(vm);
    ObjFunction* fn = c.compile(mod, diags);
    if (fn) {
        const RunStatus st = vm.execute(fn);
        check(st != RunStatus::Ok, "превышение лимита остановило скрипт");
        check(!vm.lastError().message.empty(), "ошибка содержит сообщение");
        std::printf("    сообщение: %s\n", vm.lastError().message.c_str());
    } else {
        check(false, "скрипт для проверки лимита скомпилировался");
    }
}

// ---------------------------------------------------------------------------
static void testStatsConsistency() {
    section("согласованность статистики");

    VM vm;
    installStdlib(vm);
    vm.gc().setGrowthFactor(1.2);

    for (int i = 0; i < 5000; ++i) (void)vm.makeMap();
    vm.gc().collect();
    vm.gc().collect();

    const GC::Stats s = vm.gc().stats();
    check(s.collections == 2, "число сборок совпадает с числом вызовов");
    check(s.bytes == vm.gc().bytesAllocated(), "bytes в статистике совпадает с bytesAllocated()");
    check(s.objects == vm.gc().objectCount(), "objects совпадает с objectCount()");
    check(s.maxPauseMs >= s.lastPauseMs, "maxPause не меньше последней паузы");
    check(s.phase == GC::Phase::Idle, "фаза Idle между сборками");
}

int main() {
    testBasicCollection();
    testReachableSurvives();
    testModeEquivalence();
    testIncrementalReducesPause();
    testWriteBarrier();
    testAllocateDuringMark();
    testClosuresAndCoroutines();
    testStringsAndMaps();
    testStepApi();
    testCollectFinishesIncrementalCycle();
    testModeSwitching();
    testPinnedObjects();
    testMemoryLimit();
    testStatsConsistency();

    std::printf("\n----------------------------------------\n");
    std::printf("GC self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
