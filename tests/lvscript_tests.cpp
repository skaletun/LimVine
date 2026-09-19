/**
 * @file    lvscript_tests.cpp
 * @brief   Драйвер проверки LV Script: лексер -> парсер -> компилятор -> VM.
 *
 * Запуск: `lvscript_tests` — печатает PASS/FAIL по каждому кейсу и возвращает
 * ненулевой код при любом провале (используется в CI движка).
 */
#include "lvscript/Parser.h"
#include "lvscript/Compiler.h"
#include "lvscript/VM.h"
#include "lvscript/Scheduler.h"
#include "lvscript/Stdlib.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;
std::vector<std::string> g_output;

struct Harness {
    lv::VM vm;
    lv::Scheduler sched;

    Harness() : vm(), sched(vm) {
        lv::installStdlib(vm);
        vm.setLogSink([](std::string s) { g_output.push_back(std::move(s)); });
        vm.setErrorHandler([](const lv::ScriptError& e, std::string_view tb) {
            std::fprintf(stderr, "[lv] %s\n%.*s", e.toString().c_str(),
                         static_cast<int>(tb.size()), tb.data());
        });
    }

    /// Скомпилировать и выполнить. Возвращает false при ошибке компиляции/рантайма.
    bool run(const std::string& src, const std::string& name = "test.lvs") {
        lv::DiagnosticList diags;
        lv::Module m = lv::parseSource(src, name, diags);
        bool hard = false;
        for (const auto& d : diags) {
            std::fprintf(stderr, "%s\n", d.toString().c_str());
            if (d.severity == lv::DiagSeverity::Error) hard = true;
        }
        if (hard) return false;
        lv::Compiler c(vm);
        lv::ObjFunction* fn = c.compile(m, diags);
        for (const auto& d : diags) {
            std::fprintf(stderr, "%s\n", d.toString().c_str());
            if (d.severity == lv::DiagSeverity::Error) hard = true;
        }
        if (hard || !fn) return false;
        return vm.execute(fn) == lv::RunStatus::Ok;
    }

    /// Прогнать планировщик корутин (не более maxSeconds виртуального времени).
    void pump(double maxSeconds, double dt = 0.25) {
        for (double t = 0; t < maxSeconds; t += dt) sched.tick(dt);
    }
};

void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}

void section(const char* name) { std::printf("\n== %s ==\n", name); }

