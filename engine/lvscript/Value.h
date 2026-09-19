/**
 * @file    Value.h
 * @brief   Представление значений LV Script: NaN-boxing и объектная модель.
 * @ingroup LVScript
 *
 * @details Каждое значение языка упаковано в 64 бита (NaN-boxing):
 *          - `double`            — если биты не попадают в маски тегов;
 *          - `int64`             — tag 0x1 (целые хранятся как IEEE-754 double,
 *                                  что даёт точность до 2^53 и бесплатный
 *                                  единый путь арифметики);
 *          - `bool` / `nil`      — tag 0x2;
 *          - `Obj*` (указатель)  — tag 0x3.
 *
 *          Такая упаковка означает, что стек VM — это плотный массив `uint64_t`
 *          (8 байт на слот), кэш-дружелюбный и не требующий tag-поля отдельно.
 *
 * @author  LimVine Engine Team
 */
#pragma once

#include "Common.h"

namespace lv {

struct GC;
class VM;

// ---------------------------------------------------------------------------
//  Прямые объявления объектных типов
// ---------------------------------------------------------------------------
struct ObjString;
struct ObjArray;
struct ObjMap;
struct ObjFunction;
struct ObjClosure;
struct ObjUpvalue;
struct ObjClass;
struct ObjInstance;
struct ObjCoroutine;
struct ObjNativeFn;
struct ObjResult;
struct ObjIterator;

/// Общий заголовок всех кучевых объектов. Первый член любого `Obj*`.
struct ObjHeader {
    enum class Type : std::uint8_t {
        String, Array, Map, Function, Closure, Upvalue, Class, Instance,
        Coroutine, NativeFn, Result, Iterator
    };

    Type          type;        ///< Динамический тип объекта.
    bool          marked : 1;  ///< Флаг трассировки GC (mark phase).
    bool          pinned : 1;  ///< Если true — GC никогда не освободит объект (интернированные строки, системные классы).
    std::uint32_t bytes;       ///< Размер объекта в байтах (для бюджетов песочницы и GC heuristics).
    ObjHeader*    next;        ///< Интрузивный список всех объектов кучи (владеет GC).

    ObjHeader(Type t, std::uint32_t sz) noexcept
        : type(t), marked(false), pinned(false), bytes(sz), next(nullptr) {}

    [[nodiscard]] const char* typeName() const noexcept;
};

// ---------------------------------------------------------------------------
//  NaN-boxed Value
// ---------------------------------------------------------------------------

/// Маски тегов NaN-boxing.
namespace detail {
constexpr std::uint64_t kQNaN  = 0x7ff8000000000000ULL;
constexpr std::uint64_t kSign  = 0x8000000000000000ULL;
constexpr std::uint64_t kTagInt   = 0x0001000000000000ULL; // kQNaN | 1
constexpr std::uint64_t kTagBool  = 0x0002000000000000ULL; // kQNaN | 2
constexpr std::uint64_t kTagNil   = 0x0004000000000000ULL; // kQNaN | 4
constexpr std::uint64_t kTagObj   = 0x0008000000000000ULL; // kQNaN | 8
constexpr std::uint64_t kTagMask  = 0x000f000000000000ULL;
constexpr std::uint64_t kPayload  = 0x0000ffffffffffffULL;
constexpr std::uint64_t kBoolBit  = 0x0000000000000001ULL;
} // namespace detail

/**
 * @brief Значение LV Script (8 байт, тривиально копируемое).
 *
 * @invariant Объект @c Value не владеет памятью: временем жизни кучевых объектов
 *            управляет @c lv::GC (см. GC.h). Поэтому @c Value безопасно хранить
 *            в стеке VM, в полях инстансов и в native-колбэках.
 */
class Value {
public:
    constexpr Value() noexcept : bits_(detail::kQNaN | detail::kTagNil) {}

    [[nodiscard]] static constexpr Value nil() noexcept { return Value(); }

    [[nodiscard]] static Value boolean(bool b) noexcept {
        Value v;
        v.bits_ = detail::kQNaN | detail::kTagBool | (b ? detail::kBoolBit : 0);
        return v;
    }

    [[nodiscard]] static Value integer(std::int64_t i) noexcept {
        return fromNumber(static_cast<double>(i));
    }

