# LimVine — подсистемы движка: API и рецепты

Документ описывает **фактический** API (проверяется тестами в `tests/`) и
даёт рецепты для типичных задач. Язык LV Script описан отдельно —
`docs/03_lvscript_spec.md`.

---

## 1. ECS

### 1.1 Компоненты

Компонент — обычный POD/struct с меткой:

```cpp
struct Transform {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1, 1, 1};
    static constexpr std::string_view lv_component_name = "Transform";
};
```

`lv_component_name` делает компонент видимым из LV Script (имя в
`getComponent` / `setField` / `getField`).

### 1.2 Создание и итерация

```cpp
using namespace lv::ecs;
Registry reg;
registerComponent<Transform>();

Entity e = reg.create();
reg.add<Transform>(e, Transform{{0, 1, 0}, {}, {1, 1, 1}});

// Плотная итерация по паре компонентов:
reg.each<Transform, MeshRenderer>([](Entity ent, Transform& t, MeshRenderer& mr) {
    t.position.y += 0.01f;
    return true;                    // false = прервать обход
});

if (const Transform* t = reg.get<Transform>(e)) { /* ... */ }
auto& t2 = reg.require<Transform>(e);          // бросает, если компонента нет
reg.remove<MeshRenderer>(e);
reg.destroy(e);
bool ok = reg.alive(e);                        // проверка поколения
```

### 1.3 Отложенные изменения

```cpp
CommandBuffer cmd;
cmd.spawn([&](Entity e2) { reg.add<Transform>(e2); });
cmd.destroy(oldEntity);
cmd.apply(reg);                                // между фазами, не во время each()
```

### 1.4 Системы и фазы

```cpp
World world;
SystemDesc sys;
sys.name  = "Gravity";
sys.phase = Phase::Physics;                    // Input/Script/Logic/Physics/PostPhysics/Render/Late
sys.order = 10;                                // внутри фазы
sys.update = [](World& w, float dt) { /* ... */ };
world.addSystem(std::move(sys));

world.tick(1.0f / 60.0f);                      // исполняет фазы по порядку
```

### 1.5 Иерархия сущностей

`Transform` — всегда **локальное** преобразование. Родительские связи задаются
`setParent`, а мировые величины движок кладёт в `WorldTransform`:

```cpp
#include "ecs/Hierarchy.h"

Entity player = reg.create();  reg.add<Transform>(player)->position = {10, 0, 0};
Entity weapon = reg.create();  reg.add<Transform>(weapon)->position = {0.3f, 1.2f, 0};

setParent(reg, weapon, player);                // оружие в руке игрока

world.tick(dt);                                // PostPhysics обновит матрицы

const WorldTransform* wt = reg.get<WorldTransform>(weapon);
Vec3 muzzle = wt->worldPosition;               // {10.3, 1.2, 0} — уже в мире
```

| Операция | Вызов |
|---|---|
| прикрепить | `setParent(reg, child, parent)` |
| прикрепить, не сдвинув объект | `setParent(reg, child, parent, /*keepWorldPosition=*/true)` |
| открепить в корень | `setParent(reg, child, kNullEntity)` |
| матрица без учёта кэша | `computeWorldMatrix(reg, e)` |
| обновить вручную (вне `World`) | `updateWorldTransforms(reg)` |

Попытка создать цикл (`A → B → A` или родитель сам себе) бросает
`HierarchyCycleError`, оставляя граф неизменным. Дети уничтоженного родителя
автоматически становятся корнями.

`WorldTransform::version` монотонно растёт при каждом пересчёте — это дешёвый
dirty-триггер: система может сравнить сохранённую версию и пропустить работу,
если объект не двигался.

> Внутри обхода иерархии нельзя добавлять и удалять компоненты у сущностей,
> участвующих в графе: это структурное изменение, которое переселяет сущность
> между архетипами и инвалидирует итераторы. Отложите его в `CommandBuffer`.


### 1.6 Сцены (`.lvscene`)

Сцена — JSON-снимок ECS-мира. Формат построен на той же рефлексии
(`BindingRegistry`), которую используют инспектор редактора и `getField()`
в LV Script, поэтому **что видно в инспекторе — то и сохраняется**: отдельного
кода сериализации на каждый компонент писать не нужно.

