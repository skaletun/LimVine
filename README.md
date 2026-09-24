# LimVine

Игровой 3D-движок для low-poly игр: **C++20**, data-oriented **ECS**, **job
system**, собственный скриптовый язык **LV Script** (компиляция в байткод,
стековая VM, GC, корутины, песочница), **Visual Scripting** поверх него и три
готовых игровых шаблона.

Всё дерево собирается и проходит **707 проверок в 16 self-test'ах без единой
внешней зависимости** (headless, Null-бэкенды рендера/звука, редактор без
ImGui). Vulkan, OpenGL, Jolt, OpenAL и ImGui подключаются опциями `LV_WITH_*`.

Физика — не заглушка: **Jolt Physics 5.x подключён по-настоящему**, а выбор
движка сделан через интерфейс `IPhysicsBackend`. Оба бэкенда (встроенный
детерминированный симулятор и Jolt) проходят **один и тот же контрактный
набор** `physics_tests`, поэтому расхождение в их поведении ловится в CI:
с включённым Jolt тестов становится 767.

```bash
# Сборка с настоящим Jolt (исходники подпроектом)
cmake -B build -DLV_WITH_JOLT=ON -DLV_JOLT_SOURCE_DIR=/путь/к/JoltPhysics
# ...или с установленным пакетом
cmake -B build -DLV_WITH_JOLT=ON -DJolt_DIR=/путь/к/lib/cmake/Jolt
```

```
LV Script self-test:        53 passed     Editor self-test:       36 passed
ECS self-test:              13 passed     Template self-test:     71 passed
Render self-test:           15 passed     LV Script stdlib:       26 passed
Input self-test:            17 passed     Engine integration:     27 passed
JobSystem self-test:         6 passed     Visual Scripting:       31 passed
Hierarchy self-test:        44 passed     Bytecode self-test:     74 passed
GC self-test:               60 passed     Scene self-test:       104 passed
Gameplay self-test:         70 passed
```

---

## Быстрый старт

### Без CMake (только g++ и bash)

```bash
bash build_tests.sh          # собрать и прогнать все 16 сьютов
```

### CMake

```bash
cmake -B build -G Ninja
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Опции: `LV_BUILD_TESTS`, `LV_BUILD_TOOLS`, `LV_BUILD_EDITOR`, `LV_WERROR`,
`LV_WITH_VULKAN`, `LV_WITH_OPENGL`, `LV_WITH_JOLT`, `LV_WITH_OPENAL`,
`LV_WITH_IMGUI`.

### Запустить игровой шаблон headless

```bash
./build/lvrun templates/farming_iso --frames 600        # 10 игровых секунд
./build/lvrun templates/rpg_3p     --frames 60 --disasm # + байткод модуля
./build/lvrun templates/fps_shooter --input --frames 120
./build/lvrun path/to/script.lvs                        # отдельный файл

