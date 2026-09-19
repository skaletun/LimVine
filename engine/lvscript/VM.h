/**
 * @file    VM.h
 * @brief   Стековая виртуальная машина LV Script.
 * @ingroup LVScript
 *
 * @details Ключевые решения:
 *
 *  1. **Один 4-байтовый формат инструкции** — декодирование без переключателей
 *     по длине, ip всегда выровнен.
 *  2. **CallFrame хранит базу как индекс**, а не указатель: стек значений — это
 *     @c std::vector, который может реаллоцироваться. Это устраняет целый класс
 *     багов с висячими указателями.
 *  3. **ExecutionContext на корутину**: каждая корутина владеет собственным
 *     стеком и массивом кадров. Переключение = обмен двух указателей,
 *     никаких ucontext/asm — переносимо и быстро.
 *  4. **GC-корни предоставляются VM** через @c GC::RootProvider: стек, кадры,
 *     глобалы, интерн-таблица, планировщик корутин.
 *  5. **Inline-кеши доступа к полям** (@c FieldIC) — по одному на точку
 *     MemberGet/MemberSet; превращают hash-lookup в проверку classId.
 *  6. **Песочница**: топливный счётчик инструкций, лимит памяти, лимит глубины
 *     вызова, capability-маска для нативных функций и ограничение импорта.
 */
#pragma once

#include "Bytecode.h"
#include "GC.h"

#include <deque>
#include <unordered_set>

namespace lv {

class Scheduler;
class Compiler;

// ---------------------------------------------------------------------------
//  Ошибки
// ---------------------------------------------------------------------------

/**
 * @brief Рантайм-ошибка скрипта.
 *
 * Лёгкая (без backtrace-поля): трейсбек собирается по запросу из кадров VM.
 */
struct ScriptError {
    std::string message;
    SourceName  source;
    std::uint32_t line = 0;

    [[nodiscard]] std::string toString() const { return format("{}:{}: runtime error: {}", source, line, message); }
};

/// Результат выполнения скрипта.
enum class RunStatus : std::uint8_t { Ok, RuntimeError, CompileError, Yielded, Aborted, OutOfFuel };

// ---------------------------------------------------------------------------
//  Inline-кеш полей
// ---------------------------------------------------------------------------

/**
 * @brief Однослотовый inline-кеш для @c Op::MemberGet / @c Op::MemberSet.
 *
 * @details Low-poly игры оперируют сотнями однотипных сущностей, поэтому
 *          полиморфизм в точках доступа к полям почти всегда мономорфен.
 *          Кеш превращает «hash по имени + probe» в «сравнить classId».
 *          Мисскейш — медленный путь с обновлением кеша.
 */
struct FieldIC {
    std::uint32_t cachedClassId = 0;
    std::uint32_t slot = 0;       ///< Индекс в плотном массиве полей (future) / маркер.
    bool          valid = false;
};

// ---------------------------------------------------------------------------
//  Настройки песочницы
// ---------------------------------------------------------------------------

/**
 * @brief Битовые флаги возможностей (capabilities) для изоляции модов.
 *
 * Нативная функция declares @c requiredCapability; VM отказывает в вызове,
 * если флаг не выставлен. Это позволяет запускать сторонние .lvs без доступа
 * к файловой системе, сети или разрушающим операциям над сценой.
 */
enum Capability : std::uint32_t {
    Cap_None        = 0,
    Cap_Math        = 1u << 0,  ///< Математика, строки, коллекции. Всегда разрешено.
    Cap_Scene       = 1u << 1,  ///< Чтение/изменение ECS-сцены.
    Cap_Spawn       = 1u << 2,  ///< Создание/уничтожение сущностей.
    Cap_Physics     = 1u << 3,  ///< Raycast, изменение rigid body.
    Cap_Audio       = 1u << 4,  ///< Воспроизведение звука.
    Cap_Input       = 1u << 5,  ///< Подписка на ввод.
    Cap_IO          = 1u << 6,  ///< Файлы/сохранения.
    Cap_Network     = 1u << 7,  ///< Сеть (по умолчанию выключено).
    Cap_Reflection  = 1u << 8,  ///< Доступ к метаданным классов движка.
    Cap_Debug       = 1u << 9,  ///< print/inspect/disasm/profiler.
    Cap_Coroutines  = 1u << 10, ///< Корутины и планировщик.
    Cap_All         = 0xFFFFFFFFu,
    /// Профиль по умолчанию для доверенных игровых скриптов.
    Cap_GameDefault = Cap_Math | Cap_Scene | Cap_Spawn | Cap_Physics | Cap_Audio |
                      Cap_Input | Cap_Coroutines | Cap_Debug | Cap_Reflection,
    /// Профиль для сторонних модов.
    Cap_ModSandbox  = Cap_Math | Cap_Scene | Cap_Audio | Cap_Coroutines
};

/**
 * @brief Конфигурация песочницы.
 */
struct SandboxConfig {
    std::uint32_t capabilities = Cap_GameDefault;
    std::int64_t  fuelPerCall  = 200'000'000; ///< Лимит инструкций на один вызов `execute`.
    std::size_t   memoryLimit  = 512ull * 1024 * 1024;
    std::int32_t  maxCallDepth = 512;
    bool          allowImport  = true;
    std::vector<std::string> allowedImports;  ///< Пусто => разрешены все зарегистрированные модули.
    bool          deterministic = false;      ///< Фиксированный seed и запрет wall-clock времени.
    std::uint64_t randomSeed   = 0x9E3779B97F4A7C15ull;
};

// ---------------------------------------------------------------------------
//  Профилировщик
// ---------------------------------------------------------------------------

/// Агрегированная статистика исполнения (для панели Profiler в редакторе).
struct VMProfile {
    std::uint64_t instructions = 0;
    std::uint64_t calls        = 0;
    std::uint64_t returns      = 0;
    std::uint64_t gcPauses     = 0;
    double        gcTimeMs     = 0.0;
    std::uint64_t icHits       = 0;
    std::uint64_t icMisses     = 0;
};

// ---------------------------------------------------------------------------
//  VM
// ---------------------------------------------------------------------------

/**
 * @brief Виртуальная машина LV Script.
 *
 * Один экземпляр на «скриптовый мир». В движке обычно существует два мира:
 *  - основной (игровая логика, полный набор capabilities);
 *  - песочница модов (ограниченные capabilities и жёсткий топливный лимит).
 *
 * @thread_safety Не потокобезопасна. Для многопоточной логики создавайте по VM
 *                на worker-поток и синхронизируйте доступ к ECS через команды
 *                (см. @c lv::ecs::CommandBuffer).
 */
class VM final : public GC::RootProvider {
public:
    explicit VM(SandboxConfig sandbox = {});
    ~VM() override;

    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    // -- Инициализация -------------------------------------------------------
    /// Зарегистрировать стандартную библиотеку (math/string/array/…).
    void installStdlib();
    /// Подключить планировщик корутин (обычно делает @c lv::ScriptWorld движка).
    void attachScheduler(Scheduler* s) noexcept { scheduler_ = s; }
    [[nodiscard]] Scheduler* scheduler() const noexcept { return scheduler_; }

