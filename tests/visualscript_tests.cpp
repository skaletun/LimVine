/**
 * @file    visualscript_tests.cpp
 * @brief   Тесты Visual Scripting: граф -> LV Script -> выполнение -> round-trip.
 */
#include "scripting/visual/VisualScript.h"
#include "lvscript/Compiler.h"
#include "lvscript/Parser.h"
#include "lvscript/Scheduler.h"
#include "lvscript/Stdlib.h"
#include "lvscript/VM.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace lv;
using namespace lv::vscript;

namespace {
int g_passed = 0, g_failed = 0;
std::vector<std::string> g_log;
void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
void section(const char* s) { std::printf("\n== %s ==\n", s); }
bool logHas(const std::string& s) {
    for (const auto& l : g_log) if (l.find(s) != std::string::npos) return true;
    return false;
}

/// Выполнить сгенерированный код и вернуть признак успеха.
bool runCode(const std::string& code, VM& vm, Scheduler& sched, const std::string& name = "generated.lvs") {
    // Тест работает без движка, поэтому подставляем заглушки для natives,
    // которые в реальной игре регистрирует installEngineBindings().
    vm.registerNative("inputHeld", 1, [](VM&, std::span<const Value>) { return Value::boolean(false); });
    vm.registerNative("inputPressed", 1, [](VM&, std::span<const Value>) { return Value::boolean(false); });
    vm.registerNative("deltaTime", 0, [](VM&, std::span<const Value>) { return Value::fromNumber(0.016); });
    vm.setErrorHandler([](const ScriptError& e, std::string_view tb) {
        std::fprintf(stderr, "    [runtime] %s\n%.*s", e.toString().c_str(), static_cast<int>(tb.size()), tb.data());
    });
    DiagnosticList diags;
    Module m = parseSource(code, name, diags);
    for (const Diagnostic& d : diags) std::fprintf(stderr, "    %s\n", d.toString().c_str());
    for (const Diagnostic& d : diags) if (d.severity == DiagSeverity::Error) return false;
    Compiler c(vm);
    ObjFunction* fn = c.compile(m, diags);
    for (const Diagnostic& d : diags) std::fprintf(stderr, "    %s\n", d.toString().c_str());
    if (!fn) return false;
    if (vm.execute(fn) != RunStatus::Ok) return false;
    for (int i = 0; i < 20; ++i) sched.tick(0.1f);
    return true;
}
} // namespace

