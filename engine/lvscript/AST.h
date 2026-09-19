/**
 * @file    AST.h
 * @brief   Абстрактное синтаксическое дерево LV Script.
 * @ingroup LVScript
 *
 * @details Узлы представлены как «широкие» структуры @c Expr / @c Stmt / @c Decl
 *          с тегом @c kind. Такой дизайн выбран сознательно:
 *          - один аллокатор на узел (нет виртуальных деструкторов в горячем пути);
 *          - обход через @c Visitor — предсказуемый и понятный;
 *          - компилятор и тайп-чекер делят одно представление.
 *
 *          Все узлы живут в @c Arena (bump-аллокатор), поэтому парсинг сцены из
 *          тысячи скриптов не фрагментирует кучу.
 */
#pragma once

#include "Common.h"

namespace lv {

// ---------------------------------------------------------------------------
//  Типы
// ---------------------------------------------------------------------------

/**
 * @brief Синтаксическое представление аннотации типа.
 *
 * `let hp: Int = 100`, `func f(v: [Vec3]) -> Map<String, Float>`, `let t?: Transform`
 */
struct TypeExpr {
    enum class Kind : std::uint8_t {
        Infer,   ///< Тип выводится (аннотация отсутствует).
        Named,   ///< Int, Float, Bool, String, Entity, Vec3, MyComponent...
        Array,   ///< [T]
        Map,     ///< Map<K, V>
        Func,    ///< (A, B) -> C
        Optional,///< T?
        Tuple,   ///< (A, B)
        Any,     ///< динамический тип (эскейп-люк; в строгом режиме — warning)
        Self,    ///< тип текущего класса
        Nil      ///< литеральный тип nil
    };
    Kind kind = Kind::Infer;
    std::string name;                                   ///< Для Named.
    std::vector<std::unique_ptr<TypeExpr>> params;      ///< [T] / Map<K,V> / аргументы Func.
    std::unique_ptr<TypeExpr> ret;                      ///< Возвращаемый тип Func.
    SourceLoc loc{};

    [[nodiscard]] std::string toString() const;
};

/// Псевдоним для удобства.
using TypeExprPtr = std::unique_ptr<TypeExpr>;

/**
 * @brief Семантический тип, которым оперирует тайп-чекер.
 *
 * Отделён от @c TypeExpr, поскольку вывод типов порождает промежуточные
 * type-переменные и конкретизирует дженерики.
 */
struct Type {
    enum class Kind : std::uint8_t {
        Unknown,   ///< Ещё не выведен (type variable).
        Int, Float, Bool, String, Nil, Any,
        Vec2, Vec3, Quat, Color, Entity,
        Array, Map, Func, Class, Module, Tuple, Optional
    };
    Kind kind = Kind::Unknown;
    std::string name;                        ///< Имя класса/модуля.
    std::vector<Type> args;                  ///< Элемент Array, K/V у Map, аргументы Func, база Optional.
    Type* ret = nullptr;                     ///< Возврат Func (владеет Func).
    std::uint32_t id = 0;                    ///< Для Unknown — идентификатор type-переменной.
    bool resolved = false;                   ///< Для Unknown — решена ли переменная.
    Type* resolves = nullptr;                ///< Union-find ссылка.

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] bool isNumeric() const noexcept { return kind == Kind::Int || kind == Kind::Float; }
};

// ---------------------------------------------------------------------------
//  Выражения
// ---------------------------------------------------------------------------

enum class ExprKind : std::uint16_t {
    Nil, Bool, Number, String, InterpolatedString,
    Identifier, This, Super,
    Unary, Binary, Logical, Ternary, Coalesce,
    Call, MethodCall, Index, Member,
    Assign, CompoundAssign,
    Lambda, ArrayLit, MapLit, TupleLit, Spread,
    New, Is, As, Cast, Range, Group,
    If, While, For, Match, Block,
    Yield, Resume, TryResult, Await,
    IndexAssign, MemberAssign
};

enum class BinOp : std::uint8_t {
    Add, Sub, Mul, Div, Mod, Pow,
    Eq, Ne, Lt, Gt, Le, Ge,
    BitAnd, BitOr, BitXor, Shl, Shr,
    Concat
};

enum class UnOp : std::uint8_t { Neg, Not, BitNot };

/// Бинарный (составной) оператор присваивания.
enum class CompoundOp : std::uint8_t { Add, Sub, Mul, Div, Mod, Pow };

struct Expr;
struct Stmt;
struct Decl;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

/**
 * @brief Параметр функции/лямбды/метода.
 */
struct Param {
    std::string  name;
    TypeExprPtr  type;                 ///< nullptr => вывод.
    ExprPtr      defaultValue;         ///< nullptr => обязательный.
    bool         isRest = false;       ///< `args...` — variadic хвост.
    SourceLoc    loc{};
};

/**
 * @brief Паттерн для `match`.
 */
struct Pattern {
    enum class Kind : std::uint8_t { Wildcard, Literal, Binding, Type, Array, Map };
    Kind kind = Kind::Wildcard;
    ExprPtr literal;                       ///< Для Literal.
    std::string binding;                   ///< Для Binding (`_` = wildcard).
    TypeExprPtr type;                      ///< Для Type (`is Transform`).
    std::vector<std::unique_ptr<Pattern>> sub;
    std::vector<std::string> keys;         ///< Для Map-паттерна: ключи.
    SourceLoc loc{};
};

/**
 * @brief Ветвь `match`.
 */
struct MatchCase {
    std::unique_ptr<Pattern> pattern;
    ExprPtr guard;                         ///< Необязательное `when <expr>`.
    std::vector<StmtPtr> body;
    SourceLoc loc{};
};