# проверка контента в CI и упаковка шаблона в один файл
./build/lvcook check
./build/lvcook bundle templates/rpg_3p -o dist/rpg.lvs
```

---

## Что внутри

| Подсистема | Кратко |
|---|---|
| `engine/ecs` | Архетипный Registry (SoA-пулы, swap-remove, поколения сущностей), `CommandBuffer`, `World` с фазами кадра |
| `engine/lvscript` | Lexer → Parser → Compiler → VM: NaN-boxing, mark-and-sweep GC, корутины с планировщиком, песочница (capabilities + топливо), трейсбеки, дизассемблер |
| `engine/render` | Frustum culling + GPU instancing + слияние статики; 4540 объектов → 438 draw calls |
| `engine/physics` | Обёртка Jolt + встроенный детерминированный симулятор (AABB, raycast, overlapSphere, фиксированный шаг, матрица коллизий 16×16) |
| `engine/input` | Input Mapping Contexts: стек контекстов с приоритетами, клавиши/мышь/геймпад как оси, сериализация привязок в JSON |
| `engine/audio` | Пул голосов с вытеснением по приоритету/дистанции, шины, `AudioSource` |
| `engine/asset` | Кэш по (путь, mtime, хэш), polling-`FileWatcher`, каскадная перезагрузка по зависимостям |
| `engine/scripting` | `ScriptWorld`, `BindingRegistry` (компоненты по offset), `TemplateRunner`, Visual Scripting (граф → LV Script, round-trip) |
| `engine/editor` | Панели Hierarchy / Inspector / Asset Browser / Viewport / Console+REPL против абстракции `UIDraw`: один и тот же код рисует и в окно ImGui (`LV_WITH_IMGUI`), и в строку (`TextUIDraw`) для headless-тестов и `--dump-panels` |
| `stdlib/*.lvs` | tween, fsm, pathfinding (A*), inventory, dialogue, quest |
| `templates/*` | Isometric Farming, Third-Person RPG, FPS Shooter |

---

## LV Script за 60 секунд

```lua
import math

class Crop do
    var stage = 0
    var growth = 0.0

    func init(stageSeconds: Float) do
        this.stageSeconds = stageSeconds
    end

    /// Рост считается накоплением времени, а не корутиной на каждую грядку.
    func update(dt) do
        this.growth += dt
        if this.growth >= this.stageSeconds do
            this.growth = 0.0
            this.stage += 1
        end
        return this.stage
    end
end

# Корутина — для плавных процессов, где уместно «спать» между кадрами.
coroutine growFx(entity, seconds: Float) do
    var t = 0.0
    while t < seconds do
        let k = 1.0 + 0.1 * math.sin(t / seconds * math.pi)
        setField(entity, "Transform", "scale", vec3(k, k, k))
        t += deltaTime()
        yield t
        wait(deltaTime())
    end
end

# Кадровый обработчик: его вызывает движок каждый кадр.
func update(dt) do
    if inputPressed("Interact") do
        let target = raycast(getPosition(player), lookDirection(), 3.0)
        if target["hit"] do
            print("взаимодействие с {target["distance"]}")
        end
    end
end
```

Полный язык — в [`docs/03_lvscript_spec.md`](docs/03_lvscript_spec.md):
блоки `do … end`, `let`/`var`, классы с наследованием и `super`, `match` с
паттернами, `Result`/`try`/`?`, лямбды (`|x| expr` и `|x| { stmts }`),
интерполяция строк, variadic-параметры, `while`/`for` как выражения, list
comprehension, capability-песочница для модов.

---

## Шаблоны игр

| Шаблон | Что демонстрирует |
|---|---|
| `templates/farming_iso` | Изометрическое управление, сетка грядок, рост во времени, полив, инвентарь со стеками, отгрузка, экономика и цикл дня |
| `templates/rpg_3p` | NPC на конечных автоматах, патруль по A*-маршруту в обход домов, ветвящиеся диалоги с условиями `require`, квесты-данные с цепочками, ближний бой с автонаведением |
| `templates/fps_shooter` | Hitscan с bloom-разбросом и отдачей камеры, перезарядка, ИИ Patrol/Chase/Attack с проверкой видимости raycast'ом, пикапы с респавном |

Каждый шаблон — это `template.json` (зависимости stdlib, порядок скриптов,
контексты ввода) и набор `.lvs`. **Ни строчки C++**: новый шаблон не требует
пересборки движка. Шаблоны обязаны предоставлять `update(dt)`,
`templateState()` и тонкие `test*()`-хуки — именно через них
`tests/template_tests.cpp` реально «играет» в каждый из них (71 проверка).

---

## Документация

| Документ | Содержание |
|---|---|
| [`docs/01_architecture.md`](docs/01_architecture.md) | Слои и правила зависимостей, ECS и фазы кадра, конвейер LV Script, контракт стека вызова, рендер/физика/jobs, стратегия тестирования |
| [`docs/02_directory_structure.md`](docs/02_directory_structure.md) | Дерево каталогов с назначением каждого файла, соглашения по стилю |
| [`docs/03_lvscript_spec.md`](docs/03_lvscript_spec.md) | Спецификация языка: лексика, приоритеты, выражения, классы, корутины, Result, песочница, stdlib, биндинги движка, памятка по `end` |
| [`docs/04_engine_systems.md`](docs/04_engine_systems.md) | API подсистем с примерами C++ и LV Script, рецепты, отладка |

---

## Архитектурные решения, которые стоит знать

**Локалы — в слотах кадра, стек — только для временных значений.** Это главное
отличие VM от «классической» стековой машины: `endScope()` освобождает слоты и
**не** выдаёт `Pop`, а `SetLocal` — peek-запись, требующая явного `Pop` там, где
значение не нужно. Подробности и история связанных дефектов — в спецификации,
§4.6.

**Единый контракт вызова.** Результат всегда ложится в `argsBase - 1` (ячейка
под аргументами), `top` становится `argsBase`. Контракту следуют все четыре
пути: `callClosureAt`, `callNative`, `callNativeMethod`, `callNativeFree`.
Расхождение на один слот приводит к тому, что следующий вызов принимает
предыдущий результат за callee (`attempt to call a Bool value`).

**Кто владеет позицией.** У динамического тела источник истины — тело
(`syncToECS` перезаписывает `Transform`), у кинематика — `Transform`
(`syncFromECS` обновляет и позицию тела, и его цель `desc.position`). Поэтому
`setPosition()` из скрипта двигает и тело через `teleportBody()`, а персонажи
под скриптовым управлением создаются кинематиками.

**Изменяемые значения по умолчанию в полях класса разделяются.** `var items = []`
вычисляется один раз — при определении класса. Компилятор предупреждает;
правильно создавать контейнер в `init()`.

---

## Разработка

```bash
bash build_tests.sh                       # все сьюты (g++ напрямую, без cmake)
cmake --build build -j && ctest --test-dir build --output-on-failure

# редактор без окна: печатает панели текстом (тот же код, что рисует ImGui)
cmake -B build-editor -G Ninja -DLV_BUILD_EDITOR=ON
cmake --build build-editor --target limvine-editor
./build-editor/limvine-editor --dump-panels templates/farming_iso --frames 60
```

Правила:

* новый код сопровождается проверкой в одном из self-test'ов;
* тест обязан ловить **регрессию поведения**, а не факт компиляции;
* опциональный бэкенд — только за макросом `LV_WITH_*` и с Null-реализацией;
* игровая логика шаблонов — только `.lvs` (C++ там появляться не должен).

> На машинах с 1–2 ГБ ОЗУ линковка тяжёлых сьютов с `-O2` может упасть без
> сообщения (collect2/ld). `build_tests.sh` и CMake используют для них `-O1`.
