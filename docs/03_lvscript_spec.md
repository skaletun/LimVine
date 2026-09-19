# LV Script — спецификация языка

> Версия байткода: `kBytecodeVersion` (см. `engine/lvscript/Bytecode.h`).
> Реализация: `engine/lvscript/` (Lexer → Parser → Compiler → стековая VM с GC).
> Все примеры в этом документе исполняются тестами `tests/lvscript_tests.cpp`,
> `tests/stdlib_tests.cpp` и `tests/template_tests.cpp` — спецификация описывает
> **фактическое** поведение, а не намерения.

---

## 1. Цели языка

LV Script — скриптовый язык игровой логики LimVine. Проектные решения:

| Требование | Как решено |
|---|---|
| Детерминизм и «песочница» для модов | Capability-модель (`Cap_Spawn`, `Cap_Scene`, `Cap_Input`, `Cap_Physics`, `Cap_Audio`, `Cap_Math`), лимит топлива (`fuelPerCall`), лимит памяти, белый список `import` |
| Нулевая стоимость абстракций в кадре | Компиляция в байткод, NaN-boxing значений, отсутствие подсчёта ссылок (mark-and-sweep GC), векторные типы передаются как map и арифметика делается нативными функциями |
| Плавное движение и долгие игровые процессы | Корутины + планировщик (`wait`, `waitFrames`, `waitUntil`, `waitEvent`) |
| Круг «дизайнер правит скрипт → видит результат» | Горячая перезагрузка `.lvs` с сохранением состояния инстансов (`ScriptWorld::hotReload`) |
| Генерация кода из Visual Scripting |Round-trip: граф → LV Script → граф (метаданные `#@node`) |

Язык намеренно похож на Lua/Kotlin там, где это снижает порог входа: блоки
`do ... end`, `let`/`var`, `if` как выражение, лямбды `|x| expr`.

---

## 2. Лексика

### 2.1 Комментарии

```lua
# однострочный комментарий
/// doc-комментарий (используется редактором для подсказок)
/** блочный doc-комментарий */
```

### 2.2 Литералы

| Литерал | Пример | Тип |
|---|---|---|
| Целое | `42`, `-7` | Int |
| Вещественное | `3.5`, `1e-3` | Float |
| Строка | `"hi"`, `'hi'` | String |
| Интерполяция | `"pos = {p.x}, len {len(arr)}"` | String |
| Булев | `true`, `false` | Bool |
| nil | `nil` | Nil |
| Массив | `[1, 2, 3]` | Array |
| Map | `{"hp": 100, "name": "wolf"}` | Map |
| Лямбда | `\|x\| x * 2`, `\|\| { doWork() }` | Function |
| Range | `0..10` (искл.), `0..=10` (включ.) | Range |

**Ограничение интерполяции (важно).** Внутри `{...}` нельзя использовать
вложенные фигурные скобки и кавычки того же типа, что у строки:

```lua
# НЕ компилируется:
print("[day {s["day"]}] gold {economy.gold}")

# Компилируется: раскладываем значения в локалы
let d = s["day"]
let gold = economy.gold
print("[day {d}] gold {gold}")
```

### 2.3 Ключевые слова

Жёсткие: `let var func coroutine class new if elif else while for in do end
return break continue and or not nil true false import as match is global static
pass yield resume try this super`.

**Мягкие** ключевые слова (`is`, `as`, `in`, `do`, `end`, `when`, `global`,
`static`, `pass`, `try`, `and`, `or`, `not`) допускаются как имена полей,
параметров, методов и ключей map — см. `Parser::isSoftKeyword`.

### 2.4 Приоритеты операторов

Чем больше число, тем выше приоритет (таблица из `Parser::binInfoFor`):

