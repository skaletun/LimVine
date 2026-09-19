/**
 * @file    Bytecode.h
 * @brief   Формат инструкций байткода LV Script и (де)серилизация .lvc.
 * @ingroup LVScript
 *
 * @details Формат инструкции — 4 байта, фиксированная ширина:
 *          @code
 *          [ opcode : 8 bit ][ operand : 24 bit ]
 *          @endcode
 *          Фиксированная ширина выбрана намеренно:
 *          - декодирование — это `op = *ip++`, без переключателей по длине;
 *          - ip всегда кратен 4 => выравнивание и предсказуемость branch predictor;
 *          - дизассемблер и hot-patch тривиальны.
 *
 *          Для 64-битных операндов (редко) используется пара инструкций.
 */
#pragma once

#include "Value.h"

namespace lv {

// Версия .lvc определена в Common.h (kBytecodeVersion = 0x0001'0000).
// Магическое число и endian-agnostic запись выполняет сериализатор.

/**
 * @brief Коды инструкций виртуальной машины.
 *
 * Порядок группировки подобран так, чтобы горячие инструкции (арифметика,
 * доступ к локалам, переходы) имели меньшие коды и лучше ложились в I-cache.
 */
enum class Op : Byte {
    // -- Манипуляции стеком --------------------------------------------------
    Dup,           ///<        дублировать вершину
    Dup1,          ///<        дублировать значение ПОД вершиной (receiver для вызова метода)
    IndexGetPeek,  ///<        [obj][key] -> [obj][key][old]  (чтение без потребления)
    IndexSetTop,   ///<        [obj][key][val] -> [val]  (val на вершине)
    Swap,          ///<        поменять местами две вершины
    PeekSlot,      ///< [s24]  push stack[s24] (доступ к слотам кадра, включая параметры)
    PokeSlot,      ///< [s24]  stack[s24] = pop()

    // -- Константы и литералы ----------------------------------------------
    Constant,      ///< [c24]  push constants[c24]
    Nil,           ///<        push nil
    True,          ///<        push true
    False,         ///<        push false
    IntConst,      ///< [i24]  push малое целое (sign-extended) — без пула констант
    EmptyArray,    ///<        push []
    EmptyMap,      ///<        push {}

    // -- Локалы / upvalue / глобалы ----------------------------------------
    GetLocal,      ///< [s24]  push locals[s24]
    SetLocal,      ///< [s24]  locals[s24] = pop()  (значение остаётся в стеке)
    GetUpvalue,    ///< [u24]
    SetUpvalue,    ///< [u24]
    GetGlobal,     ///< [n24]  push globals[names[n24]]
    DefineGlobal,  ///< [n24]  globals[names[n24]] = pop()
    SetGlobal,     ///< [n24]
    GetModule,     ///< [n24]  push modules[names[n24]]
    LoadName,      ///< [n24]  как GetGlobal, но nil вместо ошибки (отложенный поиск классов)

    // -- Арифметика / логика -------------------------------------------------
    Add, Sub, Mul, Div, Mod, Pow, Neg, Not,
    BitAnd, BitOr, BitXor, BitNot, Shl, Shr,
    Equal, NotEqual, Greater, Less, GreaterEq, LessEq,

    // -- Составные структуры -------------------------------------------------
    IndexGet,      ///<        push pop()[pop()]   (obj, key на стеке)
    IndexSet,      ///<        obj[key] = value
    MemberGet,     ///< [n24]  push pop().names[n24]
    MemberSet,     ///< [n24]  pop().names[n24] = pop()
    MemberGetOpt,  ///< [n24]  optional chaining: nil-safe
    ArrayPush,     ///<        arr.push(pop())
    ArraySetAt,    ///<        arr[i] = v
    MapInsert,     ///<        map[k] = v (k,v уже в стеке)
    NewArray,      ///< [n24]  собрать массив из n значений стека
    NewMap,        ///< [n24]  собрать map из n пар
    NewRange,      ///< [incl] создать range-итератор из (from, to) на стеке
    Slice,         ///<        push pop()[pop():pop()]
    Len,           ///<        push length(pop())
    IterInit,      ///<        превратить вершину в итератор
    IterNext,      ///< [o24]  следующий элемент; jump на выход, если исчерпан
    IterKeyNext,   ///< [o24]  как IterNext, но пушит пару (key, value)
    SetLoopSlots,  ///< [0|0|iterSlot8] записать слот итератора в кадр
    StorePair,     ///< [k12|v12] снять пару (key,value) и записать в два слота кадра