    /// @warning Целые вне диапазона точности double (±2^53) теряют младшие разряды.
    [[nodiscard]] static Value fromNumber(double d) noexcept {
        Value v;
        std::memcpy(&v.bits_, &d, sizeof(double));
        // Если получился NaN — принудительно канонизируем, чтобы не пересечься с тегами.
        if ((v.bits_ & detail::kQNaN) == detail::kQNaN && (v.bits_ & detail::kTagMask) != 0)
            v.bits_ = detail::kQNaN;
        return v;
    }

    [[nodiscard]] static Value object(ObjHeader* o) noexcept {
        Value v;
        v.bits_ = detail::kQNaN | detail::kTagObj |
                  (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(o)) & detail::kPayload);
        return v;
    }

    // -- Проверки типа ------------------------------------------------------
    [[nodiscard]] constexpr bool isNil()   const noexcept { return bits_ == (detail::kQNaN | detail::kTagNil); }
    [[nodiscard]] constexpr bool isBool()  const noexcept { return (bits_ & (detail::kQNaN | detail::kTagMask)) == (detail::kQNaN | detail::kTagBool); }
    [[nodiscard]] constexpr bool isInt()   const noexcept { return isNumber(); }
    [[nodiscard]] constexpr bool isNumber()const noexcept { return (bits_ & detail::kQNaN) != detail::kQNaN || (bits_ & detail::kTagMask) == 0; }
    [[nodiscard]] constexpr bool isObject()const noexcept { return (bits_ & (detail::kQNaN | detail::kTagMask)) == (detail::kQNaN | detail::kTagObj); }

    // -- Извлечение ---------------------------------------------------------
    [[nodiscard]] constexpr bool asBool() const noexcept { return (bits_ & detail::kBoolBit) != 0; }

    [[nodiscard]] double asNumber() const noexcept {
        double d;
        std::uint64_t b = bits_;
        // Сбрасываем возможные мусорные биты тегов для канонического NaN.
        std::memcpy(&d, &b, sizeof(double));
        return d;
    }

    [[nodiscard]] std::int64_t asInt() const noexcept { return static_cast<std::int64_t>(asNumber()); }

    [[nodiscard]] ObjHeader* asObject() const noexcept {
        return reinterpret_cast<ObjHeader*>(static_cast<std::uintptr_t>(bits_ & detail::kPayload));
    }

    template <class T> [[nodiscard]] T* as() const noexcept { return static_cast<T*>(asObject()); }

    /// Приведение к `bool` по правилам языка: только `nil` и `false` ложны.
    [[nodiscard]] constexpr bool truthy() const noexcept {
        return !isNil() && !(isBool() && !asBool());
    }

    /// Побитовое сравнение (для чисел — численное). Строки/объекты сравниваются
    /// через @c VM::valuesEqual, поэтому здесь только «быстрый путь».
    [[nodiscard]] constexpr bool bitwiseEquals(const Value& o) const noexcept { return bits_ == o.bits_; }

    [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return bits_; }
    [[nodiscard]] static constexpr Value fromRaw(std::uint64_t b) noexcept {
        Value v; v.bits_ = b; return v;
    }

    /// Хэш значения (для map-ключей). Объекты хэшируются по идентичности,
    /// строки — по содержимому.
    [[nodiscard]] std::uint64_t hash() const noexcept;

    /// Человекочитаемое представление (для REPL и print).
    [[nodiscard]] std::string toString() const;
    /// Интроспективное представление (строки в кавычках, поля объектов и т.д.).
    [[nodiscard]] std::string inspect() const;
    /// Имя типа для сообщений об ошибках.
    [[nodiscard]] const char* typeName() const noexcept;

private:
    std::uint64_t bits_;
};

static_assert(sizeof(Value) == 8, "Value must be NaN-boxed into 8 bytes");

// ---------------------------------------------------------------------------
//  Объектная модель
// ---------------------------------------------------------------------------

/**
 * @brief Интернированная неизменяемая строка.
 *
 * Строки интернируются в @c VM (см. @c VM::internString), поэтому равенство
 * строк — это сравнение указателей, а хэш считается один раз при создании.
 */
