# LimVine — структура каталогов

```
limvine/
├── engine/                     # движок (C++20, без внешних зависимостей по умолчанию)
│   ├── core/                   # фундамент: математика и многопоточность
│   │   ├── Math.h              #   Vec2/3/4, Quat, Mat4, AABB, Ray, intersectRayAABB
│   │   ├── JobSystem.h/.cpp    #   пул потоков, parallelFor / parallelForRange
│   ├── ecs/                    # ядро данных
│   │   ├── Entity.h            #   Entity {index, generation}
│   │   ├── Component.h         #   концепт Component, TypeId, ComponentMetaRegistry
│   │   ├── Registry.h/.cpp     #   архетипный реестр: SoA-пулы, swap-remove, поколения
│   │   ├── CommandBuffer.h     #   отложенные структурные изменения
│   │   └── World.h/.cpp        #   фазы кадра и планировщик систем
│   ├── lvscript/               # LV Script — отдельная библиотека (не знает о движке)
│   │   ├── Common.h            #   lv::format, lv::Expected (замена std::expected)
│   │   ├── Lexer.h/.cpp        #   токены, интерполяция строк, мягкие ключевые слова
│   │   ├── Parser.h/.cpp       #   AST; блоки do..end, if-выражения, лямбды, match
│   │   ├── AST.h/.cpp          #   узлы выражений/операторов, Param, MethodDecl, FieldDecl
│   │   ├── Bytecode.h          #   Op, ObjFunction, Disassembler, контейнер .lvc
│   │   ├── Compiler.h/.cpp     #   AST → байткод; moduleGlobals_, области видимости
│   │   ├── Value.h/.cpp        #   NaN-boxing Value, Obj* (String/Array/Map/Closure/…)
│   │   ├── GC.h/.cpp           #   mark-and-sweep, pin, бюджет памяти
│   │   ├── VM.h/.cpp           #   интерпретатор, кадры, корутины, трейсбеки, песочница
│   │   ├── Stdlib.h/.cpp       #   print/len/range/int/float, Array/String/Map, math, Result
│   │   └── Scheduler.h/.cpp    #   wait/waitEvent, adopt корутин, тик по dt
│   ├── render/
│   │   ├── RenderTypes.h/.cpp  #   Vertex (64 Б), MeshData, MaterialParams, DrawItem,
│   │   │                       #   InstanceBatch, MergedChunk, Frustum
│   │   ├── Batcher.h/.cpp      #   frustum culling + GPU instancing
│   │   ├── GraphicsAPI.h/.cpp  #   интерфейс + NullGraphicsAPI (LV_WITH_VULKAN / _OPENGL)
│   │   └── Renderer.h/.cpp     #   пайплайн кадра, камера, отладочные линии
│   ├── physics/
│   │   └── Physics.h/.cpp      #   обёртка Jolt (LV_WITH_JOLT) + встроенный
│   │                             #   детерминированный симулятор (AABB, raycast,
│   │                             #   overlapSphere, фикс. шаг, матрица коллизий 16×16)
│   ├── input/
│   │   └── Input.h/.cpp        #   Input Mapping Contexts: стек контекстов с
│   │                             #   приоритетами, клавиши/мышь/геймпад как оси,
│   │                             #   мёртвая зона, сериализация привязок в JSON
│   ├── audio/
│   │   └── Audio.h/.cpp        #   интерфейс + NullAudioBackend (пул голосов,
│   │                             #   вытеснение по приоритету/дистанции, шины),
│   │                             #   AudioSource; OpenAL под LV_WITH_OPENAL
│   ├── asset/
│   │   └── AssetManager.h/.cpp #   конвейер ассетов, кэш (путь, mtime, хэш),
│   │                             #   FileWatcher (polling), каскад по зависимостям
│   ├── scripting/
│   │   ├── EngineBindings.h/.cpp  # ScriptWorld, BindingRegistry, биндинги
│   │   │                          # (spawn/setPosition/raycast/inputAxis/…), hotReload
│   │   ├── TemplateRunner.h/.cpp  # headless-запуск игровых шаблонов по template.json
│   │   └── visual/
│   │       ├── VisualScript.h/.cpp # транспилятор граф → LV Script, round-trip
│   │       └── JsonMini.h/.cpp     # мини-JSON парсер/сериализатор
│   └── editor/
│       ├── EditorUI.h/.cpp     #   абстракция immediate-mode UI (UIDraw) +
│       │                       #   TextUIDraw: рисование панели в строку (headless)
│       ├── Panels.h/.cpp       #   Hierarchy, Inspector, Asset Browser, Viewport,
│       │                       #   Console+REPL, EditorApp
│       ├── ImGuiBackend.h/.cpp #   UIDraw на Dear ImGui (только LV_WITH_IMGUI)
│       └── main.cpp            #   limvine-editor [--dump-panels] [--frames N] [template]
│
├── stdlib/                     # игровые модули LV Script (не язык, а контент)
│   ├── tween.lvs               #   easing-кривые + moveTo/rotateTo/pulse
│   ├── fsm.lvs                 #   StateMachine (состояния, условные переходы)
│   ├── pathfinding.lvs         #   BinaryHeap + GridNav (A*, октидная эвристика)
│   ├── inventory.lvs           #   ItemDef/ItemDatabase/Slot/Inventory
│   ├── dialogue.lvs            #   граф диалогов (choices/actions/next, флаги, require)
│   └── quest.lvs               #   Objective/Quest/QuestLog, цепочки nextQuest, journal()
│
├── templates/                  # игровые шаблоны (манифест + скрипты + ассеты)
│   ├── farming_iso/            #   изометрическая ферма
│   │   ├── template.json       #     stdlib-зависимости, порядок скриптов, ввод
│   │   ├── scripts/            #     config/world/crops/player/economy/main .lvs
│   │   └── assets/
│   ├── rpg_3p/                 #   RPG от третьего лица
│   │   └── scripts/            #     config/world/npc/dialogues/quests/player/combat/main
│   └── fps_shooter/            #   шутер от первого лица
│       └── scripts/            #     config/arena/weapons/enemy/pickups/main
│
├── tools/
│   ├── lvrun.cpp               # CLI: lvrun <template-dir|file.lvs> [--frames N] [--disasm] [--input]
│   └── lvcook.cpp              # «повар» контента: check / bundle / list по .lvs и шаблонам
│
├── tests/                      # self-test'ы (без внешнего тестового фреймворка)
│   ├── lvscript_tests.cpp      #   язык — 53 проверки
│   ├── ecs_tests.cpp           #   13
│   ├── render_tests.cpp        #   15
│   ├── input_tests.cpp         #   17
│   ├── job_tests.cpp           #   6
│   ├── visualscript_tests.cpp  #   31
│   ├── stdlib_tests.cpp        #   stdlib/*.lvs — 26
│   ├── engine_integration_tests.cpp # скрипт ↔ движок — 27
│   ├── editor_tests.cpp        #   панели редактора — 36
│   └── template_tests.cpp      #   три шаблона — 71
│
├── docs/
│   ├── 01_architecture.md      #   слои, ECS, конвейер LV Script, рендер, физика, jobs
│   ├── 02_directory_structure.md  # этот файл
│   ├── 03_lvscript_spec.md     #   спецификация языка + биндинги движка
│   └── 04_engine_systems.md    #   API подсистем и рецепты
│
├── CMakeLists.txt              # библиотека limvine, lvrun, limvine-editor, 10 тестов
├── build_tests.sh              # то же самое без cmake (g++ напрямую)
└── README.md                   # быстрый старт, состав движка, обзор языка и шаблонов
```

