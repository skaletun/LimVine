/**
 * @file    Compiler.h
 * @brief   Компилятор AST -> байткод LV Script.
 * @ingroup LVScript
 *
 * @details Однопроходный компилятор с явной таблицей локалов и цепочкой
 *          @c FunctionCompiler для разрешения upvalue (классическая схема
 *          «captured variable», как в Lua/CPython).
 *
 *          Почему однопроходный, а не SSA/IR:
 *          - байткод стековой VM генерируется прямо из AST без промежуточного IR,
 *            что даёт минимальную задержку компиляции (важно для hot-reload и REPL);
 *          - оптимизации, которые действительно нужны игре (constant folding,
 *            peephole, специализация range-циклов, inline-кеши методов) выполняются
 *            локально и не требуют графа;
 *          - для «тяжёлых» сценариев предусмотрен @c Optimizer (см. Optimizer.h),
 *            работающий уже по байткоду (peephole + jump threading).
 */
#pragma once

#include "AST.h"
#include "Bytecode.h"

#include <functional>
#include <unordered_set>

namespace lv {

class VM;

/**
 * @brief Компилятор одного модуля.
 *
 * @note Не потокобезопасен: на каждый параллельный поток компиляции создаётся
 *       свой экземпляр. Это позволяет Job System компилировать десятки скриптов
 *       одновременно при старте проекта.
 */
/// Настройки компиляции.
///
/// @note Объявлены вне класса намеренно: GCC не позволяет использовать
///       default-member-initializers вложенного класса как аргумент по умолчанию
///       до завершения объявления объемлющего класса.
struct CompileOptions {
    bool foldConstants   = true;  ///< Константное свёртывание арифметики.
    bool peephole        = true;  ///< Peephole-оптимизации байткода.
    bool strictTypes     = true;  ///< Ошибка (а не warning) на явных конфликтах типов.
    bool allowAny        = true;  ///< Разрешён тип `Any` (в песочнице модов можно запретить).
    bool debugInfo       = true;  ///< Генерировать таблицу строк для трейсбеков.
    bool specializeLoops = true;  ///< Использовать ForRange* для целочисленных диапазонов.
};

/**
 * @brief Компилятор одного модуля.
 */
class Compiler {
public:
    using Options = CompileOptions;

    explicit Compiler(VM& vm);
    ~Compiler();

    /**
     * @brief Скомпилировать модуль.
     * @param m       AST модуля.
     * @param diags   Сюда добавляются ошибки/предупреждения.
     * @param opts    Настройки.
     * @return Прототип функции верхнего уровня или nullptr при ошибках.
     *
     * @note Возвращаемый @c ObjFunction* принадлежит GC виртуальной машины.
     */
    ObjFunction* compile(const Module& m, DiagnosticList& diags, const CompileOptions& opts = {});

    /// Компиляция одного выражения (для REPL: печатает результат).
    ObjFunction* compileExpression(const ExprPtr& e, DiagnosticList& diags, const CompileOptions& opts = {});

private:
    // -- Вложенные структуры -------------------------------------------------
    struct Local {
        std::string name;
        std::uint32_t depth = 0;
        bool isCaptured = false;
        bool isConst = false;
        bool isRest = false;
    };

    struct UpvalueDesc {
        std::uint32_t index = 0;  ///< Индекс локала в родителе или upvalue у деда.
        bool isLocal = false;     ///< true: захват из локала родителя; false: из его upvalue.
    };

    struct LoopContext {
        std::size_t startIp = 0;
        std::vector<std::size_t> breakJumps;
        std::vector<std::size_t> continueJumps;
        std::string label;
        bool isRange = false;
        bool isExprLoop = false;   ///< Цикл-выражение: break не должен снимать локалы.
        std::uint32_t hiddenBase = 0;
        std::size_t breakExitIp = 0;
        std::size_t continueIp = 0;      ///< Точка, куда ведёт `continue` (для for-in — шаг итератора).
    };

    struct FunctionCompiler {
        ObjFunction* fn = nullptr;
        FunctionCompiler* enclosing = nullptr;
        std::vector<Local> locals;
        std::vector<UpvalueDesc> upvalues;
        std::uint32_t scopeDepth = 0;
        std::uint32_t numLocals = 0;     ///< Слоты, занятые в текущей области видимости.
        std::uint32_t maxLocals = 0;     ///< Пиковое число одновременно живых слотов (=> fn->numLocals).
        std::uint32_t paramSlotCount = 0;///< Слоты, занятые параметрами («пол» numLocals).
        std::uint32_t maxStack = 0;      ///< Пиковая глубина временных значений.
        std::uint32_t stackDepth = 0;    ///< Текущая глубина временных (для maxStack).
        bool isMethod = false;
        bool isInitializer = false;
        bool isCoroutine = false;
        bool savedInCoroutine = false;  ///< Состояние `inCoroutine_` до входа в функцию.
        ObjClass* currentClass = nullptr;
        std::vector<std::pair<std::uint32_t, ExprPtr>> defaultParams; ///< (slot, expr)
        std::vector<StmtPtr> ownedBody;  ///< Владение AST для методов (жизнь на время компиляции).
    };

    // -- Управление функциями ------------------------------------------------
    void beginFunction(const std::string& name, ObjFunction::Kind kind, bool isMethod,
                       bool isCoroutine, const std::vector<Param>& params);
    ObjFunction* endFunction();

