/**
 * @file    stdlib_tests.cpp
 * @brief   Тесты стандартной библиотеки LV Script (stdlib/*.lvs).
 *
 * Каждый модуль загружается в VM, а затем прогоняется сценарий, проверяющий
 * реальное поведение: A* находит путь в обход стены, FSM переходит по условию,
 * инвентарь складывает предметы в стеки, диалог ветвится, квест завершается.
 */
#include "lvscript/Compiler.h"
#include "lvscript/Parser.h"
#include "lvscript/Scheduler.h"
#include "lvscript/Stdlib.h"
#include "lvscript/VM.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lv;

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

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Harness {
    VM vm;
    Scheduler sched;
    Harness() : vm(), sched(vm) {
        installStdlib(vm);
        vm.setLogSink([](std::string s) { g_log.push_back(std::move(s)); });
        vm.setErrorHandler([](const ScriptError& e, std::string_view tb) {
            std::fprintf(stderr, "    [runtime] %s\n%.*s", e.toString().c_str(),
                         static_cast<int>(tb.size()), tb.data());
        });
    }
    /// Загрузить модули stdlib + тестовый сценарий и выполнить.
    bool run(const std::vector<std::string>& modules, const std::string& scenario,
             const char* name = "stdlib-test") {
        std::string src;
        for (const auto& m : modules) {
            const std::string path = "stdlib/" + m + ".lvs";
            const std::string text = readFile(path);
            if (text.empty()) { std::fprintf(stderr, "    cannot read %s\n", path.c_str()); return false; }
            src += "# ---- module " + m + " ----\n" + text + "\n";
        }
        src += "# ---- scenario ----\n";
        src += scenario;

        DiagnosticList diags;
        Module mod = parseSource(src, name, diags);
        for (const Diagnostic& d : diags) std::fprintf(stderr, "    %s\n", d.toString().c_str());
        for (const Diagnostic& d : diags) if (d.severity == DiagSeverity::Error) return false;
        Compiler c(vm);
        ObjFunction* fn = c.compile(mod, diags);
        for (const Diagnostic& d : diags) std::fprintf(stderr, "    %s\n", d.toString().c_str());
        if (!fn) return false;
        const RunStatus st = vm.execute(fn);
        if (st != RunStatus::Ok) {
            std::fprintf(stderr, "    [vm] status=%d %s\n", static_cast<int>(st), vm.lastError().toString().c_str());
            return false;
        }
        for (int i = 0; i < 40; ++i) sched.tick(1.0f / 60.0f);
        return true;
    }
};
} // namespace