    // -- Выполнение ----------------------------------------------------------
    /**
     * @brief Выполнить прототип верхнего уровня модуля.
     * @param fn    Прототип, полученный от @c Compiler.
     * @param args  Аргументы (обычно пусты).
     * @return Статус и, при успехе, значение на вершине стека.
     */
    RunStatus execute(ObjFunction* fn, std::span<const Value> args = {});

    /// Вызвать замыкание/нативную функцию из C++ «с нуля» (сбрасывает состояние VM).
    RunStatus callValue(Value callable, std::span<const Value> args, Value* out = nullptr);

    /**
     * @brief Вызвать скриптовую функцию из нативного кода, не разрушая текущий кадр.
     *
     * Используется встроенными функциями высшего порядка (`map`, `filter`, `sort`, ...).
     * Исполнение происходит в основном контексте VM, после чего состояние восстанавливается.
     * @warning Не вызывать из тела корутины: контекст корутины на время приостанавливается.
     */
    RunStatus callFunction(Value fn, const std::vector<Value>& args, Value* out = nullptr);

    /**
     * @brief Вывод строки из `print`.
     *
     * По умолчанию пишет в stdout; редактор переопределяет sink в консоль LV Script,
     * сервер — в лог-файл. Такой уровень косвенности позволяет перенаправлять
     * вывод модов в песочнице.
     */
    using LogSink = std::function<void(std::string)>;
    void setLogSink(LogSink s) { logSink_ = std::move(s); }
    void logLine(std::string text);

    /// Публичная точка генерации рантайм-ошибки (для нативных функций).
    void runtimeErrorPublic(std::string msg) { runtimeError(std::move(msg)); }

    /// Вызвать метод по имени (используется при dispatch'е событий движка).
    RunStatus callMethod(Value receiver, std::string_view method, std::span<const Value> args, Value* out = nullptr);

    /// Подготовить корутину к возобновлению значением @c v и переключить контекст.
    /// Исполнение продолжается следующим вызовом @c run()/@c tickRun().
    RunStatus resumeCoroutine(ObjCoroutine* co, Value v, Value* out = nullptr);

