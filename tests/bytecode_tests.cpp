/**
 * @file    bytecode_tests.cpp
 * @brief   Тесты (де)сериализации байткода LV Script (.lvc).
 *
 * Проверяется round-trip: исходник -> компиляция -> сериализация -> загрузка ->
 * исполнение даёт тот же результат, что и прямое исполнение. Также проверяются
 * защита от повреждённых файлов, несовпадение версии и хэш исходника.
 */
#include "lvscript/Bytecode.h"
#include "lvscript/BytecodeCache.h"
#include "lvscript/Compiler.h"
#include "lvscript/Parser.h"
#include "lvscript/VM.h"
#include "lvscript/Stdlib.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

/// Скомпилировать исходник в прототип; nullptr при ошибке.
ObjFunction* compileSource(VM& vm, const std::string& src, const std::string& name = "test.lvs") {
    DiagnosticList diags;
    Module m = parseSource(src, name, diags);
    for (const Diagnostic& d : diags)
        if (d.severity == DiagSeverity::Error) { std::printf("    ! %s\n", d.toString().c_str()); return nullptr; }
    Compiler c(vm);
    ObjFunction* fn = c.compile(m, diags);
    for (const Diagnostic& d : diags)
        if (d.severity == DiagSeverity::Error) { std::printf("    ! %s\n", d.toString().c_str()); return nullptr; }
    return fn;
}

/// Исполнить прототип и вернуть значение глобала `result` как строку.
std::string runAndGet(VM& vm, ObjFunction* fn, const char* global = "result") {
    if (!fn) return "<compile failed>";
    if (vm.execute(fn) != RunStatus::Ok) return "<runtime error: " + vm.lastError().toString() + ">";
    Value* v = vm.findGlobal(global);
    return v ? v->toString() : "<no global>";
}

} // namespace

// ---------------------------------------------------------------------------
static void testRoundTripSimple() {
    section("round-trip: арифметика и строки");

    const std::string src = R"(
        global result = 0
        let a = 21
        let b = 2
        result = a * b
    )";

    VM vm1;
    installStdlib(vm1);
    ObjFunction* fn = compileSource(vm1, src);
    check(fn != nullptr, "исходник компилируется");
    if (!fn) return;

    const std::string direct = runAndGet(vm1, fn);
    check(direct == "42", "прямое исполнение даёт 42");

    BytecodeModule mod;
    mod.entry = fn;
    mod.name = "test.lvs";
    mod.sourceHash = BytecodeModule::sourceHashOf(src);
    const std::vector<Byte> blob = mod.serialize(vm1);
    check(!blob.empty(), "сериализация вернула непустой буфер");
    check(blob.size() > 16, "буфер содержит заголовок и данные");
    check(blob[0] == 'L' && blob[1] == 'V' && blob[2] == 'C' && blob[3] == '1',
          "магическое число 'LVC1' на месте");

    VM vm2;
    installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "десериализация успешна");
    if (!loaded) return;

    check(loaded->sourceHash == mod.sourceHash, "хэш исходника сохранён");
    check(loaded->name == "test.lvs", "имя модуля сохранено");
    check(loaded->entry != nullptr, "entry-прототип восстановлен");

    const std::string fromCache = runAndGet(vm2, loaded->entry);
    check(fromCache == "42", "исполнение из .lvc даёт тот же результат (42)");
}

// ---------------------------------------------------------------------------
static void testRoundTripStrings() {
    section("round-trip: строковые константы");

    const std::string src = R"(
        global result = ""
        let name = "LimVine"
        let version = "1.0"
        result = name + " " + version
    )";

    VM vm1; installStdlib(vm1);
    ObjFunction* fn = compileSource(vm1, src);
    check(fn != nullptr, "компиляция со строками");
    if (!fn) return;
    check(runAndGet(vm1, fn) == "LimVine 1.0", "прямое исполнение конкатенирует строки");

    BytecodeModule mod; mod.entry = fn; mod.name = "str.lvs";
    const std::vector<Byte> blob = mod.serialize(vm1);

    VM vm2; installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "строковый модуль десериализуется");
    if (!loaded) return;
    check(runAndGet(vm2, loaded->entry) == "LimVine 1.0",
          "строки корректно восстановлены из таблицы интернирования");
}