    void beginScope();
    void endScope();
    /// Закрыть области до @p targetDepth, НЕ генерируя Pop/PopN.
    /// Используется точкой провала `match`: связывания паттерна уже сняли свои
    /// значения со стека, и лишний Pop сломал бы баланс между ветвями.
    void closeScopesSilently(std::uint32_t targetDepth, std::uint32_t targetLocals);
    void declareLocal(const std::string& name, bool isConst = false, bool isRest = false);
    std::uint32_t addLocal(const std::string& name, bool isConst, bool isRest);
    std::optional<std::uint32_t> resolveLocal(FunctionCompiler* fc, const std::string& name);
    std::optional<std::uint32_t> resolveUpvalue(FunctionCompiler* fc, const std::string& name);
    std::uint32_t addUpvalue(FunctionCompiler* fc, std::uint32_t index, bool isLocal);

    // -- Эмиссия -------------------------------------------------------------
    std::size_t emit(Op op, std::uint32_t operand = 0, SourceLoc loc = {});
    std::size_t emitAt(std::size_t pos, Op op, std::uint32_t operand);
    std::size_t emitJump(Op op, SourceLoc loc = {});
    void patchJump(std::size_t pos);
    void patchJumpTo(std::size_t pos, std::size_t target);
    void emitLoop(std::size_t start, SourceLoc loc = {});
    std::uint32_t addConstant(Value v);
    std::uint32_t internName(const std::string& s);
    void pushStack(int n = 1);
    void popStack(int n = 1);

    /// True, если компилируется тело модуля верхнего уровня (не вложенная функция)
    /// и мы не внутри локальной области — объявления становятся глобалами.
    [[nodiscard]] bool isTopLevel() const noexcept {
        return current_ && current_->enclosing == nullptr && current_->scopeDepth <= 1;
    }

    void error(SourceLoc loc, std::string msg, std::string hint = {});
    void warning(SourceLoc loc, std::string msg, std::string hint = {});

    // -- Компиляция узлов ----------------------------------------------------
    void compileStmt(const StmtPtr& s);
    void compileBody(const std::vector<StmtPtr>& body, bool pushValue = false);
    void compileExpr(const ExprPtr& e);
    void compileFunction(const std::string& name, const std::vector<Param>& params,
                         const std::vector<StmtPtr>& body, const TypeExprPtr& ret,
                         ObjFunction::Kind kind, bool isCoroutine, SourceLoc loc);
    void compileClass(const StmtPtr& s);
    void compilePattern(const Pattern& p, std::size_t subjectSlot,
                        std::vector<std::size_t>& failJumps, std::vector<std::string>& bindings);
    void compileMatch(const ExprPtr& subject, const std::vector<std::unique_ptr<MatchCase>>& cases);
    void compileFor(const std::string& var, const std::string& var2, const ExprPtr& iterable,
                    const std::vector<StmtPtr>& body, const std::string& label, SourceLoc loc);
    void compileForExpr(const std::string& var, const std::string& var2, const ExprPtr& iterable,
                        const std::vector<StmtPtr>& body, SourceLoc loc);
    /// Общая механика цикла for-in (range-специализация и протокол итератора).
    /// @param body Колбэк, компилирующий тело цикла; вызывается внутри области видимости.
    /// @param body       Колбэк тела цикла (вызывается внутри области видимости тела).
    void compileLoopCommon(const ExprPtr& iterable, const std::string& var, const std::string& var2,
                           const std::function<void()>& body, const std::string& label, SourceLoc loc);
    /// `accLocal.push(valueFromStack)` — используется list-comprehension.
    void emitArrayPushLocal(std::uint32_t accLocal, SourceLoc loc);
    /// Эмиссия Op::Closure + хвост с описанием upvalue только что завершённой функции.
    void emitClosure(ObjFunction* fn, const std::vector<UpvalueDesc>& upvalues, SourceLoc loc);
    void compileAssignTarget(const ExprPtr& target, bool compound, CompoundOp op);

    /// Константное свёртывание. Возвращает true, если выражение заменено константой.
    bool tryFold(Expr& e);

    VM& vm_;
    Options opts_{};
    DiagnosticList* diags_ = nullptr;
    FunctionCompiler* current_ = nullptr;
    std::vector<std::unique_ptr<FunctionCompiler>> fcs_;   ///< Владение всеми FC.
    std::vector<LoopContext> loops_;
    std::vector<ObjFunction*> allProtos_;
    std::uint32_t nextProtoId_ = 0;
    bool inCoroutine_ = false;
    bool savedCoroutineState_ = false;
    bool inLoop_ = false;
    std::uint32_t classDepth_ = 0;
    /// Имя суперкласса для `super.x` (компилятор подставляет ссылку на класс;
    /// полноценный runtime-`super` см. в плане развития языка).
    std::string enclosingSuper_;
    bool hasEnclosingSuper_ = false;
    /**
     * @brief Имена, объявленные на верхнем уровне модуля.
     *
     * Объявления верхнего уровня (let/var/func/class/global) компилируются в
     * DefineGlobal, то есть живут в глобальной таблице VM. При этом они также
     * занимают слоты локалов прототипа <script>. Если вложенная функция
     * обратится к такому имени, resolveUpvalue() «захватит» слот кадра скрипта —
     * а кадр умирает сразу после выполнения модуля, и upvalue начнёт читать
     * мусор (именно так `global player` превращался в deltaTime).
     * Поэтому разрешение имён сначала сверяется с этим множеством.
     */
    std::unordered_set<std::string> moduleGlobals_;
};

} // namespace lv