struct ObjString : ObjHeader {
    std::uint64_t  hash = 0; ///< Кэш FNV-1a.
    std::uint32_t  length = 0; ///< Длина в байтах (без '\0').
    char           data[1]{};  ///< Гибкий массив символов + завершающий '\0'.

    explicit ObjString(ObjHeader h) : ObjHeader(h) {}

    [[nodiscard]] std::string_view view() const noexcept { return {data, length}; }
};

/// Динамический массив (`[1, 2, 3]`). Ёмность округляется до степени двойки.
struct ObjArray : ObjHeader {
    std::uint32_t count = 0;
    std::uint32_t capacity = 0;
    Value*        items = nullptr; ///< Память выделяется GC-аллокатором и трассируется.

    explicit ObjArray(ObjHeader h) : ObjHeader(h) {}

    [[nodiscard]] std::span<Value> span() noexcept { return {items, count}; }
    void grow(GC& gc, std::uint32_t needed);
};

/// Открытая адресация для ObjMap (load factor <= 0.75).
struct MapEntry {
    Value key;
    Value value;
    std::uint64_t hash : 63;
    bool          used : 1;
};

/// Ассоциативный массив (`{"hp": 100}`) и основа таблицы полей инстанса.
struct ObjMap : ObjHeader {
    std::uint32_t count = 0;
    std::uint32_t capacity = 0; ///< Всегда степень двойки; 0 = пуст.
    MapEntry*     entries = nullptr;

    explicit ObjMap(ObjHeader h) : ObjHeader(h) {}

    Value*  find(Value key);
    Value*  findString(std::string_view name, std::uint64_t hash);
    bool    set(GC& gc, Value key, Value value);
    bool    remove(Value key);
    void    resize(GC& gc, std::uint32_t newCapacity);
};

/// Прототип функции: результат компиляции одного `func`/лямбды/метода.
struct ObjFunction : ObjHeader {
    enum class Kind : std::uint8_t { Script, Method, Initializer, Lambda, Native };

    Kind                   kind = Kind::Script;
    ObjString*             name = nullptr;
    std::uint32_t          arity = 0;       ///< Число обязательных параметров.
    std::uint32_t          totalParams = 0; ///< Вместе с параметрами по умолчанию.
    std::int32_t           restParamSlot = -1; ///< Слот variadic-параметра (`args...`), -1 = нет.
    std::uint32_t          numUpvalues = 0;
    std::uint32_t          numLocals = 0;   ///< Параметры + локалы (слоты стека кадра).
    std::uint32_t          maxStack = 0;    ///< Макс. глубина временных значений.
    std::vector<Byte>      code;            ///< Байткод.
    std::vector<Value>     constants;       ///< Пул констант.
    std::vector<ObjFunction*> nested;       ///< Вложенные прототипы (для рекомпиляции/hot-reload).
    std::vector<std::uint32_t> lineStarts;  ///< Отладочная таблица: строка каждой точки байткода.
    std::vector<std::uint32_t> defaultSlots;///< Инструкции-заполнители для параметров по умолчанию.
    SourceName             source;
    std::uint32_t          protoId = 0;     ///< Идентификатор для hot-reload патчинга.

    explicit ObjFunction(ObjHeader h) : ObjHeader(h) {}

    [[nodiscard]] std::uint32_t lineAt(std::size_t ip) const noexcept;
};

/// Замыкание: прототип + массив захваченных upvalue.
struct ObjClosure : ObjHeader {
    ObjFunction*  function = nullptr;
    ObjUpvalue**  upvalues = nullptr; ///< Длина = function->numUpvalues.

    explicit ObjClosure(ObjHeader h) : ObjHeader(h) {}

    [[nodiscard]] ObjFunction* proto() const noexcept { return function; }
};

/// Открытый upvalue: пока переменная жива в стеке — `location` указывает на слот;
/// после возврата кадра значение «закрывается» в `closed`.
struct ObjUpvalue : ObjHeader {
    Value*      location = nullptr;
    Value       closed;
    /// Следующий открытый upvalue в списке VM (для переиспользования одного слота).
    /// @note Имя намеренно отличается от `ObjHeader::next`, чтобы не затенять его:
    ///       затенение ломает интрузивный список GC.
    ObjUpvalue* nextOpen = nullptr;