int main() {
    // ------------------------------------------------------------------
    section("graph -> LV Script");

    // Граф: event.update -> branch(inputHeld("Fire")) -> print("shot")
    //                                  \-> false -> print("idle")
    const std::string json = R"JSON({
      "name": "ShooterLogic",
      "nodes": [
        {"id": 1, "type": "event.update", "x": 40, "y": 80,
         "pins": [{"id": "then", "type": "exec", "dir": "out"}]},
        {"id": 2, "type": "pure.engine.inputHeld", "props": {"action": "Fire"},
         "pins": [{"id": "value", "type": "bool", "dir": "out"}]},
        {"id": 3, "type": "flow.branch", "x": 260, "y": 80,
         "pins": [{"id": "exec", "type": "exec", "dir": "in"},
                 {"id": "condition", "type": "bool", "dir": "in"},
                 {"id": "true", "type": "exec", "dir": "out"},
                 {"id": "false", "type": "exec", "dir": "out"}]},
        {"id": 4, "type": "pure.literal", "props": {},
         "pins": [{"id": "value", "type": "string", "dir": "out", "value": "shot!"}]},
        {"id": 5, "type": "action.print", "x": 520, "y": 20,
         "pins": [{"id": "exec", "type": "exec", "dir": "in"},
                 {"id": "value", "type": "any", "dir": "in"},
                 {"id": "then", "type": "exec", "dir": "out"}]},
        {"id": 6, "type": "pure.literal",
         "pins": [{"id": "value", "type": "string", "dir": "out", "value": "idle"}]},
        {"id": 7, "type": "action.print", "x": 520, "y": 180,
         "pins": [{"id": "exec", "type": "exec", "dir": "in"},
                 {"id": "value", "type": "any", "dir": "in"},
                 {"id": "then", "type": "exec", "dir": "out"}]}
      ],
      "connections": [
        {"from": 1, "fromPin": "then", "to": 3, "toPin": "exec"},
        {"from": 2, "fromPin": "value", "to": 3, "toPin": "condition"},
        {"from": 3, "fromPin": "true", "to": 5, "toPin": "exec"},
        {"from": 4, "fromPin": "value", "to": 5, "toPin": "value"},
        {"from": 3, "fromPin": "false", "to": 7, "toPin": "exec"},
        {"from": 6, "fromPin": "value", "to": 7, "toPin": "value"}
      ]
    })JSON";

    Graph graph;
    std::string err;
    check(parseGraphJson(json, graph, err), "graph JSON parses");
    check(err.empty(), "no JSON errors");
    check(graph.nodes.size() == 7, "7 nodes loaded");
    check(graph.connections.size() == 6, "6 connections loaded");

    Transpiler t;
    const TranspileResult res = t.transpile(graph);
    std::printf("\n----- generated LV Script -----\n%s-------------------------------\n", res.code.c_str());
    check(res.ok, "transpile succeeded without errors");
    check(res.warnings.empty(), "no transpiler warnings");
    check(res.code.find("func update(dt) do") != std::string::npos, "event.update became `func update(dt)`");
    check(res.code.find("if inputHeld(\"Fire\") do") != std::string::npos, "branch + input node became an if-statement");
    check(res.code.find("print(\"shot!\")") != std::string::npos, "true branch emitted");
    check(res.code.find("else do") != std::string::npos, "false branch emitted");
    check(res.code.find("print(\"idle\")") != std::string::npos, "else body emitted");
    check(res.code.find("#@node id=3 type=flow.branch") != std::string::npos, "round-trip metadata emitted");

    // Сгенерированный код обязан компилироваться штатным компилятором.
    {
        VM vm;
        installStdlib(vm);
        Scheduler sched(vm);
        g_log.clear();
        vm.setLogSink([](std::string s) { g_log.push_back(std::move(s)); });
        // inputHeld без подключённого ввода возвращает false => ветка else.
        // Сгенерированный файл объявляет `update(dt)`; вызывает его движок,
        // поэтому в тесте делаем явный вызов, как это делает ScriptWorld::tick.
        check(runCode(res.code + "\nupdate(0.016)\n", vm, sched), "generated code compiles and runs");
        check(logHas("idle"), "executed the false branch at runtime");
    }

    // ------------------------------------------------------------------
    section("round-trip: metadata back from code");
    {
        const Graph restored = Transpiler::parseMetadata(res.code);
        std::printf("    restored %zu nodes from metadata\n", restored.nodes.size());
        check(restored.nodes.size() >= 3, "executed nodes restored from #@node comments");
        bool foundBranch = false;
        for (const Node& n : restored.nodes)
            if (n.type == "flow.branch" && n.id == 3 && n.x == 260 && n.y == 80) foundBranch = true;
        check(foundBranch, "node type, id and canvas position survived the round-trip");
    }

    // ------------------------------------------------------------------
    section("serialization round-trip");
    {
        const std::string dumped = serializeGraphJson(graph);
        Graph g2;
        std::string err2;
        check(parseGraphJson(dumped, g2, err2), "serialized graph parses back");
        check(g2.nodes.size() == graph.nodes.size(), "node count preserved");
        check(g2.connections.size() == graph.connections.size(), "connection count preserved");
        check(g2.name == graph.name, "graph name preserved");
        const Node* n3 = g2.node(3);
        check(n3 && n3->type == "flow.branch" && n3->pins.size() == 4, "node pins preserved");
    }

    // ------------------------------------------------------------------
    section("arithmetic data-flow graph");
    {
        const std::string mathJson = R"JSON({
          "name": "Math",
          "nodes": [
            {"id": 1, "type": "pure.literal", "pins": [{"id":"value","type":"int","dir":"out","value":"6"}]},
            {"id": 2, "type": "pure.literal", "pins": [{"id":"value","type":"int","dir":"out","value":"7"}]},
            {"id": 3, "type": "pure.math.mul",
             "pins": [{"id":"a","type":"int","dir":"in"},{"id":"b","type":"int","dir":"in"},
                      {"id":"value","type":"int","dir":"out"}]},
            {"id": 4, "type": "pure.string.format", "props": {"format": "answer={}"},
             "pins": [{"id":"arg0","type":"any","dir":"in"},{"id":"value","type":"string","dir":"out"}]},
            {"id": 5, "type": "action.print",
             "pins": [{"id":"exec","type":"exec","dir":"in"},{"id":"value","type":"any","dir":"in"},
                      {"id":"then","type":"exec","dir":"out"}]}
          ],
          "connections": [
            {"from":1,"fromPin":"value","to":3,"toPin":"a"},
            {"from":2,"fromPin":"value","to":3,"toPin":"b"},
            {"from":3,"fromPin":"value","to":4,"toPin":"arg0"},
            {"from":4,"fromPin":"value","to":5,"toPin":"value"}
          ]
        })JSON";
        Graph g; std::string e2;
        check(parseGraphJson(mathJson, g, e2), "math graph parses");
        const TranspileResult r = t.transpile(g);
        std::printf("\n----- generated -----\n%s---------------------\n", r.code.c_str());
        check(r.ok, "math graph transpiles");

        VM vm; installStdlib(vm); Scheduler sched(vm);
        g_log.clear();
        vm.setLogSink([](std::string s) { g_log.push_back(std::move(s)); });
        check(runCode(r.code, vm, sched), "math graph code runs");
        check(logHas("answer=42"), "data-flow computed 6*7 and formatted the string");
    }

    // ------------------------------------------------------------------
    section("loop graph");
    {
        const std::string loopJson = R"JSON({
          "name": "Loop",
          "nodes": [
            {"id": 1, "type": "pure.literal", "pins":[{"id":"value","type":"int","dir":"out","value":"1"}]},
            {"id": 2, "type": "pure.literal", "pins":[{"id":"value","type":"int","dir":"out","value":"4"}]},
            {"id": 3, "type": "flow.forRange", "props": {"index":"i","inclusive":"1"},
             "pins":[{"id":"exec","type":"exec","dir":"in"},{"id":"from","type":"int","dir":"in"},
                     {"id":"to","type":"int","dir":"in"},{"id":"body","type":"exec","dir":"out"},
                     {"id":"completed","type":"exec","dir":"out"}]},
            {"id": 4, "type": "pure.string.format", "props": {"format":"i={}"},
             "pins":[{"id":"arg0","type":"any","dir":"in"},{"id":"value","type":"string","dir":"out"}]},
            {"id": 5, "type": "action.print",
             "pins":[{"id":"exec","type":"exec","dir":"in"},{"id":"value","type":"any","dir":"in"},
                     {"id":"then","type":"exec","dir":"out"}]}
          ],
          "connections": [
            {"from":1,"fromPin":"value","to":3,"toPin":"from"},
            {"from":2,"fromPin":"value","to":3,"toPin":"to"},
            {"from":3,"fromPin":"body","to":5,"toPin":"exec"},
            {"from":3,"fromPin":"body","to":4,"toPin":"arg0"},
            {"from":4,"fromPin":"value","to":5,"toPin":"value"}
          ]
        })JSON";
        Graph g; std::string e3;
        check(parseGraphJson(loopJson, g, e3), "loop graph parses");
        const TranspileResult r = t.transpile(g);
        std::printf("\n----- generated -----\n%s---------------------\n", r.code.c_str());
        check(r.code.find("for i in 1..=4 do") != std::string::npos, "forRange became a native LV Script loop");
        VM vm; installStdlib(vm); Scheduler sched(vm);
        g_log.clear();
        vm.setLogSink([](std::string s) { g_log.push_back(std::move(s)); });
        check(runCode(r.code, vm, sched), "loop code runs");
        check(logHas("i=1") && logHas("i=4"), "loop body executed for every iteration");
    }

    // ------------------------------------------------------------------
    section("error handling");
    {
        Graph bad;
        Node n1; n1.id = 1; n1.type = "action.print";
        n1.pins.push_back(Pin{"exec", PinType::Exec, PinDir::Input, "", ""});
        n1.pins.push_back(Pin{"value", PinType::Any, PinDir::Input, "", ""});
        n1.pins.push_back(Pin{"then", PinType::Exec, PinDir::Output, "", ""});
        Node n2 = n1; n2.id = 2;
        bad.nodes = {n1, n2};
        // Цикл в exec-графе: 1 -> 2 -> 1.
        bad.connections = {{1, "then", 2, "exec"}, {2, "then", 1, "exec"}};
        const TranspileResult r = t.transpile(bad);
        for (const auto& e : r.errors) std::printf("    [err] %s\n", e.c_str());
        check(!r.ok, "execution cycle is reported as an error, not an infinite loop");
        bool mentioned = false;
        for (const auto& e : r.errors) if (e.find("cycle") != std::string::npos) mentioned = true;
        check(mentioned, "error message mentions the cycle");
    }

    std::printf("\n----------------------------------------\n");
    std::printf("Visual Scripting self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