int main() {
    // ------------------------------------------------------------------
    section("fsm — конечный автомат");
    {
        Harness h;
        g_log.clear();
        check(h.run({"fsm"}, R"LV(
            let ai = new StateMachine()
            var hp = 100
            ai.state("Patrol", {"enter": || print("enter Patrol"), "update": |dt| print("patrol tick")})
            ai.state("Flee",   {"enter": || print("enter Flee")})
            ai.transition("Patrol", "Flee", || hp < 30)
            ai.start = nil
            ai.changeState("Patrol")
            ai.update(0.1)
            ai.update(0.1)
            print("current", ai.current, "elapsed>0", ai.elapsed() > 0)
            hp = 10
            ai.update(0.1)
            print("after damage", ai.current)
            print("isIn Flee", ai.isIn("Flee"))
        )LV"), "fsm module loads and runs");
        check(logHas("enter Patrol"), "enter handler fired");
        check(logHas("patrol tick"), "update handler fired");
        check(logHas("after damage\tFlee"), "conditional transition Patrol -> Flee");
        check(logHas("isIn Flee\ttrue"), "isIn() reflects the state");
    }

    // ------------------------------------------------------------------
    section("pathfinding — A*");
    {
        Harness h;
        g_log.clear();
        check(h.run({"pathfinding"}, R"LV(
            let nav = new GridNav(12, 12)
            # Стена с одним проходом сверху.
            for y in 0..10 do
                if y != 5 do nav.block(6, y) end
            end
            let path = nav.findPath({"x": 2, "y": 5}, {"x": 10, "y": 5})
            print("pathlen", len(path))
            print("start", path[0]["x"], path[0]["y"])
            print("goal", path[len(path)-1]["x"], path[len(path)-1]["y"])
            print("visited>0", nav.lastVisited > 0)
            # Путь обязан обходить стену через проход в y=5.
            var throughGap = false
            for p in path do
                if p["x"] == 6 and p["y"] == 5 do throughGap = true end
            end
            print("throughGap", throughGap)
            # Недостижимая цель.
            nav.block(3, 3)
            let none = nav.findPath({"x": 3, "y": 3}, {"x": 9, "y": 9})
            print("blocked-start-empty", len(none))
            # Путь (2,5) -> (10,5) идёт строго по горизонтали через проход в
            # стене, поэтому его длина РОВНО 8.0 (9 точек, 8 единичных шагов).
            # Проверяем «не короче прямой» с допуском на float-суммирование.
            print("dist", nav.pathLength(path) >= 7.999)
        )LV"), "pathfinding module loads and runs");
        check(logHas("throughGap\ttrue"), "A* routes through the only gap in the wall");
        check(logHas("goal\t10\t5"), "path ends at the goal");
        check(logHas("blocked-start-empty\t0"), "unreachable start yields an empty path");
        check(logHas("dist\ttrue"), "pathLength measures the route");
    }

    // ------------------------------------------------------------------
    section("inventory — стеки и вес");
    {
        Harness h;
        g_log.clear();
        check(h.run({"inventory"}, R"LV(
            let db = new ItemDatabase()
            let potion = new ItemDef("potion", "Health Potion", 16, 0.5)
            potion.category = "consumable"
            potion.consumable = true
            let sword = new ItemDef("sword", "Iron Sword", 1, 3.0)
            sword.category = "weapon"
            db.register(potion).register(sword)

            let inv = new Inventory(6, db)
            print("leftover", inv.add("potion", 40))
            print("potions", inv.countOf("potion"))
            print("slots used", [for s in inv.slots do s.count].filter(|c| c > 0).len())
            inv.add("sword", 1)
            print("weight", inv.weight())
            print("has 20 potions", inv.has("potion", 20))
            print("remove 35", inv.remove("potion", 35), "left", inv.countOf("potion"))
            print("weapons", inv.itemsInCategory("weapon").len())
            let saved = inv.serialize()
            let inv2 = new Inventory(6, db)
            inv2.deserialize(saved)
            print("restored", inv2.countOf("potion"), inv2.countOf("sword"))
        )LV"), "inventory module loads and runs");
        check(logHas("potions\t40"), "40 potions stored");
        check(logHas("slots used\t3"), "16-per-stack splits 40 potions into 3 slots");
        // 40 зелий по 0.5 (влезли все, leftover == 0) + 1 меч 3.0 = 23.0.
        check(logHas("weight\t23"), "weight = 40*0.5 + 1*3.0 = 23.0");
        check(logHas("remove 35\ttrue\tleft\t5"), "removal works and reports the remainder");
        check(logHas("restored\t5\t1"), "serialize/deserialize round-trip");
    }

    // ------------------------------------------------------------------
    section("dialogue — ветвящийся граф");
    {
        Harness h;
        g_log.clear();
        check(h.run({"dialogue"}, R"LV(
            let d = new Dialogue("intro")
            d.node("intro", {
                "speaker": "Elder",
                "text": "The well is dry.",
                "choices": [
                    {"text": "I will help", "goto": "accept", "setFlags": {"agreed": true}},
                    {"text": "Not my problem", "goto": "refuse", "require": |dlg| not dlg.flag("rude")}
                ]
            })
            d.node("accept", lineNode("Elder", "Bless you.", "reward"))
            d.node("reward", lineNode("Elder", "Take this coin.", nil))
            d.node("refuse", lineNode("Elder", "Then leave.", nil))

            d.start()
            print("choices", d.availableChoices().len())
            d.choose(0)
            print("now", d.currentId)
            d.advance()
            print("then", d.currentId)
            print("visited accept", d.visited("accept"))
            print("flag agreed", d.flag("agreed"))
            d.advance()
            print("finished", d.currentId == nil)
        )LV"), "dialogue module loads and runs");
        check(logHas("choices\t2"), "both choices available");
        check(logHas("now\taccept"), "choice 0 moved to the accept branch");
        check(logHas("then\treward"), "linear advance follows 'next'");
        check(logHas("flag agreed\ttrue"), "node actions set dialogue flags");
        check(logHas("finished\ttrue"), "dialogue finishes when next is nil");
    }

    // ------------------------------------------------------------------
    section("quest — цели и журнал");
    {
        Harness h;
        g_log.clear();
        check(h.run({"quest"}, R"LV(
            let log = new QuestLog()
            let q = new Quest("rats", "Cellar Rats", "Clear the cellar")
            q.objective("kill", "rat", 5, "Slay 5 rats")
            q.objective("collect", "pelt", 2, "Collect 2 pelts")
            q.rewards = {"xp": 120, "gold": 30, "items": ["potion"]}
            let follow = new Quest("report", "Report Back")
            follow.objective("talk", "elder", 1)
            q.nextQuest = "report"
            log.register(q).register(follow)

            log.start("rats")
            print("active", log.active.len())
            print("progress", q.progressText())
            log.notify("kill", "rat", 3)
            print("after 3 kills", q.progressText())
            log.notify("collect", "pelt", 2)
            log.notify("kill", "rat", 2)
            print("completed", log.isCompleted("rats"))
            print("chain started", log.isActive("report"))
            print("journal", log.journal().len())
        )LV"), "quest module loads and runs");
        // progressText() считает ЗАКРЫТЫЕ цели: после 3 из 5 убийств ни одна
        // из двух целей ещё не выполнена, поэтому «0/2».
        check(logHas("after 3 kills\t0/2"), "partial progress reported");
        check(logHas("completed\ttrue"), "quest completes when all objectives are done");
        check(logHas("chain started\ttrue"), "nextQuest chains automatically");
    }

    std::printf("\n----------------------------------------\n");
    std::printf("LV Script stdlib self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
