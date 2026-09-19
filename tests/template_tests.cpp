/**
 * @file    template_tests.cpp
 * @brief   Интеграционные тесты игровых шаблонов LimVine (templates/<имя>).
 *
 * Каждый шаблон запускается headless тем же раннером, что использует редактор и
 * CLI (`lvrun`), после чего тесты дёргают игровую логику напрямую — через
 * TemplateRunner::callGlobalFn / callMethodOnGlobal. Это проверяет не «скрипт
 * компилируется», а реальное поведение: грядка вспахивается, культура растёт со
 * временем, урожай собирается в инвентарь, отгрузка даёт деньги, день
 * перезапускает влажность.
 */
#include "scripting/TemplateRunner.h"
#include "lvscript/Value.h"

#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>

using namespace lv;
using namespace lv::scripting;

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

using StrMap = std::unordered_map<std::string, std::string>;

/// Строковое значение для передачи в скрипт.
///
/// VM::internString() уже возвращает Value (интернированная строка), поэтому
/// дополнительно оборачивать её в Value::object() не нужно.
Value str(VM& vm, const char* s) { return vm.internString(s); }

/// Снимок состояния шаблона: `templateState()` -> map строк.
StrMap snapshot(TemplateRunner& r) {
    Value out;
    if (!r.callGlobalFn("templateState", {}, &out)) return {};
    return valueToMap(out);
}
double num(const StrMap& m, const char* key) {
    const auto it = m.find(key);
    if (it == m.end()) return 0.0;
    try { return std::stod(it->second); } catch (...) { return 0.0; }
}
std::string str(const StrMap& m, const char* key) {
    const auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

} // namespace