| Приоритет | Операторы |
|---|---|
| 4 | `or`, `\|\|` (в позиции выражения — логическое «или») |
| 6 | `and`, `&&` |
| 8 | `==`, `!=` |
| 9 | `<`, `>`, `<=`, `>=`, `is` |
| 10 / 11 / 12 | `\|` / `^` / `&` (побитовые) |
| 13 | `<<`, `>>` |
| 14 | `..`, `..=` (range, неассоциативен) |
| 15 | `+`, `-` |
| 16 | `*`, `/`, `%` |
| 18 | `**` (правоассоциативно) |
| унарные | `-x`, `not x`, `~x`, `!x` |
| постфикс | `a.b`, `a[i]`, `f(x)`, `a?.b` |

> **Историческая ловушка.** `parseTernary` одно время вызывал
> `parseBinary(minPrec = 5)`, из-за чего `or` (приоритет 4) не разбирался
> **вообще**: `if a or b do ...` падал с «expected 'do' after if condition,
> found 'or'», а `let x = a or b` превращал `or` в имя переменной. Сейчас
> `parseTernary` начинает с `minPrec = 0`.

Составное присваивание: `+= -= *= /= %= **=`.

---

## 3. Выражения

### 3.1 Тернарный и элвис

```lua
let step = if diagonal do 1.41421356 else 1.0 end   # if-выражение
let name = user.nickname ?: "anonymous"             # элвис (Op::Elvis, без повторного вычисления)
```

`if`-выражение всегда возвращает значение: ветвь без `else` даёт `nil`.

### 3.2 `while` и `for` как выражения

```lua
let last = while i < n do      # значение = последнее вычисленное в теле
    i += 1
end

let squares = [for x in items do x * x]   # list comprehension -> Array
```

Аккумулятор `while`-выражения живёт в **скрытом локале** `<whileAcc>`, а не в
стеке, поэтому `break`/`continue` не требуют Swap/Pop и не ломают баланс.

### 3.3 Лямбды

```lua
let sq  = |x| x * x                    # неявный return
let sum = |a, b| { let s = a + b
                   s }                 # блочное тело: значение = последнее выражение
let tick = || { onTick() }             # без аргументов, блочное тело
items.map(|x| x * 2).filter(|x| x > 4) # цепочки
```

Правила тел:

* `|x| expr` — неявный `return expr`;
* `|x| => expr` — то же с явной стрелкой;
* `|x| { stmts }` — блок, **последнее выражение становится результатом**
  (`Parser::makeLastExprImplicitReturn`);
* `|x| do stmts end` — блок, значение не возвращается (нужен явный `return`).

> **Историческая ловушка.** `|| { ... }` (лексема `PipePipe`) разбиралась как
> «короткое тело-выражение», и `{` уходил в литерал map'а: запись
> `{"enter": || { onEnter() }}` падала с «expected ':' in map literal».
> Сейчас у `||` есть полноценная блочная ветка.

### 3.4 Циклы

```lua
for i in 0..10 do ... end              # 0..9 (числовой цикл без итератора)
for i in 0..=10 do ... end             # 0..10
for item in collection do ... end      # массив/map/строка/range
for k, v in map do ... end             # две переменные (Op::StorePair)
while cond do ... end
```

`break`/`continue` поддерживают метки: `outer: for ... do break outer end`.

> **Историческая ловушка.** В for-in `continueIp` одно время указывал на
> `loopIp + 4` — то есть **в тело цикла, мимо итератора**. Итератор не
> продвигался, а `SetLocal` снимал со стека значение, которого там не было:
> каждый `continue` сдвигал стек на −1, и временные значения начинали писать
> поверх локалов кадра. Сейчас `continueIp == loopIp` (на `IterNext`).

### 3.5 `match`

```lua
let label = match value {
    0            => "zero",
    n: Int if n > 100 => "big",
    [a, b]       => "pair",
    {"hp": h}    => "has hp",
    _            => "other"
}
```

Субъект `match` живёт в скрытом локале `<matchSubject>`; паттерн-связывание
читает его **дубликат** (`PeekSlot`), поэтому все ветви потребляют ровно одно
значение и баланс стека между ветвями сходится.

---

## 4. Операторы

### 4.1 Объявления