    explicit ObjUpvalue(ObjHeader h) : ObjHeader(h) {}
};

/// Класс: имя, суперкласс, таблица методов и optional `initializer`.
struct ObjClass : ObjHeader {
    ObjString* name = nullptr;
    ObjClass*  superclass = nullptr;
    ObjMap*    methods;       ///< key: ObjString* (имя метода), value: ObjClosure*.
    ObjMap*    defaults;      ///< Значения полей по умолчанию (применяются в NewInstance).
    ObjClosure* initializer = nullptr;
    std::uint32_t classId = 0;///< Для быстрого `is` и inline-кэшей.
    std::vector<ObjString*> fieldNames; ///< Порядок объявления полей (для inspect/сериализации).

    explicit ObjClass(ObjHeader h) : ObjHeader(h) {}
};

/// Инстанс класса. Поля хранятся в map — это делает hot-reload тривиальным:
/// добавление поля в скрипте не инвалидирует существующие объекты.
struct ObjInstance : ObjHeader {
    ObjClass* klass = nullptr;
    ObjMap*   fields = nullptr;

    explicit ObjInstance(ObjHeader h) : ObjHeader(h) {}
};

/**
 * @brief Корутина (fiber). Имеет собственный стек значений и кадр вызовов.
 *
 * @details Реализация «симметричных» корутин: @c resume передаёт управление внутрь,
 *          @c yield возвращает его назад вместе со значениями. Планировщик движка
 *          (см. Scheduler.h) хранит корутины, ожидающие времени/события, и
 *          возобновляет их в нужном кадре.
 */

// ---------------------------------------------------------------------------
//  Кадр вызова и контекст исполнения
// ---------------------------------------------------------------------------
/**
 * @brief Кадр вызова стековой VM.
 *
 * @invariant @c base — индекс в @c ExecutionContext::values, а не указатель:
 *            стек значений является @c std::vector и может реаллоцироваться.
 *            Хранение индекса устраняет целый класс багов с висячими указателями.
 */
struct CallFrame {
    struct ObjClosure* closure = nullptr;
    std::size_t  ip        = 0;   ///< Смещение в байтах от начала `closure->function->code`.
    std::uint32_t base     = 0;   ///< Индекс первого слота кадра (локалы) в стеке значений.
    std::uint32_t tempBase = 0;   ///< base + numLocals: начало области временных.
    std::uint32_t numLocals = 0;
    Value        thisValue;       ///< `this` для методов.
    Value        superValue;      ///< `super` для вызовов базового класса.
    std::uint32_t calleeSlot = 0; ///< Куда doReturn пишет результат (обычно base-1).
    std::uint32_t iterSlot = 0;   ///< Слот итератора (for-in).
    std::uint32_t varSlot  = 0;   ///< Слот переменной цикла.
    std::uint32_t varSlot2 = 0;   ///< Слот второй переменной цикла (`for k, v in ...`).
};

/**
 * @brief Изолированный контекст исполнения: основной поток скрипта или корутина.
 *
 * Каждая корутина владеет собственным стеком и массивом кадров, поэтому
 * переключение — это обмен двух указателей, без ucontext/asm и без привязки к ABI.
 */
struct ExecutionContext {
    std::vector<Value>     values;
    std::vector<CallFrame> frames;
    std::size_t            frameCount = 0;
    std::size_t            top = 0;             ///< Индекс вершины стека значений.
    struct ObjCoroutine*   coroutine = nullptr; ///< nullptr для основного контекста.
    /// Контекст-владелец для вспомогательных контекстов нативных колбэков
    /// (map/filter/sort...). При возврате из последнего кадра VM передаёт
    /// результат владельцу и завершает run() — иначе интерпретатор продолжил бы
    /// исполнять байткод вызывающей функции.
    ExecutionContext*      returnTo = nullptr;

    /// Выделить стек значений и массив кадров (один раз на контекст).
    void reserve(std::size_t valueSlots = kValueStackCapacity, std::size_t frameSlots = kMaxCallFrames) {
        values.resize(valueSlots, Value::nil());
        frames.resize(frameSlots);
    }

    [[nodiscard]] Value& peek(std::size_t n = 0) noexcept { return values[top - 1 - n]; }
    void push(Value v) { values[top++] = v; }
    Value pop() { return values[--top]; }
};