// ---------------------------------------------------------------------------
static void testRoundTripFunctions() {
    section("round-trip: функции и вложенные прототипы");

    const std::string src = R"(
        global result = 0
        func square(x) do
            return x * x
        end
        func sumOfSquares(a, b) do
            return square(a) + square(b)
        end
        result = sumOfSquares(3, 4)
    )";

    VM vm1; installStdlib(vm1);
    ObjFunction* fn = compileSource(vm1, src);
    check(fn != nullptr, "компиляция с функциями");
    if (!fn) return;
    check(runAndGet(vm1, fn) == "25", "прямое исполнение: 9 + 16 = 25");

    BytecodeModule mod; mod.entry = fn; mod.name = "fn.lvs";
    const std::vector<Byte> blob = mod.serialize(vm1);

    VM vm2; installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "модуль с функциями десериализуется");
    if (!loaded) return;
    check(loaded->protos.size() >= 3, "сохранены entry + два вложенных прототипа");
    check(runAndGet(vm2, loaded->entry) == "25", "функции работают после загрузки из .lvc");
}

// ---------------------------------------------------------------------------
static void testRoundTripControlFlow() {
    section("round-trip: циклы и ветвления");

    const std::string src = R"(
        global result = 0
        var total = 0
        for i in 1..=10 do
            if i % 2 == 0 do
                total = total + i
            end
        end
        result = total
    )";

    VM vm1; installStdlib(vm1);
    ObjFunction* fn = compileSource(vm1, src);
    check(fn != nullptr, "компиляция с циклом и условием");
    if (!fn) return;
    check(runAndGet(vm1, fn) == "30", "прямое исполнение: 2+4+6+8+10 = 30");

    BytecodeModule mod; mod.entry = fn; mod.name = "flow.lvs";
    const std::vector<Byte> blob = mod.serialize(vm1);

    VM vm2; installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "модуль с потоком управления десериализуется");
    if (!loaded) return;
    check(runAndGet(vm2, loaded->entry) == "30", "переходы и смещения сохранены корректно");
}

// ---------------------------------------------------------------------------
static void testBytecodeIdentity() {
    section("идентичность байткода до и после");

    const std::string src = R"(
        func fib(n) do
            if n < 2 do return n end
            return fib(n - 1) + fib(n - 2)
        end
        global result = fib(10)
    )";

    VM vm1; installStdlib(vm1);
    ObjFunction* fn = compileSource(vm1, src);
    check(fn != nullptr, "компиляция рекурсивной функции");
    if (!fn) return;

    const std::vector<Byte> originalCode = fn->code;
    const std::size_t originalConsts = fn->constants.size();

    BytecodeModule mod; mod.entry = fn; mod.name = "fib.lvs";
    const std::vector<Byte> blob = mod.serialize(vm1);

    VM vm2; installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "рекурсивный модуль десериализуется");
    if (!loaded) return;

    check(loaded->entry->code == originalCode, "байткод entry побайтово идентичен");
    check(loaded->entry->constants.size() == originalConsts, "число констант совпадает");
    check(loaded->entry->numLocals == fn->numLocals, "numLocals сохранён");
    check(loaded->entry->maxStack == fn->maxStack, "maxStack сохранён");
    check(runAndGet(vm2, loaded->entry) == "55", "fib(10) == 55 после загрузки из кэша");
}

// ---------------------------------------------------------------------------
static void testDebugInfoPreserved() {
    section("отладочная информация");

    const std::string src = "global result = 1\nglobal other = 2\nresult = result + other\n";
    VM vm1; installStdlib(vm1);
    ObjFunction* fn = compileSource(vm1, src, "debug.lvs");
    check(fn != nullptr, "компиляция");
    if (!fn) return;
    check(!fn->lineStarts.empty(), "таблица строк заполнена компилятором");

    BytecodeModule mod; mod.entry = fn; mod.name = "debug.lvs";
    const std::vector<Byte> blob = mod.serialize(vm1);

    VM vm2; installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "десериализация");
    if (!loaded) return;
    check(loaded->entry->lineStarts == fn->lineStarts, "таблица номеров строк сохранена полностью");
    check(loaded->entry->source == "debug.lvs", "имя исходного файла восстановлено");
}

// ---------------------------------------------------------------------------
static void testCorruptedData() {
    section("защита от повреждённых данных");

    VM vm; installStdlib(vm);

    const std::vector<Byte> empty;
    check(!BytecodeModule::deserialize(vm, empty).has_value(), "пустой буфер отвергается");

    const std::vector<Byte> tooShort{'L', 'V', 'C'};
    check(!BytecodeModule::deserialize(vm, tooShort).has_value(), "слишком короткий буфер отвергается");

    std::vector<Byte> badMagic(64, 0);
    badMagic[0] = 'X'; badMagic[1] = 'X'; badMagic[2] = 'X'; badMagic[3] = 'X';
    check(!BytecodeModule::deserialize(vm, badMagic).has_value(), "неверное магическое число отвергается");

    // Правильная магия, но неверная версия.
    std::vector<Byte> badVersion(64, 0);
    badVersion[0] = 'L'; badVersion[1] = 'V'; badVersion[2] = 'C'; badVersion[3] = '1';
    const std::uint32_t wrongVer = 0xDEADBEEF;
    std::memcpy(badVersion.data() + 4, &wrongVer, 4);
    check(!BytecodeModule::deserialize(vm, badVersion).has_value(),
          "несовпадение версии формата отвергается (файл будет перекомпилирован)");
}