```lua
let immutable = 1          # повторное присваивание — ошибка компиляции
var mutable = 1
global shared = 1          # явный глобал
```

**Объявления верхнего уровня модуля — всегда глобалы** (`Compiler::moduleGlobals_`),
даже если физически занимают слот кадра прототипа `<script>`. Иначе замыкания
захватывали бы слот кадра, который умирает сразу после исполнения модуля.

### 4.2 Функции и корутины

```lua
func greet(name, greeting = "Hello") -> String do
    return "{greeting}, {name}!"
end

func sum(args...) do               # variadic-хвост
    var t = 0
    for a in args do t += a end
    return t
end

coroutine grow() do
    print("planted")
    yield 0.5
    wait(2.0)                      # секунды реального времени
    print("sprouted")
end
grow()                             # объявление корутины создаёт ПЕРВИЧНУЮ корутину:
                                   # вызов немедленно стартует её и возвращает первый yield
```

Параметры по умолчанию проверяются **прологом** функции:

```
PeekSlot i ; JumpIfTrue ->skip ; <default> ; PokeSlot i ; skip:
```

`Op::JumpIfTrue` **снимает** проверяемое значение, поэтому дополнительного
`Pop` после него быть не должно.

> **Историческая ловушка.** Лишний `Pop` уводил вершину стека **ниже базы
> кадра**, и следующее значение по умолчанию записывалось в соседний параметр:
> `new Slot()` оставлял в поле `count` мусор (`<native print>`), что выглядело
> как «cannot compare String with Int» в `stdlib/inventory`.

### 4.3 Классы

```lua
class Entity do
    var name = "entity"            # значение по умолчанию поля
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

let p = new Player("hero", 100, 7)
print(p.describe())                # hero(hp=100) lvl=7
print(p is Entity, p is Player)    # true  true
```

**Инициализатор всегда возвращает инстанс.** Правило покадровое: `doReturn`
проверяет `ObjFunction::Kind::Initializer` и подставляет `frame.thisValue`.

> **Историческая ловушка.** Прежний механизм был глобальным
> (`pendingInstanceOverride_` + глубина стека кадров) и ломался на **вложенных
> конструкторах**: `new Inventory()` внутри своего `init` вызывал `new Slot()`,
> подмена результата внутреннего init «съедала» флаг, и внешний
> `new Inventory(...)` возвращал `nil`.

**Изменяемые значения по умолчанию полей разделяются между инстансами.**
`var items = []` вычисляется **один раз** — при определении класса — и копия
*ссылки* попадает в каждый инстанс:

```lua
# ПЛОХО: все квесты разделят один массив целей
class Quest do
    var objectives = []
end

# ХОРОШО: контейнер создаётся на каждый инстанс
class Quest do
    var objectives                 # объявление без значения
    func init() do
        this.objectives = []
    end
end
```

Компилятор выдаёт предупреждение `field '...' is initialised with a shared
mutable default` для массивов и map-литералов в полях класса.

### 4.4 Вызов метода: раскладка стека

Единый контракт вызова (одинаков для свободных функций, методов, нативных
функций и нативных методов):

```
стек до:    [callee/приёмник ...][a0..aN-1]
стек после: [result] на месте callee-слота, top = (слот результата) + 1
слот результата ВСЕГДА = argsBase - 1   (ячейка непосредственно под аргументами)
```

Раскладки:

| Вызов | Раскладка | argsBase | Слот результата |
|---|---|---|---|
| `f(a, b)` | `[callee][a][b]` | top−2 | callee-слот |
| `o.m(a, b)` | `[receiver][a][b][callee]` | recvPos+1 | recvPos |

Для метода callee-слот создаёт `Op::Dup1` (копия приёмника **из-под**
аргументов), а база кадра = `recvPos + 1`, поэтому аргументы становятся
локалами «на месте» и первый параметр не затирается замыканием метода.

> Любое отклонение от этого правила сдвигает стек вызывающей функции на
> единицу, и **следующий** вызов принимает результат предыдущего за callee:
> «attempt to call a Bool value», «value of type Array is not callable».