/// Доступ к i-му локалу кадра (слоты кадра живут в общем стеке значений).
inline Value& frame_local(ExecutionContext* ctx, const CallFrame& f, std::uint32_t i) noexcept {
    return ctx->values[f.base + i];
}

struct ObjCoroutine : ObjHeader {
    enum class State : std::uint8_t { Created, Running, Suspended, Dead, Error };

    State             state = State::Created;
    ObjClosure*       root;                 ///< Тело корутины.
    std::vector<Value> stack;               ///< Собственный стек значений (дубль для совместимости).
    std::size_t       frameCount = 0;
    Value             resumeValue;          ///< Значение, переданное последним resume.
    Value             yieldValue;           ///< Значение, возвращённое последним yield.
    double            waitUntil = 0.0;      ///< Для `wait(sec)`: время в секундах игры.
    ObjString*        waitEvent = nullptr;  ///< Для `waitEvent(name)`
    ExecutionContext* parent = nullptr;     ///< Контекст, вызвавший resume (для возврата управления).
    ExecutionContext* returnTo = nullptr;   ///< Куда вернуть управление после yield/смерти.
    std::size_t       savedIp = 0;          ///< ip родителя на момент resume.
    std::size_t       savedTop = 0;         ///< top родителя на момент resume.
    std::string       errorMessage;
    ExecutionContext* ownerContext = nullptr; ///< Собственный контекст исполнения корутины.

    explicit ObjCoroutine(ObjHeader h) : ObjHeader(h) {}
};

/// Нативная функция (биндинг C++). Может быть «yielding» — тогда она имеет право
/// приостановить текущую корутину (используется для `wait`, `awaitAsset` и т.п.).
struct ObjNativeFn : ObjHeader {
    using Fn = std::function<Value(VM&, std::span<const Value>)>;

    ObjString*   name = nullptr;
    std::uint32_t arity;        ///< -1 (0xFFFFFFFF) = variadic.
    Fn           fn;
    bool         yielding = false;
    std::uint32_t requiredCapability = 0; ///< Битовая маска capability песочницы.

    explicit ObjNativeFn(ObjHeader h) : ObjHeader(h) {}
};

/// `Result<T, E>` — лёгкая обработка ошибок без исключений.
struct ObjResult : ObjHeader {
    bool  ok = false;
    Value value;   ///< При ok == true.
    Value error;   ///< При ok == false.

    explicit ObjResult(ObjHeader h) : ObjHeader(h) {}
};

/// Итератор для `for x in ...` (range, string, map, генератор).
struct ObjIterator : ObjHeader {
    enum class Kind : std::uint8_t { Range, Array, String, Map, Generator };
    Kind      kind = Kind::Range;
    Value     source;   ///< ObjArray* / ObjString* / ObjMap*
    ObjClosure* generator = nullptr;
    std::int64_t index = 0;
    std::int64_t step  = 1;
    std::int64_t end   = 0;
    bool        done = false;
    std::size_t mapCursor = 0;

    explicit ObjIterator(ObjHeader h) : ObjHeader(h) {}
};

// ---------------------------------------------------------------------------
//  Вспомогательные функции
// ---------------------------------------------------------------------------

/// Создание строки (копирует данные; интернирование выполняет VM).
[[nodiscard]] ObjString* makeRawString(GC& gc, std::string_view sv, std::uint64_t hash);

/// Создание пустого массива.
[[nodiscard]] ObjArray* makeArray(GC& gc, std::uint32_t capacity = 0);

/// Создание пустой map.
[[nodiscard]] ObjMap* makeMap(GC& gc);

/// Создание класса.
[[nodiscard]] ObjClass* makeClass(GC& gc, ObjString* name);

/// Создание инстанса.
[[nodiscard]] ObjInstance* makeInstance(GC& gc, ObjClass* klass);

/// Создание Result.
[[nodiscard]] ObjResult* makeResult(GC& gc, bool ok, Value v, Value e);

/// Глубокое структурное сравнение значений (строки по содержимому,
/// массивы/map — по элементам, остальные объекты — по идентичности).
[[nodiscard]] bool valuesEqual(const Value& a, const Value& b);

} // namespace lv