```cpp
#include "scene/Scene.h"

std::string err;
lv::scene::saveSceneToFile(world, "levels/village.lvscene", {}, &err);

lv::ecs::World fresh;
auto r = lv::scene::loadSceneFromFile(fresh, "levels/village.lvscene");
if (!r) std::fprintf(stderr, "%s\n", r.error.c_str());
for (const std::string& w : r.warnings) std::puts(w.c_str());   // не фатально
```

Из скриптов (требует `Cap_IO`, поэтому недоступно модам):

```lv
let e = spawn()
setName(e, "Chest")
setField(e, "Transform", "position", vec3(4, 0, 9))

saveScene("saves/slot1.lvscene")

clearScene()
let r = loadScene("saves/slot1.lvscene")
print("загружено сущностей: {r.entities}, предупреждений: {r.warnings}")
```

Свойства формата, на которые можно опираться:

| Свойство | Как достигается | Зачем |
|---|---|---|
| Ссылки на сущности переживают загрузку | в файл пишется плотный `id`, а не `Entity{index, generation}`; после создания всех сущностей ссылки чинятся вторым проходом | handle зависит от истории аллокаций и при загрузке всегда другой |
| Нет рассогласованных данных | сохраняется только `Parent`; `Children` и `WorldTransform` пересчитываются при загрузке | в файле нет второй копии — рассогласовать нечего |
| `save -> load -> save` побайтово стабилен | детерминированный порядок сущностей и компонентов | дружелюбные git-diff'ы и сравнение снимков в тестах |
| Старый движок читает новые сцены | неизвестный компонент/поле → запись в `warnings`, а не отказ | схема может эволюционировать без миграций |
| Частичная запись не портит сцену | запись через `tmp` + `rename` | падение на сохранении не уничтожает уровень |

`loadScene` **догружает** сцену поверх текущего мира — это нужно для стриминга
уровней по частям. Чтобы заменить содержимое, сначала вызовите `clearScene()`
(в C++ — `world.registry().clear()`).

Имя сущности хранится в обычном ECS-компоненте `scene::Name`, а не во внешней
карте: так оно переживает сериализацию и удаление сущности без отдельного кода
очистки.

---

## 2. LV Script в движке

### 2.1 ScriptWorld

```cpp
EngineContext ctx;
ctx.world = &world; ctx.physics = &physics; ctx.input = &input;
ctx.audio = &audio; ctx.renderer = &renderer;

ScriptWorld scripts;
scripts.setLogCallback([](const std::string& line) { ui.console(line); });
scripts.attach(ctx);                           // ставит stdlib + биндинги движка

scripts.runFile("game.lvs", sourceText);       // загрузить и выполнить
scripts.tick(dt);                              // планировщик + dispatch("update")
scripts.dispatch("onCollision", {a, b});       // свой колбэк
scripts.hotReload("game.lvs", newSourceText);  // с сохранением состояния
```

### 2.2 Регистрация своего компонента в скриптах

```cpp
auto& reg = BindingRegistry::instance();
using FK = FieldBinding::Kind;

reg.registerComponent<Health>("Health");
reg.field<Health>("Health", "current", &Health::current, FK::Int);
reg.field<Health>("Health", "max",     &Health::max,     FK::Int);
reg.field<Health>("Health", "invuln",  &Health::invuln,  FK::Bool, /*readOnly=*/true);
```

После этого в скрипте доступны:

```lua
print(getField(e, "Health", "current"))
setField(e, "Health", "current", 42)
if hasComponent(e, "Health") do ... end
local h = getComponent(e, "Health")   -- map со всеми полями
```

Поддерживаемые виды полей: `Float, Int, Bool, Vec3, Vec2, Quat, Color, Entity,
String`. Добавление нового типа движка = одна специализация `Marshal<T>`.

### 2.3 Вызов скрипта из C++

```cpp
Value* fn = vm.findGlobal("templateState");
Value out;
vm.callValue(*fn, {}, &out);

auto fields = valueToMap(out);                 // TemplateRunner.h
double gold = std::stod(fields["gold"]);

vm.callMethod(instance, "interact", args, &out);
```

---

## 3. Шаблоны и TemplateRunner

### 3.1 Манифест