std::string lastLine() { return g_output.empty() ? std::string() : g_output.back(); }
bool outputContains(const std::string& s) {
    for (const auto& l : g_output) if (l.find(s) != std::string::npos) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
int main() {
    // -- 1. Арифметика, строки, интерполяция --------------------------------
    section("expressions");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            let a = 2 + 3 * 4
            print(a)
            print(2 ** 10)
            print(7 % 3, 7 / 2)
            let name = "LimVine"
            print("hello {name}, 1+1={1 + 1}")
            print([1, 2, 3], {"hp": 100})
            print(not (1 < 2), 3 >= 3, "a" == "a")
        )LV"), "compiles and runs");
        check(g_output.size() >= 6, "produced output");
        check(g_output[0] == "14", "operator precedence (2+3*4 == 14)");
        check(g_output[1] == "1024", "power operator");
        check(g_output[3] == "hello LimVine, 1+1=2", "string interpolation");
        check(g_output[4] == "[1, 2, 3]\t{\"hp\": 100}", "array and map literals");
        check(g_output[5] == "false\ttrue\ttrue", "comparison and logic");
    }

    // -- 2. Управление потоком ----------------------------------------------
    section("control flow");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            var total = 0
            for i in 0..5 do total += i end
            print("range", total)

            var sum = 0
            for i in 0..=4 do sum += i end
            print("inclusive", sum)

            let items = ["a", "b", "c"]
            var joined = ""
            for it in items do joined += it end
            print("foreach", joined)

            for k, v in {"x": 1} do print("kv", k, v) end

            var n = 10
            while n > 0 do n -= 3 end
            print("while", n)

            let sign = if n < 0 do -1 elif n == 0 do 0 else 1 end
            print("ifexpr", sign)

            var acc = 0
            for i in 0..10 do
                if i == 5 do break end
                if i % 2 == 0 do continue end
                acc += i
            end
            print("breakcont", acc)
        )LV"), "runs");
        check(g_output[0] == "range\t10", "exclusive range sum 0..5 == 10");
        check(g_output[1] == "inclusive\t10", "inclusive range sum 0..=4 == 10");
        check(g_output[2] == "foreach\tabc", "for-in over array");
        check(g_output[4] == "while\t-2", "while loop");
        check(g_output[5] == "ifexpr\t-1", "if as an expression");
        check(g_output[6] == "breakcont\t4", "break/continue (1+3 == 4)");
    }

    // -- 3. Функции, замыкания, рекурсия ------------------------------------
    section("functions & closures");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            func fib(n: Int) -> Int do
                if n < 2 do return n end
                return fib(n - 1) + fib(n - 2)
            end
            print("fib20", fib(20))

            func makeCounter() do
                var count = 0
                return () => {
                    count += 1
                    return count
                }
            end
            let c = makeCounter()
            c(); c(); print("closure", c())

            func greet(name, greeting = "Hello") do return "{greeting}, {name}!" end
            print(greet("World"))
            print(greet("World", "Hi"))

            let sq = |x| x * x
            print("lambda", sq(9))
            print("hof", [1, 2, 3, 4].map(|x| x * x).filter(|x| x > 4))

            func sum(args...) do
                var t = 0
                for a in args do t += a end
                return t
            end
            print("variadic", sum(1, 2, 3, 4, 5))
        )LV"), "runs");
        check(g_output[0] == "fib20\t6765", "recursion fib(20)");
        check(g_output[1] == "closure\t3", "closure captures mutable local");
        check(g_output[2] == "Hello, World!", "default parameter");
        check(g_output[3] == "Hi, World!", "overridden default parameter");
        check(g_output[4] == "lambda\t81", "lambda expression");
        check(g_output[5] == "hof\t[9, 16]", "map/filter higher-order functions");
        check(g_output[6] == "variadic\t15", "variadic parameter");
    }

    // -- 4. Классы и наследование -------------------------------------------
    section("classes");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            class Entity do
                var name = "entity"
                var hp = 100

                func init(name: String, hp: Int) do
                    this.name = name
                    this.hp = hp
                end

                func describe() -> String do return "{this.name}(hp={this.hp})" end
            end

            class Player extends Entity do
                var level = 1

                func init(name: String, hp: Int, level: Int) do
                    super.init(name, hp)
                    this.level = level
                end

                func describe() -> String do return super.describe() + " lvl={this.level}" end
            end

            let e = new Entity("crate", 10)
            let p = new Player("hero", 100, 7)
            print(e.describe())
            print(p.describe())
            print(p is Entity, p is Player, e is Player)
            print(p.hp, p.level)
        )LV"), "runs");
        check(g_output[0] == "crate(hp=10)", "field defaults + init");
        check(g_output[1] == "hero(hp=100) lvl=7", "inheritance and super.describe()");
        check(g_output[2] == "true\ttrue\tfalse", "`is` operator walks the class chain");
        check(g_output[3] == "100\t7", "super.init assigned base fields");
    }

    // -- 5. match -----------------------------------------------------------
    section("match");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            func describe(v) do
                return match v
                    when 0 => "zero"
                    when n is Int if n < 0 => "negative {n}"
                    when s is String => "string {s}"
                    when [a, b] => "pair {a}/{b}"
                    when * => "other"
                end
            end
            print(describe(0))
            print(describe(-5))
            print(describe("hey"))
            print(describe(3.5))
            print(describe([7, 8]))
        )LV"), "runs");
        check(g_output[0] == "zero", "literal pattern");
        check(g_output[1] == "negative -5", "binding + type test + guard");
        check(g_output[2] == "string hey", "type pattern with binding");
        check(g_output[3] == "other", "wildcard fallback");
        check(g_output[4] == "pair 7/8", "array destructuring pattern");
    }

    // -- 6. Result / try ----------------------------------------------------
    section("errors");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            func safeDiv(a, b) do
                if b == 0 do return Err("division by zero") end
                return Ok(a / b)
            end

            let r = safeDiv(10, 2)
            print(r.ok(), r.unwrap())

            let bad = safeDiv(1, 0)
            print(bad.ok(), bad.error())

            let t = try (1 / 0)
            print(t.ok(), t.error())

            func chain(x) do
                let v = safeDiv(100, x)?      # propagation
                return Ok(v + 1)
            end
            print(chain(4).unwrap())
            print(chain(0).ok())
        )LV"), "runs");
        check(g_output[0] == "true\t5", "Ok result");
        check(g_output[1] == "false\tdivision by zero", "Err result");
        check(g_output[2] == "false\tdivision by zero", "`try` captures runtime error");
        check(g_output[3] == "26", "`?` propagation on success");
        check(g_output[4] == "false", "`?` propagation on failure");
    }

    // -- 7. Корутины и планировщик ------------------------------------------
    section("coroutines");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            coroutine grow() do
                print("planted")
                wait(1.0)
                print("sprouted")
                wait(2.0)
                print("harvest-ready")
            end

            # Объявление `coroutine` создаёт первичную корутину:
            # вызов grow() стартует её и возвращает управление на первом wait().
            grow()
            print("main continues")
        )LV"), "runs");
        check(g_output[0] == "planted", "coroutine starts immediately on resume");
        check(g_output[1] == "main continues", "resume returns to the caller");
        g_output.clear();
        h.pump(1.5);
        check(outputContains("sprouted"), "wait(1.0) resumes after 1s of game time");
        check(!outputContains("harvest-ready"), "wait(2.0) is not ready yet");
        h.pump(2.5);
        check(outputContains("harvest-ready"), "second wait completes");
    }

    // -- 9. Песочница: топливо и capability ---------------------------------
    section("sandbox");
    {
        lv::SandboxConfig cfg;
        cfg.capabilities = lv::Cap_Math;       // без Cap_Debug => print недоступен
        cfg.fuelPerCall = 5000;
        lv::VM vm(cfg);
        lv::installStdlib(vm);
        lv::Scheduler sched(vm);

        lv::DiagnosticList diags;
        lv::Module m = lv::parseSource("var i = 0\nwhile true do i += 1 end", "evil.lvs", diags);
        lv::Compiler c(vm);
        auto* fn = c.compile(m, diags);
        const auto st = vm.execute(fn);
        check(st == lv::RunStatus::OutOfFuel, "infinite loop is stopped by the fuel budget");

        lv::DiagnosticList d2;
        lv::Module m2 = lv::parseSource("print(\"secret\")", "mod.lvs", d2);
        lv::Compiler c2(vm);
        auto* fn2 = c2.compile(m2, d2);
        check(vm.execute(fn2) == lv::RunStatus::RuntimeError,
              "print() is rejected without the Cap_Debug capability");
    }

    // -- 10. GC под нагрузкой ------------------------------------------------
    section("gc");
    {
        Harness h;
        g_output.clear();
        check(h.run(R"LV(
            func churn() do
                var keep = []
                for i in 0..2000 do
                    let tmp = {"i": i, "s": "value-{i}"}
                    if i % 100 == 0 do keep.push(tmp) end
                end
                return keep.len()
            end
            print("kept", churn())
            print("kept", churn())
        )LV"), "runs under GC pressure");
        check(g_output[0] == "kept\t20", "20 survivors out of 2000 allocations");
        h.vm.collectGarbage();
        check(h.vm.gc().collections() > 0, "garbage collector ran and reclaimed garbage");
        check(h.vm.gc().bytesAllocated() < 8ull * 1024 * 1024, "heap stayed small (no leak)");
    }

    // -- 11. Дизассемблер ---------------------------------------------------
    section("disassembler");
    {
        Harness h;
        lv::DiagnosticList diags;
        lv::Module m = lv::parseSource("func add(a, b) do return a + b end\nprint(add(2, 3))",
                                       "dis.lvs", diags);
        lv::Compiler c(h.vm);
        auto* fn = c.compile(m, diags);
        const std::string dis = lv::Disassembler::disassemble(*fn, &h.vm.names(), true);
        check(dis.find("Add") != std::string::npos, "bytecode contains Add");
        check(dis.find("function add") != std::string::npos, "nested prototype is disassembled");
        std::printf("%s", dis.c_str());
    }

    std::printf("\n----------------------------------------\n");
    std::printf("LV Script self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