    /// Продолжить исполнение текущего контекста (используется планировщиком).
    RunStatus tickRun();

    // -- Глобальное окружение -------------------------------------------------
    void   setGlobal(std::string_view name, Value v);
    [[nodiscard]] Value* findGlobal(std::string_view name);
    [[nodiscard]] Value  getGlobal(std::string_view name);
    void   defineGlobal(std::string_view name, Value v) { setGlobal(name, v); }
    /// Удалить глобал (используется hot-reload для полной переустановки модуля).
    bool   removeGlobal(std::string_view name);
    [[nodiscard]] ObjMap* globals() noexcept { return globals_; }

    // -- Нативные функции ------------------------------------------------------
    /**
     * @brief Зарегистрировать нативную функцию.
     * @param name    Имя в скрипте.
     * @param arity   Число аргументов; 0xFFFFFFFF = variadic.
     * @param fn      Реализация.
     * @param cap     Требуемая capability (проверяется песочницей).
     * @param yielding Может ли функция приостанавливать корутину.
     */
    void registerNative(std::string_view name, std::uint32_t arity, ObjNativeFn::Fn fn,
                        std::uint32_t cap = Cap_Math, bool yielding = false);

    /// Зарегистрировать нативный метод типа (например `Array.push`).
    void registerMethod(std::string_view typeName, std::string_view methodName, std::uint32_t arity,
                        ObjNativeFn::Fn fn, std::uint32_t cap = Cap_Math);

    /// Таблица модулей (результаты `import`).
    void setModule(std::string_view name, Value v);
    [[nodiscard]] Value* findModule(std::string_view name);

    // -- Интернирование и фабрики ---------------------------------------------
    [[nodiscard]] Value internString(std::string_view s);
    [[nodiscard]] ObjString* internRaw(std::string_view s);
    [[nodiscard]] Value makeArray(std::uint32_t capacity = 0);
    [[nodiscard]] Value makeMap();
    [[nodiscard]] Value makeNumber(double d) const noexcept { return Value::fromNumber(d); }
    [[nodiscard]] Value makeInt(std::int64_t i) const noexcept { return Value::integer(i); }
    [[nodiscard]] Value makeBool(bool b) const noexcept { return Value::boolean(b); }
    [[nodiscard]] Value makeResult(bool ok, Value v, Value e);
    [[nodiscard]] Value makeClosure(ObjFunction* fn);
    [[nodiscard]] Value makeCoroutine(ObjClosure* root);
    [[nodiscard]] ObjClass* declareClass(std::string_view name, ObjClass* super = nullptr);

    // -- GC --------------------------------------------------------------------
    [[nodiscard]] GC& gc() noexcept { return gc_; }
    void collectGarbage() { gc_.collect(); }

    // -- Песочница -------------------------------------------------------------
    [[nodiscard]] const SandboxConfig& sandbox() const noexcept { return sandbox_; }
    void setSandbox(const SandboxConfig& s);
    [[nodiscard]] bool hasCapability(std::uint32_t cap) const noexcept {
        return (sandbox_.capabilities & cap) == cap;
    }
    void requireCapability(std::uint32_t cap, std::string_view what);

    /// Топливный счётчик: уменьшается каждой инструкцией, 0 => OutOfFuel.
    void setFuel(std::int64_t f) noexcept { fuel_ = f; }
    [[nodiscard]] std::int64_t fuel() const noexcept { return fuel_; }

    // -- Ошибки и трейсбек ------------------------------------------------------
    [[nodiscard]] const ScriptError& lastError() const noexcept { return error_; }
    [[nodiscard]] std::string traceback() const;
    void clearError() noexcept { error_ = {}; }

    /// Пользовательский обработчик ошибок (логирование в консоль редактора).
    using ErrorHandler = std::function<void(const ScriptError&, std::string_view traceback)>;
    void setErrorHandler(ErrorHandler h) { errorHandler_ = std::move(h); }

    // -- Имена и отладка ---------------------------------------------------------
    [[nodiscard]] const std::vector<ObjString*>& names() const noexcept { return names_; }
    [[nodiscard]] std::uint32_t internName(const std::string& s);
    [[nodiscard]] VMProfile& profile() noexcept { return profile_; }
    void enableProfiling(bool e) noexcept { profiling_ = e; }

    /// Запрос приостановки корутины из нативной функции.
    void requestSuspend(double seconds = 0.0, ObjString* event = nullptr) noexcept {
        suspendRequested_ = true;
        suspendSeconds_ = seconds;
        suspendEvent_ = event;
    }
    [[nodiscard]] bool suspendRequested() const noexcept { return suspendRequested_; }