```json
{
  "id": "fps_shooter",
  "name": "FPS Shooter",
  "summary": "…",
  "engine": { "backend": "Vulkan", "physics": "Jolt", "audio": "OpenAL" },
  "stdlib": ["fsm", "tween"],
  "scripts": ["scripts/config.lvs", "scripts/arena.lvs", "scripts/main.lvs"],
  "input": { "contexts": ["Combat"], "actions": ["MoveX", "Fire"] },
  "tags": ["fps", "hitscan"]
}
```

Порядок `scripts` **значим**: объявления верхнего уровня модуля являются
глобалами, поэтому `config.lvs` обязан идти первым.

### 3.2 Запуск

```cpp
TemplateRunner runner;
runner.setLogCallback([](const std::string& s) { std::printf("%s\n", s.c_str()); });
const auto res = runner.load("templates/fps_shooter");
if (!res.ok) for (auto& e : res.errors) std::fprintf(stderr, "%s\n", e.c_str());

runner.runFrames(600, 1.0f / 60.0f);           // 10 игровых секунд
runner.input().beginFrame();                   // настоящий ввод
runner.input().onKey(87, true);                // W
runner.tick(1.0f / 60.0f);
runner.input().endFrame();

Value out;
runner.callGlobalFn("templateState", {}, &out);
runner.callMethodOnGlobal("farmer", "interact", {}, &out);
```

CLI:

```bash
lvrun templates/rpg_3p --frames 600            # прогнать 10 секунд headless
lvrun templates/rpg_3p --frames 60 --disasm    # + байткод модуля
lvrun templates/farming_iso --input --frames 120   # подать демонстрационный ввод
lvrun script.lvs                               # отдельный файл
```

### 3.3 lvcook — проверка и упаковка контента

Ошибка в любом `.lvs` всплывает только в рантайме, поэтому в CI контент
прогоняется через парсер и компилятор отдельной утилитой:

```bash
lvcook check                                   # stdlib/ и templates/ (код возврата 1 при ошибке)
lvcook check templates/rpg_3p --quiet          # один шаблон, только ошибки
lvcook compile                                 # скомпилировать всё в кэш .lvcache/*.lvc
lvcook compile templates/rpg_3p --cache-dir .lvcache
lvcook clean                                   # очистить кэш байткода
lvcook bundle templates/farming_iso -o dist/farming.lvs   # один файл для дистрибуции
lvcook list                                    # шаблоны и их stdlib-зависимости
```

`bundle` склеивает модули stdlib и скрипты шаблона **в порядке из
`template.json`** (порядок значим: объявления верхнего уровня модуля являются
глобалами), добавляя маркеры `# ---- file ... ----` по одному на строку, чтобы
номера строк в трейсбеках совпадали с исходниками. Полученный файл исполняется
`lvrun` напрямую.

### 3.4 Кэш байткода (`.lvc`)

`lvcook compile` прогоняет исходники через компилятор и сохраняет байткод в
`.lvcache/` рядом с проектом. Кэш экономит полный проход лексер → парсер →
компилятор при каждом запуске редактора; на `stdlib/` и трёх шаблонах он даёт
26 модулей и занимает на 30–50 % меньше исходников.

```cpp
lv::BytecodeCache cache(".lvcache");
if (auto cached = cache.load(vm, path, sourceText)) {
    vm.execute(cached->entry);            // компиляция пропущена
} else {
    ObjFunction* fn = compiler.compile(module, diags);
    lv::BytecodeModule m; m.entry = fn; m.name = path.string();
    cache.store(vm, path, sourceText, m); // ошибки записи не фатальны
}
```

Валидность кэша определяется двумя условиями: версия формата
(`kBytecodeVersion`) и FNV-1a хэш **текста** исходника. Хэш содержимого выбран
вместо `mtime` намеренно — он переживает `git checkout` и корректно работает в
CI, где у всех файлов одинаковое время модификации.

Что попадает в `.lvc`: прототипы функций, пул констант, таблица строк, таблица
номеров строк и собственная **таблица имён модуля**. Последняя обязательна:
операнды инструкций кодируют имена как индексы в `VM::names()`, а этот порядок
различается между экземплярами VM — при загрузке индексы переотображаются в
таблицу целевой машины.