    // -- Вызовы --------------------------------------------------------------
    Call,          ///< [a8]   вызвать callable с a аргументами
    CallSpread,    ///< [a8]   как Call, но последний аргумент — массив (распаковка `f(a...)`)
    CallMethod,    ///< [a8 | n16] вызвать метод names[n16] с a аргументами
    InvokeSuper,   ///< [a8 | n16]
    PackRest,      ///< [n24]  упаковать n верхних значений в массив (variadic-параметр)
    Closure,       ///< [f24]  создать замыкание из прототипа f24 + upvalue (+ хвост: numUpv, пары (isLocal,idx))
    MakeCoroutine, ///<        превратить замыкание на вершине в объект-корутину
    Return,        ///<        возврат из функции
    ReturnNil,     ///<        быстрый возврат nil
    NewInstance,   ///< [a8]   создать инстанс: class = pop(), затем a аргументов init
    GetClass,      ///< [n24]
    GetThis,       ///<        push frame.thisValue
    GetSuper,      ///<        push frame.superValue
    GetSuperOf,    ///<        push superclass of the instance/class on top (runtime `super`)
    SpreadAppend,  ///<        распаковать массив с вершины в формируемый массив

    // -- Управление потоком --------------------------------------------------
    Jump,          ///< [o24]  безусловный (sign-extended, 4-байтные шаги)
    JumpIfFalse,   ///< [o24]  pop; jump if !truthy
    JumpIfTrue,    ///< [o24]
    JumpIfFalsePop,///< [o24]  не снимает значение (для and/or/??)
    JumpIfTruePop, ///< [o24]
    JumpIfNil,     ///< [o24]  переход, если вершина == nil (значение остаётся)
    Elvis,         ///< [o24]  `a ?: b`: если вершина истинна — оставить её, иначе снять и исполнить b
    Loop,          ///< [o24]  обратный переход
    Pop,           ///<        снять одно значение
    PopN,          ///< [n24]  снять n значений

    // -- Циклы ---------------------------------------------------------------
    ForRangePrep,  ///< [slot] подготовить числовой range-цикл (3 слота: i, end, step)
    ForRangeLoop,  ///< [o24]  инкремент + проверка; jump на выход
    ForIterPrep,   ///<        создать итератор из pop()
    ForIterNext,   ///< [o24]  следующий элемент или jump на выход

    // -- Корутины ------------------------------------------------------------
    Yield,         ///< [n24]  приостановить корутину, вернуть n значений
    Resume,        ///< [n24]  возобновить корутину
    Suspend,       ///<        запросить приостановку у native (wait/await)