## Соглашения

* **`.h` + `.cpp` рядом**, один модуль — одна пара файлов; приватных каталогов
  `detail/` нет, чтобы путь включения всегда читался как `подсистема/Имя.h`.
* **Заголовки самодостаточны**: каждый компилируется отдельно (проверяется
  `-Wall -Wextra` на всех сьютах).
* **Имена**: `PascalCase` для типов, `camelCase` для функций и полей,
  `trailing_` для полей-членов класса, `kPrefix` для констант.
* **Док-комментарии** — Doxygen (`@brief`, `@details`). Внутри блочного
  комментария нельзя писать `*/` (закрывает комментарий) — это однажды сломало
  сборку `VisualScript.h`.
* **Опциональные бэкенды** всегда за макросом `LV_WITH_*`, и всегда есть
  Null-реализация: дерево обязано собираться и проходить тесты без единой
  внешней зависимости.
* **Шаблоны игр** не содержат C++: только `template.json` и `.lvs`. Новый
  шаблон = новый каталог, правка движка не требуется.

## Что читать первым

| Задача | Файл |
|---|---|
| Понять устройство в целом | `docs/01_architecture.md` |
| Писать игровую логику | `docs/03_lvscript_spec.md` |
| Разобраться в API подсистем | `docs/04_engine_systems.md` |
| Запустить шаблон прямо сейчас | `tools/lvrun.cpp`, `bash build_tests.sh` |
| Править VM/компилятор | `engine/lvscript/VM.cpp` (комментарии о контракте стека) |