Что НЕ попадает: классы, инстансы, замыкания, корутины и нативные функции. Это
рантайм-объекты; в кэше лежит байткод, который их создаёт, а сами объекты
появляются при исполнении верхнего уровня модуля — ровно как при горячей
перезагрузке.

### 3.5 Контракт шаблона

Каждый шаблон обязан предоставлять:

| Имя | Назначение |
|---|---|
| `update(dt)` | глобальный кадр (вызывает `ScriptWorld::tick`) |
| `templateState()` | map с состоянием — для HUD, консоли редактора и тестов |
| `test*()` | тонкие хуки над настоящей логикой (без эмуляции ввода по кадрам) |

---

## 4. Рендер

```cpp
render::Renderer renderer;
render::SurfaceDesc surface{1280, 720};
renderer.init(render::Backend::Null, surface);      // Null | OpenGL | Vulkan
renderer.attachToWorld(world);                      // система фазы Render

MeshData mesh = makeBoxMesh({1, 1, 1});
DrawItem item;
item.mesh = &mesh; item.transform = modelMatrix; item.material = mat;
Batcher batcher;
batcher.submit(item);
auto batches = batcher.cullAndBatch(camera);        // frustum + instancing
```

* `Vertex` — 64 байта: позиция, нормаль, UV, тангент, цвет, материал-id.
* `InstanceBatch` — один меш + массив трансформаций (один draw call).
* `MergedChunk` — слитая статическая геометрия уровня.
* `Frustum` — шесть плоскостей; `Renderer::collectDebugLines` даёт wireframe
  для отладки в редакторе.

Измерение: **4540 объектов → 438 draw calls** (`tests/render_tests.cpp`).

---

## 5. Физика

```cpp
physics::PhysicsWorld physics;
physics.init(4096);                                  // ёмкость пула тел
physics::attachToWorld(physics, world);              // система фазы Physics

BodyDesc d;
d.kind      = BodyKind::Kinematic;                   // Static | Kinematic | Dynamic
d.position  = {0, 1, 0};
d.shape.kind = ShapeKind::Box;
d.shape.halfExtents = {0.5f, 0.9f, 0.5f};
d.entity    = e;
BodyId body = physics.createBody(d);

physics.setLinearVelocity(body, {0, 0, 5});
physics.applyImpulse(body, {0, 6, 0}, atWorldPos);
physics.teleportBody(body, newPos, newRot);          // обязателен для телепорта

RaycastHit hit = physics.raycast(Ray{origin, dir}, 50.0f);
auto overlaps  = physics.overlapSphere(center, radius);
```

**Источник истины по типам тел** (см. также §6 архитектуры):

| Тип | Кто владеет позицией | Как телепортировать |
|---|---|---|
| Static | `desc` | пересоздать тело |
| Kinematic | `Transform` | `setPosition()` из скрипта достаточно |
| Dynamic | тело | `teleportBody()` (биндинг `setPosition` делает это сам) |

Фиксированный шаг с аккумулятором: `step()` накапливает `deltaTime` и делает
целое число подшагов `fixedStep_`; `maxSubSteps_` защищает от «спирали смерти»
на просадках FPS (долг сбрасывается).

---

## 6. Ввод

```cpp
input::InputSystem input;
auto& gameplay = input.createContext("Gameplay", /*priority=*/0);
gameplay.mapKeyAxis("MoveX", 65, -1.f);              // A
gameplay.mapKeyAxis("MoveX", 68, +1.f);              // D
gameplay.mapKey("Jump", 32);                         // Space
gameplay.mapMouseButton("Fire", 0);                  // ЛКМ
gameplay.mapAxis("Look", BindingKind::MouseDeltaX, 0, 1.f);
input.pushContext("Gameplay");

// Кадр:
input.beginFrame();                                  // сохранить prev-состояние
input.onKey(68, true);                               // события от окна
input.endFrame();                                    // вычислить состояния

if (input.pressed("Jump")) { /* ... */ }
float x = input.axis("MoveX");
Vec2 mv   = input.axis2D("MoveX", "MoveY");
Vec2 look = input.mouseDelta();
```

Контексты образуют стек с приоритетами: `pushContext("Dialogue", 10)`
перехватывает действия, `popContext("Dialogue")` возвращает управление.
Привязки сериализуются в JSON (`serializeBindings` / `applyBindings`) — это
экран настроек и пользовательский конфиг.