// ===========================================================================
//  Isometric Farming
// ===========================================================================
static void testFarming() {
    section("farming_iso — изометрическая ферма");
    g_log.clear();

    TemplateRunner r;
    r.setLogCallback([](const std::string& s) { g_log.push_back(s); std::printf("    | %s\n", s.c_str()); });
    const auto res = r.load("templates/farming_iso");
    check(res.ok, "template loads and runs");
    if (!res.ok) {
        for (const auto& e : res.errors) std::printf("    ! %s\n", e.c_str());
        return;
    }
    check(res.scripts.size() == 6, "all six scripts were loaded in manifest order");

    StrMap st = snapshot(r);
    check(!st.empty(), "templateState() is callable from C++");
    check(num(st, "plots") == 48, "field has 8x6 = 48 plots");
    check(num(st, "gold") == 50, "starting gold comes from config");
    check(num(st, "seeds_turnip") == 6, "starting turnip seeds were granted");
    check(r.world().entityCount() >= 50, "ground + 48 plots + bin + farmer exist in ECS");

    // --- Вспашка ------------------------------------------------------------
    // Фермер спавнится у нижней кромки поля; первая доступная грядка ищется
    // по радиусу CFG_REACH, поэтому просто вызываем interact() несколько раз,
    // сдвигая фермера к центру поля.
    Value act;
    r.callMethodOnGlobal("farmer", "interact", {}, &act);
    check(act.toString() == "till" || act.toString() == "nothing" || act.toString() == "tired",
          "interact() reports a sensible first action");

    // Гарантированно вспашем конкретную клетку напрямую — так тест не зависит
    // от точки спавна.
    Value tilled;
    r.callGlobalFn("testTill", {Value::integer(2), Value::integer(2)}, &tilled);
    check(tilled.isBool() && tilled.asBool(), "testTill(2,2) tills the plot");

    Value planted;
    r.callGlobalFn("testPlant", {Value::integer(2), Value::integer(2)}, &planted);
    check(planted.isBool() && planted.asBool(), "testPlant(2,2) plants the selected crop");

    // --- Рост во времени ----------------------------------------------------
    Value stage0;
    r.callGlobalFn("testStage", {Value::integer(2), Value::integer(2)}, &stage0);
    check(stage0.asInt() == 0, "freshly planted crop is at stage 0");

    // 13 секунд игрового времени (780 кадров по 1/60) — половина цикла репы.
    r.runFrames(780);
    Value stage1;
    r.callGlobalFn("testStage", {Value::integer(2), Value::integer(2)}, &stage1);
    check(stage1.asInt() == 1, "crop reaches stage 1 after ~13 in-game seconds");

    // Полив ускоряет рост: поливаем и доигрываем до зрелости.
    r.callGlobalFn("testWater", {Value::integer(2), Value::integer(2)}, nullptr);
    r.runFrames(900);
    Value stage2;
    r.callGlobalFn("testStage", {Value::integer(2), Value::integer(2)}, &stage2);
    check(stage2.asInt() == 2, "watered crop matures (stage 2)");

    st = snapshot(r);
    check(num(st, "ready") >= 1, "templateState().ready counts mature crops");

    // --- Сбор и экономика ---------------------------------------------------
    Value harvested;
    r.callGlobalFn("testHarvest", {Value::integer(2), Value::integer(2)}, &harvested);
    check(harvested.asInt() == 1, "harvest yields one unit");
    st = snapshot(r);
    check(num(st, "turnips") == 1, "harvested turnip landed in the inventory");

    Value earned;
    r.callGlobalFn("testShip", {}, &earned);
    check(earned.asInt() == 9, "shipping one turnip pays its sell price (9)");
    st = snapshot(r);
    check(num(st, "gold") == 59, "gold increased to 50 + 9");

    // --- Смена дня ----------------------------------------------------------
    st = snapshot(r);
    check(!st.empty(), "state readable before sleep");
    const int dayBefore = static_cast<int>(num(st, "day"));
    r.callGlobalFn("testSleep", {}, nullptr);
    r.runFrames(2);                       // update() замечает, что день истёк
    check(logHas("[day "), "sleep prints the end-of-day summary");
    st = snapshot(r);
    check(!st.empty(), "state readable after sleep");
    check(num(st, "day") == dayBefore + 1, "day counter advanced");
    check(num(st, "energy") == 100, "farmer energy restored by sleep");

    // --- Покупка семян ------------------------------------------------------
    Value bought;
    r.callGlobalFn("testBuySeeds", {Value::integer(2)}, &bought);
    check(bought.asInt() == 2, "buySeed() spends gold and adds seeds");
    st = snapshot(r);
    check(num(st, "seeds_turnip") >= 1, "bought seeds are visible in the inventory");
}