// ---------------------------------------------------------------------------
static void testSourceHash() {
    section("хэш исходника (инвалидация кэша)");

    const std::string src1 = "global result = 1";
    const std::string src2 = "global result = 2";

    const std::uint64_t h1 = BytecodeModule::sourceHashOf(src1);
    const std::uint64_t h2 = BytecodeModule::sourceHashOf(src2);
    const std::uint64_t h1again = BytecodeModule::sourceHashOf(src1);

    check(h1 != 0, "хэш непустого исходника не равен нулю");
    check(h1 == h1again, "хэш детерминирован");
    check(h1 != h2, "разные исходники дают разные хэши");

    // Сценарий кэша: сохранили хэш, исходник изменился -> кэш невалиден.
    VM vm; installStdlib(vm);
    ObjFunction* fn = compileSource(vm, src1);
    if (!fn) { check(false, "компиляция"); return; }
    BytecodeModule mod; mod.entry = fn; mod.name = "hash.lvs";
    mod.sourceHash = h1;
    const std::vector<Byte> blob = mod.serialize(vm);

    VM vm2; installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "кэш загружается");
    if (!loaded) return;
    check(loaded->sourceHash != BytecodeModule::sourceHashOf(src2),
          "хэш из кэша не совпадает с хэшем изменённого исходника => перекомпиляция");
    check(loaded->sourceHash == BytecodeModule::sourceHashOf(src1),
          "хэш совпадает с исходным текстом => кэш валиден");
}

// ---------------------------------------------------------------------------
static void testClassesRuntimeReconstruction() {
    section("классы восстанавливаются исполнением верхнего уровня");

    // Классы — рантайм-объекты: в .lvc сохраняется байткод их определения,
    // а сами ObjClass создаются при исполнении модуля.
    const std::string src = R"(
        class Counter do
            var value = 0
            func init(start) do
                this.value = start
            end
            func bump() do
                this.value = this.value + 1
                return this.value
            end
        end
        global result = 0
        let c = new Counter(10)
        c.bump()
        result = c.bump()
    )";

    VM vm1; installStdlib(vm1);
    ObjFunction* fn = compileSource(vm1, src);
    check(fn != nullptr, "компиляция класса");
    if (!fn) return;
    check(runAndGet(vm1, fn) == "12", "прямое исполнение: 10 -> 11 -> 12");

    BytecodeModule mod; mod.entry = fn; mod.name = "cls.lvs";
    const std::vector<Byte> blob = mod.serialize(vm1);

    VM vm2; installStdlib(vm2);
    auto loaded = BytecodeModule::deserialize(vm2, blob);
    check(loaded.has_value(), "модуль с классом десериализуется");
    if (!loaded) return;
    check(runAndGet(vm2, loaded->entry) == "12",
          "класс пересоздаётся при исполнении и работает идентично");
}

// ---------------------------------------------------------------------------
static void testCompactness() {
    section("компактность формата");

    const std::string src = R"(
        global result = 0
        var sum = 0
        for i in 1..100 do
            sum = sum + i * 2
        end
        result = sum
    )";

    VM vm; installStdlib(vm);
    ObjFunction* fn = compileSource(vm, src);
    check(fn != nullptr, "компиляция");
    if (!fn) return;

    BytecodeModule mod; mod.entry = fn; mod.name = "compact.lvs";
    const std::vector<Byte> blob = mod.serialize(vm);

    std::printf("    исходник: %zu байт, .lvc: %zu байт (%.0f%%)\n",
                src.size(), blob.size(), 100.0 * static_cast<double>(blob.size()) / static_cast<double>(src.size()));
    check(blob.size() < src.size() * 8, "размер .lvc в разумных пределах относительно исходника");
    check(blob.size() >= fn->code.size(), ".lvc не меньше самого байткода");
}