Из скрипта раскладка объявляется там же, где логика:

```lua
createInputContext("Gameplay", 0)
mapKeyAxis("Gameplay", "MoveX", 65, -1.0)
mapKey("Gameplay", "Interact", 69)
mapMouseButton("Gameplay", "Fire", 0)
pushInputContext("Gameplay")
```

---

## 7. Звук

```cpp
audio::AudioSystem audio;
audio.init(/*voices=*/24, /*forceNull=*/true);       // Null-бэкенд для CI
audio::attachToWorld(audio, world);

auto clip  = audio.loadClip("assets/shot.wav");
auto voice = audio.play3D(clip, position, volume);
audio.stop(voice);
audio.setBusVolume("sfx", 0.8f);
```

Пулы голосов конечны, поэтому есть **вытеснение**: при нехватке голосов
вытесняется наименее приоритетный с учётом дистанции до слушателя. Так «100
выстрелов одновременно» не обрывают музыку и не текут.

---

## 8. Ассеты

```cpp
asset::AssetManager assets;
assets.setRoot("assets/");
assets.mountWatcher();                               // polling FileWatcher

auto tex = assets.load<Texture>("wood.png");         // кэш по (путь, mtime, хэш)
assets.onReload("wood.png", [](AssetHandle h) { /* пересоздать */ });
assets.pollChanges();                                // каскад по зависимостям
```

Каскад означает: изменение `.mat` перезагружает связанные `.mesh`, а изменение
`.lvs` вызывает `ScriptWorld::hotReload` (подмена тела прототипа с сохранением
состояния инстансов).

---

## 9. Visual Scripting

```cpp
visual::Graph graph = visual::parseGraph(jsonText);  // JsonMini
std::vector<std::string> diags;
std::string code = visual::transpile(graph, &diags); // → LV Script
visual::Graph back = visual::parseFromCode(code);    // round-trip по #@node
```

Типы узлов:

| Категория | Примеры |
|---|---|
| `event.*` | `event.update`, `event.collision`, `event.start` |
| `flow.*` | `flow.branch`, `flow.sequence`, `flow.forRange`, `flow.whileLoop`, `flow.break`, `flow.continue` |
| `action.*` | `action.spawn`, `action.setPosition`, `action.playSound`, `action.print` |
| `pure.*` | `pure.add`, `pure.compare`, `pure.getField`, `pure.vec3` |

Транспилятор обходит **exec-рёбра** (порядок исполнения) и подставляет выражения
по **data-рёбрам**; циклы в exec-графе детектируются отдельно, чтобы не уйти в
бесконечную рекурсию. Сгенерированный код — обычный LV Script: его можно
читать, править руками и возвращать обратно в граф.

---

## 10. JobSystem

```cpp
JobSystem::init(0);                                  // 0 = по числу ядер

parallelFor(n, [&](int i) { /* ... */ });
parallelForRange(0, n, 256, [&](int lo, int hi) { /* чанк */ });
```

Правила: задача не меняет ECS структурно (только через `CommandBuffer`), не
бросает исключений наружу и не захватывает ссылки на короткоживущие локалы.

---

## 11. Отладка

| Инструмент | Как включить |
|---|---|
| Дизассемблер байткода | `lvrun --disasm`, `lv::Disassembler::disassemble(fn, &vm.names())` |
| Трейсбек ошибки | `vm.traceback()` (автоматически в лог через `setLogCallback`) |
| Консоль редактора | `:disasm`, `:entities`, `:reload <path>`, REPL поверх LV Script |
| Панели без окна | `limvine-editor --dump-panels [template] [--frames N]` |
| Профилировщик VM | `vm.setProfiling(true)` → `profile_.instructions/calls/returns` |
| Wireframe физики | `PhysicsWorld::collectDebugLines(out, color)` |
| Все self-test'ы | `bash build_tests.sh` (14 сьютов, 577 проверок, headless) |

> На машинах с 1–2 ГБ ОЗУ линковка тяжёлых сьютов с `-O2` может упасть без
> сообщения (collect2/ld). В `build_tests.sh` для них используется `-O1`
> (`FLAGS_LINK_HEAVY`) — на результаты тестов это не влияет.