// ===========================================================================
//  Third-Person RPG
// ===========================================================================
static void testRpg() {
    section("rpg_3p — RPG от третьего лица");
    g_log.clear();

    TemplateRunner r;
    r.setLogCallback([](const std::string& s) { g_log.push_back(s); std::printf("    | %s\n", s.c_str()); });
    const auto res = r.load("templates/rpg_3p");
    check(res.ok, "template loads and runs");
    if (!res.ok) { for (const auto& e : res.errors) std::printf("    ! %s\n", e.c_str()); return; }
    check(res.stdlib.size() == 6, "six stdlib modules were linked in");

    StrMap st = snapshot(r);
    check(!st.empty(), "templateState() is callable");
    check(num(st, "actors") == 4, "elder + smith + two wolves are registered");
    check(num(st, "enemies") == 2, "two hostile actors are alive");
    check(num(st, "playerHp") == 100, "player starts at full health");
    check(r.world().entityCount() >= 10, "ground, buildings, campfire, actors and player exist");

    // --- Навигация: A* обходит постройки ------------------------------------
    // Точки — точка спавна игрока (0, 6) и логово волка (12, 10). Брать
    // произвольные координаты нельзя: клетки под домами/колодцем помечены
    // непроходимыми (с запасом в полклетки), и A* честно вернёт пустой путь.
    Value path;
    r.callGlobalFn("testPathLength", {Value::fromNumber(0.0), Value::fromNumber(6.0),
                                      Value::fromNumber(12.0), Value::fromNumber(10.0)}, &path);
    check(path.asInt() >= 2, "A* builds a route from the village to the wolf lair");

    // --- Диалог и выдача квеста ---------------------------------------------
    Value node;
    r.callGlobalFn("testTalkTo", {str(r.scripts().vm(), "elder")}, &node);
    check(node.isObject(), "talking to the elder opens a dialogue node");
    st = snapshot(r);
    check(str(st, "dialogue") == "true", "dialogue is active while it runs");

    // Ветка «What is in it for me?» -> линейный узел -> «accept» (квест) -> «bye».
    // Важно: choose() применим ТОЛЬКО к узлу с choices; у линейного узла список
    // пуст, и choose(0) завершил бы диалог. Поэтому дальше — advance().
    r.callGlobalFn("testDialogueChoose", {Value::integer(1)}, &node);
    r.callGlobalFn("testDialogueAdvance", {}, &node);   // reward_talk -> accept
    r.callGlobalFn("testDialogueAdvance", {}, &node);   // accept -> bye
    r.callGlobalFn("testDialogueAdvance", {}, &node);   // bye -> конец
    st = snapshot(r);
    check(num(st, "questsActive") >= 1, "accepting starts the wolves quest");
    check(str(st, "dialogue") == "false", "dialogue closed after the last node");

    // --- Бой: два удара убивают волка ---------------------------------------
    // Подходим вплотную к волку и бьём: raycast обязан попасть в его тело.
    Value wolfPos;
    r.callGlobalFn("testActorPos", {str(r.scripts().vm(), "wolf_a")}, &wolfPos);
    check(wolfPos.isObject(), "testActorPos() returns the wolf position");
    const auto wp = valueToMap(wolfPos);
    const double wx = num(wp, "x"), wz = num(wp, "z");

    r.callGlobalFn("testTeleport", {Value::fromNumber(wx - 1.2), Value::fromNumber(wz)}, nullptr);
    Value hit;
    r.callGlobalFn("testAttackAt", {Value::fromNumber(wx), Value::fromNumber(wz)}, &hit);
    const auto h1 = valueToMap(hit);
    check(str(h1, "hit") == "true", "melee attack hits the wolf in front of the player");

    // Откат кулдауна (0.55 с). За это время волк, получив урон, переходит в
    // Chase и сам идёт к игроку, поэтому координаты цели перечитываем заново.
    r.runFrames(40);
    r.callGlobalFn("testActorPos", {str(r.scripts().vm(), "wolf_a")}, &wolfPos);
    const auto wp2 = valueToMap(wolfPos);
    r.callGlobalFn("testTeleport", {Value::fromNumber(num(wp2, "x") - 1.2), Value::fromNumber(num(wp2, "z"))}, nullptr);
    r.callGlobalFn("testAttackAt", {Value::fromNumber(num(wp2, "x")), Value::fromNumber(num(wp2, "z"))}, &hit);
    const auto h2 = valueToMap(hit);
    check(str(h2, "hit") == "true", "second swing also connects");
    check(str(h2, "hp") == "9" || num(h2, "hp") < 45, "wolf health dropped below its maximum");

    // --- Урон игроку и смерть ------------------------------------------------
    r.callGlobalFn("testDamagePlayer", {Value::integer(120)}, nullptr);
    st = snapshot(r);
    check(num(st, "playerHp") == 0, "player health clamps at zero");
    check(str(st, "playerDead") == "true", "player dies from a lethal hit");

    // --- FSM: бегство при низком здоровье ------------------------------------
    Value fsm;
    r.callGlobalFn("testForceState", {str(r.scripts().vm(), "wolf_b"),
                                      str(r.scripts().vm(), "Flee")}, &fsm);
    check(fsm.isBool() && fsm.asBool(), "FSM accepts an explicit state switch");
    r.callGlobalFn("testFsmState", {str(r.scripts().vm(), "wolf_b")}, &fsm);
    check(fsm.toString() == "Flee", "wolf_b stays in the Flee state");

    // --- Квест продвигается игровым событием ---------------------------------
    r.callGlobalFn("testNotify", {str(r.scripts().vm(), "kill"),
                                  str(r.scripts().vm(), "wolf"),
                                  Value::integer(2)}, nullptr);
    st = snapshot(r);
    check(str(st, "wolvesDone") == "true", "two kills complete the wolves quest");
    // Квесты в шаблоне связаны цепочкой (Quest.nextQuest): после «wolves»
    // автоматически стартует «smith_reward», поэтому активных снова один.
    check(num(st, "questsActive") == 1, "the follow-up quest auto-starts (quest chain)");
}