    // -- Классы / match / прочее ---------------------------------------------
    Class,         ///< [n24]  объявить класс
    ClassSuper,    ///<        установить суперкласс (top-1)
    ClassMethod,   ///< [n24]  определить метод
    ClassField,    ///< [n24]  значение по умолчанию для поля (value = pop())
    StaticMethod,  ///< [n24]
    IsInstance,    ///< [c24]  проверка типа (снимает значение и типname)
    IsInstancePeek,///< [c24]  как IsInstance, но НЕ снимает проверяемое значение
    Cast,          ///< [c24]
    ResultOk,      ///<        wrap top into Ok(...)
    ResultErr,     ///<        wrap top into Err(...)
    ResultCheck,   ///< [o24]  если Err — выйти из функции с этим Result
    TryBegin,      ///< [o24]  точка восстановления при рантайм-ошибке
    TryEnd,        ///<        снять обработчик
    Throw,         ///<        сгенерировать ошибку из top
    Concat,        ///< [n24]  конкатенация n строк (интерполяция)
    Halt,          ///<        конец скрипта
    Breakpoint,    ///<        отладчик
    Nop
};

/// Мнемоника для дизассемблера.
[[nodiscard]] const char* opName(Op op) noexcept;
/// Длина инструкции в байтах (всегда 4 в текущем формате, кроме Closure).
[[nodiscard]] constexpr std::size_t opLength(Op) noexcept { return 4; }
/// True для инструкций с переменным хвостом (Closure: numUpvalues + пары (kind,index)).
[[nodiscard]] constexpr bool hasVariableTail(Op op) noexcept { return op == Op::Closure; }

/// Упаковка 24-битного операнда.
inline void emitOperand24(std::vector<Byte>& code, std::uint32_t v) {
    LV_ASSERT(v <= kMaxOperand24);
    code.push_back(static_cast<Byte>((v >> 16) & 0xFF));
    code.push_back(static_cast<Byte>((v >> 8) & 0xFF));
    code.push_back(static_cast<Byte>(v & 0xFF));
}

/// Чтение 24-битного операнда (big-endian).
inline std::uint32_t readOperand24(const Byte* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
            static_cast<std::uint32_t>(p[2]);
}

/// Чтение знакового 24-битного смещения перехода.
inline std::int32_t readSigned24(const Byte* p) noexcept {
    std::uint32_t u = readOperand24(p);
    if (u & 0x800000u) u |= 0xFF000000u;
    return static_cast<std::int32_t>(u);
}

/**
 * @brief Дизассемблер байткода.
 *
 * Используется консолью редактора (`:disasm`), профилировщиком и юнит-тестами
 * компилятора. Печатает смещение, строку исходника, мнемонику и операнд.
 */
class Disassembler {
public:
    /// Полная расшифровка прототипа (рекурсивно для вложенных).
    [[nodiscard]] static std::string disassemble(const ObjFunction& fn,
                                                const std::vector<ObjString*>* names = nullptr,
                                                bool recursive = true);
    /// Одна инструкция. Возвращает длину в байтах.
    [[nodiscard]] static std::size_t disassembleInstruction(const ObjFunction& fn, std::size_t offset,
                                                            std::string& out,
                                                            const std::vector<ObjString*>* names = nullptr);
};

// ---------------------------------------------------------------------------
//  Сериализация байткода (.lvc)
// ---------------------------------------------------------------------------

/**
 * @brief Контейнер скомпилированного модуля.
 *
 * Кэшируется на диске рядом с исходником (`.lvs` -> `.lvc`). При загрузке
 * проверяются @c kBytecodeVersion и хэш исходника; при несовпадении модуль
 * перекомпилируется. Это даёт мгновенный старт редактора на больших проектах.
 */
struct BytecodeModule {
    std::uint32_t version = 0;
    std::uint64_t sourceHash = 0;
    SourceName    name;
    ObjFunction*  entry = nullptr;   ///< Прототип верхнего уровня (владеет VM).
    std::vector<ObjFunction*> protos;///< Плоский список всех прототипов (для патчинга).

    [[nodiscard]] std::vector<Byte> serialize(VM& vm) const;
    /// Возвращает nullopt, если версия/хэш не совпали.
    [[nodiscard]] static std::optional<BytecodeModule> deserialize(VM& vm, std::span<const Byte> data);
    /// Вычисление FNV-1a хэша исходного текста (для кэширования).
    [[nodiscard]] static std::uint64_t sourceHashOf(std::string_view src);
};

} // namespace lv