### 4.5 Присваивание по индексу и по полю

```
Op::IndexGet     [obj][key]        -> [value]
Op::IndexGetPeek [obj][key]        -> [obj][key][value]   (операнды не потребляются)
Op::IndexSet     [val][obj][key]   -> [val]
Op::IndexSetTop  [obj][key][val]   -> [val]
Op::MemberSet    [obj][val]        -> [val]
```

Составное присваивание `obj[key] op= rhs` использует `IndexGetPeek` +
`IndexSetTop`: obj и key вычисляются **ровно один раз** (иначе `f()[g()] += 1`
вызвал бы побочные эффекты дважды) и всё время лежат в стеке под временными
значениями.

> **Историческая ловушка.** При порядке снятия `[key][obj][val]` составное
> присваивание давало `[obj][key][new]`, и `IndexSet` принимал новое значение
> за ключ, ключ — за объект, а объект — за значение: `p["respawnIn"] -= dt`
> падал с «string index out of range» (индексация строки числом 12.0).

### 4.6 Локалы и стек

**Локалы живут в слотах кадра** `[base, base+numLocals)`, стек значений
начинается выше (`tempBase`) и служит только временным операндам. Отсюда два
правила, которые обязан соблюдать компилятор:

1. `Op::SetLocal` — **peek-запись**: значение копируется в слот и **остаётся**
   в стеке. Там, где оно не нужно (объявления `let`/`var`, переменная цикла,
   итератор, `import`, `func`/`class` внутри функции), компилятор обязан
   выдать явный `Op::Pop`.
2. `Compiler::endScope()` освобождает **только слоты** локалов и **не трогает
   стек**.

> **Исторические ловушки.**
> * Без явного `Pop` каждый `let` внутри функции протекал одним слотом; в
>   длинных функциях (`stdlib/pathfinding` — 25 локалов) временные значения
>   начинали писать поверх локалов, что выглядело как «cannot index a Int».
> * `endScope()` раньше выдавал `Pop`/`PopN` «по числу закрытых локалов» —
>   наследие стековой модели Crafting Interpreters, где локалы и есть стек.
>   Эти `Pop` съедали настоящие временные значения: внутри циклов портились
>   callee/аргументы следующего вызова, и уже на второй итерации
>   `let c = "{a},{i}"` читал мусор (`-9.2e18`, «съехавшие» строки лога).

### 4.7 `import`

```lua
import math                     # встроенный модуль движка (VM::setModule)
import "mods/mylib.lvs"         # путь (проверяется белым списком песочницы)
import physics as ph            # псевдоним
```

`import` связывает модуль как глобал (на уровне модуля) или локал (внутри
функции). Модуль — обычный map, поэтому `math.min(...)` является **вызовом
функции-поля**: приёмник не передаётся неявным аргументом
(`VM::callNativeFree` / `callClosureAt`).

---

## 5. Синтаксис блоков: памятка по `end`

| Конструкция | Закрытие |
|---|---|
| `if c do A end` | один `end` |
| `if c do A else B end` | один `end` (блочный `else` без `do`) |
| `if c do A else B` | короткий `else <expr>` — `end` необязателен |
| `if c do A elif d do B else C end` | один `end` на всю цепочку |
| `if c do A else if d do B end end` | **два** `end`: у вложенного `if` свой |
| `while c do ... end`, `for ... do ... end` | один `end` |
| `func/coroutine ... do ... end` | один `end` |
| `class ... do ... end` | один `end` |
| `\|x\| { ... }` | `}` |

---

## 6. Корутины и планировщик

```lua
coroutine pulse(entity, seconds: Float, times: Int) do
    var n = 0
    while n < times do
        setField(entity, "Transform", "scale", vec3(1.15, 1.15, 1.15))
        yield n                       # вернуть значение и приостановиться
        wait(seconds * 0.5)           # сон в секундах (планировщик)
        setField(entity, "Transform", "scale", vec3(1, 1, 1))
        wait(seconds * 0.5)
        n += 1
    end
end
```