// ===========================================================================
//  FPS Shooter
// ===========================================================================
static void testFps() {
    section("fps_shooter — шутер от первого лица");
    g_log.clear();

    TemplateRunner r;
    r.setLogCallback([](const std::string& s) { g_log.push_back(s); std::printf("    | %s\n", s.c_str()); });
    const auto res = r.load("templates/fps_shooter");
    check(res.ok, "template loads and runs");
    if (!res.ok) { for (const auto& e : res.errors) std::printf("    ! %s\n", e.c_str()); return; }

    StrMap st = snapshot(r);
    check(!st.empty(), "templateState() is callable");
    check(num(st, "enemies") == 4, "four enemies were spawned");
    check(num(st, "covers") == 6, "arena has six cover boxes");
    check(num(st, "pickups") == 4, "four pickups are available");
    check(num(st, "ammoMag") == 30, "rifle starts with a full magazine");

    VM& vm = r.scripts().vm();

    // --- Точный выстрел по врагу -------------------------------------------
    // Тесты стреляют с обнулённым разбросом (testFirePrecise), чтобы проверять
    // механику попаданий, а не случайность конуса рассеивания.
    r.callGlobalFn("testTeleport", {Value::fromNumber(0.0), Value::fromNumber(14.0)}, nullptr);
    r.callGlobalFn("testLookAtEnemy", {str(vm, "enemy_3")}, nullptr);   // (15,15) — ближайший угол
    Value shot;
    r.callGlobalFn("testFirePrecise", {}, &shot);
    StrMap s1 = valueToMap(shot);
    check(str(s1, "shot") == "true", "trigger produces a shot");
    check(str(s1, "hit") == "true", "hitscan reaches the enemy in the crosshair");
    check(num(s1, "hp") == 60 - 22, "one rifle bullet takes 22 HP off a 60 HP enemy");

    // --- Магазин, перезарядка ----------------------------------------------
    st = snapshot(r);
    check(num(st, "ammoMag") == 29, "magazine decreased after one shot");

    // Выстрелы идут с прокруткой кулдауна между ними: rpm=640 даёт задержку
    // ~0.094 с, поэтому без testTickWeapon() второй выстрел отбрасывается как
    // «cooldown» и счётчик не растёт.
    r.callGlobalFn("testTickWeapon", {Value::fromNumber(0.2)}, nullptr);
    r.callGlobalFn("testFirePrecise", {}, &shot);
    r.callGlobalFn("testTickWeapon", {Value::fromNumber(0.2)}, nullptr);
    r.callGlobalFn("testFirePrecise", {}, &shot);
    st = snapshot(r);
    check(num(st, "shotsFired") == 3, "three precise shots are accounted for");
    check(num(st, "shotsHit") == 3, "all three shots hit the aimed enemy");

    // Выстреливаем магазин до конца: дальше оружие обязано уйти в перезарядку.
    for (int i = 0; i < 40; ++i) {
        r.callGlobalFn("testTickWeapon", {Value::fromNumber(0.1)}, nullptr);
        r.callGlobalFn("testFirePrecise", {}, &shot);
    }
    st = snapshot(r);
    check(num(st, "ammoMag") == 0, "magazine empties after sustained fire");
    check(str(st, "reloading") == "true", "empty magazine triggers an automatic reload");

    // Перезарядка занимает 1.9 с: через 2 с магазин снова полон.
    r.callGlobalFn("testTickWeapon", {Value::fromNumber(2.1)}, nullptr);
    st = snapshot(r);
    check(num(st, "ammoMag") == 30, "reload refills the magazine");

    // --- FSM врага: патруль -> погоня --------------------------------------
    Value fsm;
    r.callGlobalFn("testForceEnemyState", {str(vm, "enemy_0"), str(vm, "Chase")}, &fsm);
    check(fsm.isBool() && fsm.asBool(), "enemy FSM accepts an explicit state switch");
    r.callGlobalFn("testEnemyState", {str(vm, "enemy_0")}, &fsm);
    check(fsm.toString() == "Chase", "enemy stays in Chase");

    // --- Урон и смерть врага ------------------------------------------------
    Value hp;
    r.callGlobalFn("testDamageEnemy", {str(vm, "enemy_0"), Value::integer(100)}, &hp);
    check(hp.asInt() == 0, "lethal damage clamps enemy HP at zero");
    st = snapshot(r);
    // enemy_3 добит тремя точными выстрелами выше (22*3 = 66 > 60 HP),
    // enemy_0 — явным testDamageEnemy(): из четырёх врагов остаётся двое.
    check(num(st, "enemiesAlive") == 2, "both killed enemies are excluded from the alive count");

    // --- Пикап: аптечка лечит игрока ---------------------------------------
    // Сначала снимаем здоровье: на полном HP медпакет ничего не меняет
    // (min со 100), и утверждение «не меньше, чем было» было бы пустым.
    Value hpAfterDamage;
    r.callGlobalFn("testHurtPlayer", {Value::integer(40)}, &hpAfterDamage);
    check(hpAfterDamage.asInt() == 60, "testHurtPlayer() lowers player health to 60");

    r.callGlobalFn("testTeleport", {Value::fromNumber(-6.0), Value::fromNumber(6.0)}, nullptr);
    r.runFrames(3);
    st = snapshot(r);
    check(num(st, "pickups") == 3, "walking over a pickup removes it from the arena");
    // +25 от медпакета и немного пассивной регенерации за 3 кадра (1.2 HP/с),
    // поэтому сравниваем с допуском, а не на точное равенство.
    // Точное значение проверять нельзя: за эти кадры по игроку успевают
    // выстрелить враги (6 урона за попадание), плюс идёт пассивная
    // регенерация 1.2 HP/с. Поэтому проверяем сам факт лечения — здоровье
    // заметно выросло относительно 60, а число доступных пикапов упало.
    const double hpNow = num(st, "playerHp");
    check(hpNow >= 70.0 && hpNow <= 100.0, "health pickup heals the player (60 -> 70..100)");
    check(logHas("[pickup]"), "pickup is reported to the log");

    // --- Движение от клавиатуры (настоящий ввод, без тестовых хуков) ---------
    // Телепортируем игрока в центр и запоминаем позицию: дальше он обязан
    // сдвинуться САМ — через inputAxis("MoveY") -> updatePlayer -> setPosition.
    // Это проверяет всю цепочку ввода, а не только скриптовую математику.
    Value pos0;
    r.callGlobalFn("testTeleport", {Value::fromNumber(0.0), Value::fromNumber(0.0)}, &pos0);
    const auto before = valueToMap(pos0);
    for (int i = 0; i < 30; ++i) {
        r.input().beginFrame();
        r.input().onKey(87, true);        // W — вперёд
        r.tick(1.0f / 60.0f);
        r.input().endFrame();
    }
    r.input().beginFrame();
    r.input().onKey(87, false);
    r.input().endFrame();
    Value pos;
    r.callGlobalFn("testPlayerPos", {}, &pos);
    const auto pp = valueToMap(pos);
    const double moved = std::abs(num(pp, "x") - num(before, "x")) +
                         std::abs(num(pp, "z") - num(before, "z"));
    check(moved > 0.2, "holding W actually moves the player (real input path)");
}

int main() {
    JobSystem::init(0);
    testFarming();
    testRpg();
    testFps();

    std::printf("\n----------------------------------------\n");
    std::printf("Template self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