    /// Виртуальное время (секунды). Управляется планировщиком; в детерминированном
    /// режиме песочницы wall-clock недоступен.
    void   setGameTime(double t) noexcept { gameTime_ = t; }
    [[nodiscard]] double gameTime() const noexcept { return gameTime_; }

    /// Случайное число (детерминированный PRNG песочницы).
    [[nodiscard]] std::uint64_t nextRandom() noexcept;

    // -- Корни GC ----------------------------------------------------------------
    void markRoots(GC& gc) override;

    /// Текущий контекст исполнения (для нативных функций и планировщика).
    [[nodiscard]] ExecutionContext& context() noexcept { return *ctx_; }
    void pushContext(ExecutionContext& c) noexcept { ctx_ = &c; }

    /// Проверка типа значения (`is` / `IsInstance`).
    [[nodiscard]] bool isTypeOf(const Value& v, const std::string& typeName);
    /// Применение значений полей по умолчанию при создании инстанса.
    void applyFieldDefaults(ObjInstance* inst, ObjClass* cls);

    /// Все живые контексты (для трассировки GC).
    [[nodiscard]] std::vector<ExecutionContext*>& contexts() noexcept { return contexts_; }

    /// Хост-объект: произвольные данные движка, привязанные к VM (SceneHandle и т.п.).
    void  setHostData(void* p) noexcept { hostData_ = p; }
    void* hostData() const noexcept { return hostData_; }

private:
    // -- Внутренности выполнения --------------------------------------------------
    /// Цикл интерпретатора с преобразованием исключений в RunStatus::RuntimeError.
    RunStatus run();
    /// Собственно диспетчер инструкций; может бросить (bad_alloc из GC).
    RunStatus runUnsafe();
    void runtimeError(std::string msg);
    void resetStack();
    ExecutionContext* mainCtx_ = nullptr;
    LogSink logSink_;

    /**
     * @brief Войти в скриптовую функцию. Раскладка стека: `[callee][a0..aN-1]`.
     *
     * base нового кадра = argsBase (первый аргумент), поэтому Return корректно
     * снимает и callee, и аргументы. Слот callee остаётся «ниже» кадра и
     * перезаписывается результатом.
     */
    bool callClosure(ObjClosure* closure, int argCount);

    /**
     * @brief То же, но с явным указанием базы кадра.
     *
     * Нужно для ВЫЗОВА МЕТОДОВ, где раскладка `[receiver][dup][a0..aN-1]`:
     * dup-слот одновременно является callee-слотом, а receiver обязан остаться
     * ниже базы. При обычной формуле (argsBase = top - argc) первым «аргументом»
     * оказался бы dup, и параметр метода превратился бы в Function.
     */
    bool callClosureAt(ObjClosure* closure, int argCount, std::size_t argsBase,
                       std::size_t calleeSlot = static_cast<std::size_t>(-1));
    bool callNative(ObjNativeFn* native, int argCount);
    /// Вызов встроенного метода: раскладка `[receiver][a0..aN-1]` превращается
    /// в span `{receiver, a0..aN-1}` и кладётся ровно одно значение-результат.
    bool callNativeMethod(ObjNativeFn* native, int argCount);

    /**
     * @brief Вызов нативной функции, лежащей В ПОЛЕ объекта (модуль, таблица колбэков).
     *
     * `math.min(3, 1)`: приёмник здесь — просто контейнер, а не `this`, поэтому
     * функция вызывается БЕЗ неявного первого аргумента (в отличие от
     * callNativeMethod). Контракт стека тот же, что у вызова метода:
     * `[receiver][a0..aN-1][callee]` -> `[result]` на месте receiver'а.
     */
    bool callNativeFree(ObjNativeFn* native, int argCount);
    /// Обработка приостановки корутины (wait/await). true => continue цикла.
    bool handleSuspend(CallFrame*& framePtr, ObjFunction*& fn, const Byte*& code);
    /// Раскрутка стека до ближайшего try-обработчика. true => ошибка перехвачена.
    bool handleTryError(CallFrame*& framePtr, ObjFunction*& fn, const Byte*& code);
    void popTry();
    bool invokeMethod(Value receiver, ObjString* name, int argCount);
    bool invokeBuiltinMethod(ObjHeader* obj, ObjString* name, int argCount);
    void closeUpvalues(std::uint32_t fromIndex);
    bool doReturn();

    // -- Поиск методов -------------------------------------------------------------
    static ObjClosure* findMethod(ObjClass* klass, ObjString* name);