Функции ожидания: `wait(sec)`, `waitFrames(n)`, `waitUntil(pred)`,
`waitEvent(name)`. Планировщик (`lv::Scheduler`) тикает из
`ScriptWorld::tick(dt)` — поэтому корутины работают и в headless-тестах.

**Метод-коротина класса — один объект на класс.** `compileClass` выдаёт
`Op::MakeCoroutine` сразу после прототипа, то есть `this.bump()` возвращает
*ту же самую* корутину для всех инстансов и всех вызовов: одновременная
анимация двух объектов перезапускала бы одну корутину. Для покадровых
анимаций в шаблонах используется keyframe-подход (состояние в данных +
проход в `update(dt)`) — см. `templates/farming_iso/scripts/crops.lvs`.

---

## 7. Обработка ошибок и Result

```lua
let r: Result = try parseInt(input)
match r {
    Ok(v)  => print("parsed {v}")
    Err(e) => print("failed: {e}")
}

func readConfig(path) -> Result do
    let txt = readFile(path)?         # оператор `?`: Err сразу возвращается наружу
    return Ok(parse(txt))
end
```

* `try <expr>` → `Result` (`Op::TryBegin/TryEnd`, `ResultOk/ResultErr`);
* `expr?` — распространение ошибки;
* `Result` имеет методы `ok()`, `value()`, `error()`, `unwrap()`, `orElse(fallback)`;
* `Ok(x)` / `Err(msg)` — конструкторы-глобалы.

Внутри песочницы любая ошибка рантайма превращается в `Err`, а не рвёт
выполнение хоста: трейсбек доступен через `VM::traceback()`.

---

## 8. Песочница (моды)

```cpp
SandboxConfig cfg;
cfg.allowImport   = true;
cfg.allowedImports = {"math", "string"};   // пустой список = разрешено всё
cfg.fuelPerCall   = 2'000'000;             // бюджет инструкций на вызов
cfg.maxCallDepth  = 256;
cfg.memoryBudget  = 64 << 20;
cfg.capabilities  = Cap_Math | Cap_Scene;  // БЕЗ Cap_Spawn
```

Проверка capability выполняется **в самом биндинге**, а не в игровом коде:

```lua
let stolen = spawn()
# -> runtime error: sandbox violation: 'spawn' is not available in this context
```

---

## 9. Стандартная библиотека языка

### 9.1 Глобальные функции

`print(...)`, `str(v)`, `type(v)`, `len(v)`, `inspect(v)`,
`int(v)`, `float(v)`, `num(v)`, `range(from, to, step?)`,
`min/max/abs/clamp/lerp`, `assert(cond, msg)`, `error(msg)`,
`coroutine(fn)`, `wait/waitFrames/waitUntil/waitEvent`, `Ok/Err/Result`.

> `int()`/`float()` **нормализуют** значение: строка парсится, `bool → 0/1`,
> `nil → 0`. Применять `Value::asInt()` к нечислу напрямую нельзя — это сырая
> переинтерпретация битов NaN-боксинга (`int("3")` возвращал `-9.2e18`).

### 9.2 Методы встроенных типов

| Тип | Методы |
|---|---|
| Array | `push pop insert removeAt get set len count contains indexOf find map filter reduce sort sum all any forEach join slice reverse clear shuffle` |
| String | `len upper lower trim split replace contains startsWith find sub format` |
| Map | `get set has remove keys values len forEach` |
| Result | `ok value error unwrap orElse` |
| Coroutine | `status dead` |

### 9.3 Модуль `math`

Константы `pi tau e inf`; функции `sqrt sin cos tan asin acos atan atan2 pow
exp log floor ceil round sign min max abs clamp lerp random randomInt deg rad`.

### 9.4 Игровые модули (`stdlib/*.lvs`)