/**
 * @brief Узел выражения.
 */
struct Expr {
    ExprKind kind;
    SourceLoc loc{};

    // -- Общие поля ---------------------------------------------------------
    ExprPtr   a, b, c;                     ///< Операнды / объект / then / значение.
    std::vector<ExprPtr> args;             ///< Аргументы вызова, элементы массива и т.д.
    std::vector<ExprPtr> keys;             ///< Ключи map-литерала.
    std::vector<std::string> strs;         ///< Части интерполированной строки.
    std::vector<std::unique_ptr<MatchCase>> cases;

    // -- Листовые данные ----------------------------------------------------
    double      number = 0;
    bool        boolean = false;
    std::string name;                      ///< Идентификатор, имя поля, имя метода.
    BinOp       binOp = BinOp::Add;
    UnOp        unOp  = UnOp::Neg;
    CompoundOp  cmpOp = CompoundOp::Add;

    // -- Функции/лямбды -----------------------------------------------------
    std::vector<Param> params;
    std::vector<StmtPtr> body;
    TypeExprPtr  returnType;
    bool         isBlockBody = false;      ///< `=> { ... }` vs `=> expr`.
    std::vector<std::string> captures;     ///< Заполняется чекером (для анализа).

    // -- Типы/приведения ----------------------------------------------------
    TypeExprPtr type;                      ///< Для Is/As/Cast.
    Type        resolvedType;              ///< Заполняется тайп-чекером.

    // -- Циклы/ветвления как выражения --------------------------------------
    std::vector<StmtPtr> stmts;            ///< Тело блока (Block/If/While/For).
    std::vector<ExprPtr> elifConds;
    std::vector<std::vector<StmtPtr>> elifBodies;
    std::vector<StmtPtr> elseBody;

    // -- Разное -------------------------------------------------------------
    bool optionalChain = false;            ///< `a?.b`
    bool inclusiveRange = false;           ///< `a..=b`
    bool variadic = false;                 ///< `f(args...)`

    explicit Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
};

// ---------------------------------------------------------------------------
//  Объявления и инструкции
// ---------------------------------------------------------------------------

enum class StmtKind : std::uint16_t {
    Let, ExprStmt, Block, If, While, For, Return, Break, Continue,
    Func, Class, Coroutine, Import, Match, Yield, Resume, Global, Pass
};

/**
 * @brief Поле класса: `var hp: Float = 100.0` внутри `class`.
 */
struct FieldDecl {
    std::string name;
    TypeExprPtr type;
    ExprPtr     initializer;
    bool        isConst = false;
    bool        isStatic = false;
    SourceLoc   loc{};
};

/**
 * @brief Метод класса.
 */
struct MethodDecl {
    std::string name;
    std::vector<Param> params;
    TypeExprPtr returnType;
    std::vector<StmtPtr> body;
    bool isStatic = false;
    bool isCoroutine = false;
    SourceLoc loc{};
};

/**
 * @brief Узел инструкции.
 */
struct Stmt {
    StmtKind kind;
    SourceLoc loc{};

    // -- let / var ----------------------------------------------------------
    std::vector<std::string> names;        ///< `let x, y = f()` — несколько имён.
    std::vector<TypeExprPtr> types;
    ExprPtr value;
    bool isMutable = true;                 ///< let => false, var => true.
    bool isConst = false;
    bool isGlobal = false;

    // -- выражения ----------------------------------------------------------
    ExprPtr expr;

    // -- блоки / условия ----------------------------------------------------
    std::vector<StmtPtr> body;
    ExprPtr cond;
    std::vector<ExprPtr> elifConds;
    std::vector<std::vector<StmtPtr>> elifBodies;
    std::vector<StmtPtr> elseBody;

    // -- циклы --------------------------------------------------------------
    std::string varName;                   ///< for-переменная.
    std::string varName2;                  ///< вторая переменная (`for k, v in map`).
    ExprPtr iterable;
    std::string label;                     ///< для `break outer`.

    // -- функции / классы ---------------------------------------------------
    std::string name;
    std::vector<Param> params;
    TypeExprPtr returnType;
    bool isCoroutine = false;
    std::string superClass;
    std::vector<FieldDecl> fields;
    std::vector<MethodDecl> methods;

    // -- import -------------------------------------------------------------
    std::string path;                      ///< "math" / "./systems/crop.lvs"
    std::string alias;

    // -- match / yield ------------------------------------------------------
    std::vector<std::unique_ptr<MatchCase>> cases;

    explicit Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
};

/**
 * @brief Единица компиляции: один .lvs файл или REPL-фрагмент.
 */
struct Module {
    SourceName name;
    std::vector<StmtPtr> body;
    std::vector<std::string> imports;      ///< Разрешённые зависимости (для графа hot-reload).
};

// ---------------------------------------------------------------------------
//  Visitor
// ---------------------------------------------------------------------------

/**
 * @brief Интерфейс обхода AST.
 *
 * Реализации: @c TypeChecker (вывод типов), @c Compiler (генерация байткода),
 * @c AstFormatter (pretty-print для Visual Scripting round-trip),
 * @c LintPass (проверка стиля в редакторе).
 */
class AstVisitor {
public:
    virtual ~AstVisitor() = default;
    virtual void visitExpr(Expr& e) = 0;
    virtual void visitStmt(Stmt& s) = 0;
};

/// Печать AST в человекочитаемом виде (отладка компилятора).
[[nodiscard]] std::string dumpAst(const Module& m);

} // namespace lv