    // -- Данные --------------------------------------------------------------------
    GC                 gc_;
    SandboxConfig      sandbox_;
    ObjMap*            globals_   = nullptr;
    ObjMap*            modules_   = nullptr;
    ObjMap*            natives_   = nullptr;   ///< name -> ObjNativeFn
    ObjMap*            methodTables_ = nullptr;///< typeName -> ObjMap(name -> ObjNativeFn)
    std::vector<ObjString*> names_;
    std::unordered_map<std::uint64_t, ObjString*> interned_;

    std::vector<std::unique_ptr<ExecutionContext>> ownedContexts_;
    /// Пул вспомогательных контекстов для нативных колбэков (map/filter/sort...).
    std::vector<std::unique_ptr<ExecutionContext>> ownedHelpers_;
    std::vector<ExecutionContext*> contexts_;
    ExecutionContext*  ctx_ = nullptr;
    ExecutionContext   main_;

    ObjUpvalue*        openUpvalues_ = nullptr;
    std::vector<FieldIC> fieldICs_;
    Scheduler*         scheduler_ = nullptr;

    ScriptError        error_;
    ErrorHandler       errorHandler_;
    VMProfile          profile_{};
    bool               profiling_ = false;
    std::int64_t       fuel_ = std::numeric_limits<std::int64_t>::max();

    bool               suspendRequested_ = false;
    double             suspendSeconds_ = 0.0;
    ObjString*         suspendEvent_ = nullptr;
    double             gameTime_ = 0.0;
    std::uint64_t      rngState_ = 0x9E3779B97F4A7C15ull;

    std::vector<Value>   tryHandlers_;     ///< Стек try-обработчиков (целевой ip).
    std::vector<ExecutionContext*> tryCtx_;
    std::vector<std::size_t> tryFrameDepth_;
    std::vector<std::size_t> tryStackDepth_;
    Value                pendingInstanceOverride_;  ///< Инстанс, подставляемый после возврата init.
    /// Глубина стека кадров, на которой нужно применить pendingInstanceOverride_.
    /// Без этого замена произошла бы ДО выполнения init (сразу после push кадра).
    std::size_t          pendingOverrideFrameDepth_ = 0;
    void*                hostData_ = nullptr;

    /// Временные буферы (избегаем аллокаций в горячем пути).
    std::vector<Value> scratchArgs_;
    std::vector<std::pair<Value, Value>> scratchPairs_;
    std::string scratchStr_;
};

// ---------------------------------------------------------------------------
//  Маршалинг C++ <-> LV Script
// ---------------------------------------------------------------------------

/**
 * @brief Конвертация значений между C++ и LV Script.
 *
 * Специализируется для типов движка (Vec3, Entity, Color, ...) в соответствующих
 * модулях; базовые специализации для скаляров и строк заданы здесь.
 * @see Bindings.h — декларативный API поверх этой примитивной механики.
 */
template <class T> struct Marshal;

template <> struct Marshal<bool> {
    static Value to(VM&, bool v) { return Value::boolean(v); }
    static bool  from(VM&, const Value& v) { return v.truthy(); }
};
template <> struct Marshal<std::int64_t> {
    static Value to(VM&, std::int64_t v) { return Value::integer(v); }
    static std::int64_t from(VM&, const Value& v) { return v.asInt(); }
};
template <> struct Marshal<int> {
    static Value to(VM&, int v) { return Value::integer(v); }
    static int from(VM&, const Value& v) { return static_cast<int>(v.asInt()); }
};
template <> struct Marshal<std::uint32_t> {
    static Value to(VM&, std::uint32_t v) { return Value::integer(v); }
    static std::uint32_t from(VM&, const Value& v) { return static_cast<std::uint32_t>(v.asInt()); }
};
template <> struct Marshal<float> {
    static Value to(VM&, float v) { return Value::fromNumber(v); }
    static float from(VM&, const Value& v) { return static_cast<float>(v.asNumber()); }
};
template <> struct Marshal<double> {
    static Value to(VM&, double v) { return Value::fromNumber(v); }
    static double from(VM&, const Value& v) { return v.asNumber(); }
};
template <> struct Marshal<std::string> {
    static Value to(VM& vm, const std::string& v) { return vm.internString(v); }
    static std::string from(VM&, const Value& v) { return v.toString(); }
};
template <> struct Marshal<std::string_view> {
    static Value to(VM& vm, std::string_view v) { return vm.internString(v); }
};
template <> struct Marshal<Value> {
    static Value to(VM&, const Value& v) { return v; }
    static Value from(VM&, const Value& v) { return v; }
};

} // namespace lv