| Модуль | Содержимое |
|---|---|
| `tween.lvs` | easing-кривые (`easeInOutQuad`, `easeOutElastic`, …), `Tween.number/moveTo/rotateTo/pulse` |
| `fsm.lvs` | `StateMachine`: `state(name, {enter, exit, update})`, `transition(from, to, when)`, `changeState`, `update(dt)`, `elapsed`, `isIn` |
| `pathfinding.lvs` | `BinaryHeap`, `GridNav` (A* по сетке, октидная эвристика, запрет срезания углов, `pathLength`) |
| `inventory.lvs` | `ItemDef`, `ItemDatabase`, `Slot`, `Inventory` (стеки, вес, категории, `serialize`/`deserialize`) |
| `dialogue.lvs` | `Dialogue`: граф узлов (`speaker/text/choices/actions/next`), флаги, условия `require`, история |
| `quest.lvs` | `Objective`, `Quest`, `QuestLog`: цели-данные, `notify(kind, target, n)`, цепочки `nextQuest`, `journal()` |

---

## 10. Биндинги движка

### 10.1 Сущности и компоненты

Сущность в скрипте — map `{__entity: true, index, generation}`: handle'ы
сериализуемы, сравниваемы и переживают горячую перезагрузку.

```lua
let e = spawn()                                   # создать (+ Transform)
setPosition(e, vec3(0, 1, 0))
translate(e, vec3(0.1, 0, 0))
print(getPosition(e))                             # {"x":..,"y":..,"z":..}
setRotationEuler(e, vec3(0, 90, 0))
print(hasComponent(e, "Transform"))
setField(e, "MeshRenderer", "tint", color(1, 0, 0, 1))
let hp = getField(e, "Health", "current")
destroy(e);  print(alive(e), entityCount())
```

Компоненты, доступные из скриптов (`registerEngineComponents`):
`Transform` (`position`, `rotation`, `scale`), `MeshRenderer` (`tint`),
`RigidBody`, `AudioSource`, `InputState`.

> `Vec3`/`Quat`/`Color` передаются как **map**, а арифметика делается
> нативными функциями (`vec3Add`, `vec3Mul`, …): в кадре бывают тысячи
> векторов, и аллокация скриптового класса на каждый была бы недопустима.

### 10.2 Ввод

```lua
createInputContext("Gameplay", 0)
mapKeyAxis("Gameplay", "MoveX", 65, -1.0)   # A
mapKeyAxis("Gameplay", "MoveX", 68,  1.0)   # D
mapKey("Gameplay", "Jump", 32)              # Space
mapMouseButton("Gameplay", "Fire", 0)       # ЛКМ
pushInputContext("Gameplay")

if inputPressed("Jump") do jump() end
let move = inputAxis2D("MoveX", "MoveY")    # -> {"x":..,"y":0,"z":..}
let look = mouseDelta()
```

Контексты образуют **стек приоритетов**: `pushInputContext("Dialogue", 10)`
перехватывает действия у контекста gameplay, `popInputContext("Dialogue")`
возвращает управление.

### 10.3 Физика

```lua
# kind: 0 = Static, 1 = Kinematic, 2 = Dynamic
addRigidBody(entity, vec3(0.5, 0.9, 0.5), 1)
setVelocity(entity, vec3(0, 0, 5))
applyImpulse(entity, vec3(0, 6, 0))
let hit = raycast(from, dir, 50.0)
if hit["hit"] do print(hit["entity"], hit["distance"], hit["position"], hit["normal"]) end
```

**Кто владеет позицией.** У *динамического* тела источником истины является
тело: `PhysicsWorld::syncToECS()` каждый кадр перезаписывает `Transform`, а
`syncFromECS()` игнорирует `Transform`. Поэтому:

* персонажи, которыми управляет скрипт, в шаблонах создаются **кинематиками**
  (`kind = 1`) — у них `Transform` является источником истины;
* биндинг `setPosition()`/`translate()` всё равно двигает тело через
  `PhysicsWorld::teleportBody()`, иначе телепорт динамического тела
  отменялся бы на следующем кадре.