// ---------------------------------------------------------------------------
static void testDiskCache() {
    section("дисковый кэш (.lvc)");

    namespace fs = std::filesystem;
    const fs::path tmpRoot = fs::temp_directory_path() / "limvine_cache_test";
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot / "scripts", ec);

    const fs::path srcPath = tmpRoot / "scripts" / "logic.lvs";
    const std::string src = "global result = 0\nfunc triple(x) do return x * 3 end\nresult = triple(14)\n";
    { std::ofstream out(srcPath); out << src; }

    BytecodeCache cache(tmpRoot / ".lvcache");
    check(cache.enabled(), "кэш включён по умолчанию");
    check(cache.pathFor(srcPath).extension() == ".lvc", "путь кэша имеет расширение .lvc");

    // Первый заход: кэша нет -> промах.
    {
        VM vm; installStdlib(vm);
        auto miss = cache.load(vm, srcPath, src);
        check(!miss.has_value(), "первая загрузка — промах кэша");
        check(cache.stats().misses == 1, "счётчик промахов увеличился");

        ObjFunction* fn = compileSource(vm, src, srcPath.string());
        check(fn != nullptr, "исходник компилируется");
        if (!fn) return;

        BytecodeModule mod; mod.entry = fn; mod.name = srcPath.string();
        check(cache.store(vm, srcPath, src, mod), "модуль записан в кэш");
        check(fs::exists(cache.pathFor(srcPath)), ".lvc появился на диске");
        check(cache.stats().writes == 1, "счётчик записей увеличился");
    }

    // Второй заход: кэш валиден -> попадание, и код исполняется.
    {
        VM vm; installStdlib(vm);
        auto hit = cache.load(vm, srcPath, src);
        check(hit.has_value(), "вторая загрузка — попадание в кэш");
        check(cache.stats().hits == 1, "счётчик попаданий увеличился");
        if (hit) check(runAndGet(vm, hit->entry) == "42", "код из кэша исполняется (14*3 = 42)");
    }

    // Исходник изменился -> кэш устарел.
    {
        VM vm; installStdlib(vm);
        const std::string changed = "global result = 0\nfunc triple(x) do return x * 3 end\nresult = triple(15)\n";
        auto stale = cache.load(vm, srcPath, changed);
        check(!stale.has_value(), "изменение исходника инвалидирует кэш");
        check(cache.stats().stale == 1, "счётчик устаревших увеличился");
    }

    // Повреждённый файл кэша не должен ломать загрузку.
    {
        const fs::path cachePath = cache.pathFor(srcPath);
        { std::ofstream out(cachePath, std::ios::binary); out << "garbage not a real lvc file"; }
        VM vm; installStdlib(vm);
        auto broken = cache.load(vm, srcPath, src);
        check(!broken.has_value(), "повреждённый .lvc отвергается без падения");
    }

    // Отключённый кэш всегда промахивается и ничего не пишет.
    {
        VM vm; installStdlib(vm);
        BytecodeCache off(tmpRoot / ".lvcache");
        off.setEnabled(false);
        check(!off.enabled(), "кэш можно отключить");
        check(!off.load(vm, srcPath, src).has_value(), "отключённый кэш не читает");
        ObjFunction* fn = compileSource(vm, src, srcPath.string());
        BytecodeModule mod; mod.entry = fn; mod.name = srcPath.string();
        check(!off.store(vm, srcPath, src, mod), "отключённый кэш не пишет");
    }

    // Очистка.
    {
        VM vm; installStdlib(vm);
        ObjFunction* fn = compileSource(vm, src, srcPath.string());
        BytecodeModule mod; mod.entry = fn; mod.name = srcPath.string();
        cache.store(vm, srcPath, src, mod);
        const std::size_t removed = cache.clear();
        check(removed >= 1, "clear() удалил файлы кэша");
        check(!fs::exists(cache.pathFor(srcPath)), ".lvc удалён с диска");
    }

    // Статистика.
    {
        BytecodeCache fresh(tmpRoot / ".lvcache2");
        check(fresh.stats().hits == 0 && fresh.stats().misses == 0, "новый кэш имеет нулевую статистику");
        VM vm; installStdlib(vm);
        (void)fresh.load(vm, srcPath, src);
        check(fresh.stats().hitRate() == 0.0, "hitRate == 0 при единственном промахе");
    }

    // Путь кэша не должен выходить за свой каталог даже для абсолютных путей.
    {
        BytecodeCache c2(tmpRoot / ".lvcache3");
        const fs::path escaped = c2.pathFor("/etc/passwd");
        const std::string s = escaped.lexically_normal().string();
        check(s.find((tmpRoot / ".lvcache3").string()) == 0,
              "абсолютный путь исходника остаётся внутри каталога кэша");
    }

    fs::remove_all(tmpRoot, ec);
}

int main() {
    testRoundTripSimple();
    testRoundTripStrings();
    testRoundTripFunctions();
    testRoundTripControlFlow();
    testBytecodeIdentity();
    testDebugInfoPreserved();
    testCorruptedData();
    testSourceHash();
    testClassesRuntimeReconstruction();
    testCompactness();
    testDiskCache();

    std::printf("\n----------------------------------------\n");
    std::printf("Bytecode self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