### 10.4 Звук и время

```lua
let voice = playSound2D(clipId, volume)
playSound3D(clipId, position, volume)
stopSound(voice)

let dt = deltaTime();  let t = time();  let f = frameIndex()
```

### 10.5 Сцены и сохранения

```lua
let e = spawn()
setName(e, "Chest")                       # имя хранится в ECS-компоненте Name
print(getName(e))                         # "Chest"

saveScene("saves/slot1.lvscene")          # атомарная запись (tmp + rename)

clearScene()                              # уничтожить все сущности
let r = loadScene("saves/slot1.lvscene")
if r.ok do
    print("сущностей: {r.entities}, предупреждений: {r.warnings}")
else
    print("ошибка: {r.error}")
end
```

| Функция | Право | Описание |
|---|---|---|
| `saveScene(path) -> Bool` | `Cap_IO` | сохранить мир в `.lvscene` |
| `loadScene(path) -> Map` | `Cap_IO` | догрузить сцену **поверх** текущей; поля `ok`, `entities`, `warnings`, `error` |
| `clearScene()` | `Cap_Spawn` | уничтожить все сущности (системы остаются) |
| `setName(e, s)` / `getName(e)` | `Cap_Scene` | имя сущности |

> `saveScene`/`loadScene` требуют **`Cap_IO`**, а не `Cap_Scene`: это запись на
> диск по произвольному пути. В профиле `Cap_ModSandbox` такого права нет,
> поэтому мод не может ни перезаписать сейв игрока, ни подменить уровень.

Неизвестные компоненты и поля при загрузке не роняют сцену — они попадают в
счётчик `warnings` и в лог консоли. Это позволяет старому билду открывать
сцены, сохранённые более новой версией движка.

### 10.6 Глобальные обработчики кадра

`ScriptWorld::tick(dt)` тикает планировщик и вызывает (если они определены)
глобальные функции `update(dt)` и `fixedUpdate(dt)`. Соглашение перечислено в
`ScriptWorld::dispatch`.

---

## 11. Горячая перезагрузка

```cpp
scripts.hotReload("game.lvs", newSource);
```

Подменяется **тело прототипа** (`ObjFunction`), а сам объект прототипа
сохраняется: существующие замыкания и инстансы продолжают ссылаться на него,
поэтому состояние (поля объектов, upvalue, счётчики) переживает перезагрузку.
Файловые ассеты перезагружает `AssetManager` (polling-`FileWatcher`, кэш по
`(путь, mtime, хэш)`, каскад по зависимостям).

---

## 12. Взаимодействие с C++

```cpp
lv::Value* fn = vm.findGlobal("templateState");
lv::Value out;
vm.callValue(*fn, {}, &out);                       // вызов глобальной функции
vm.callMethod(instance, "interact", args, &out);   // вызов метода
```

`VM::callMethod` сам раскладывает стек как `[receiver][args][callee]` и
устанавливает `thisValue` кадра. Для шаблонов есть обёртки
`TemplateRunner::callGlobalFn` / `callMethodOnGlobal` и хелпер `valueToMap()`,
который разворачивает скриптовый map в `unordered_map<string,string>` — так
тесты сравнивают строки вместо работы с NaN-боксингом.

---

## 13. Соглашения по стилю

* Отступ 4 пробела; блоки только `do ... end` (не фигурные скобки) — кроме тел
  лямбд и литералов.
* Один оператор на строку; `;` допустим как разделитель, но не обязателен.
* Игровые атрибуты (HP, стадии роста, цены) держим **в скрипте** (map/поля
  инстанса), а не в C++-компонентах: их можно править и перезагружать на лету.
* Конфигурация шаблона — отдельный `config.lvs`, который грузится первым
  (объявления верхнего уровня модуля являются глобалами).
* Тестовые хуки шаблона (`testTill`, `testFirePrecise`, …) — тонкие обёртки над
  настоящей логикой, а не отдельная «тестовая» реализация.
