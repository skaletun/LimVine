/**
 * @file    Compiler.cpp
 * @brief   Реализация компилятора AST -> байткод LV Script.
 *
 * Дисциплина стека
 * ----------------
 * Каждая функция @c compileExpr оставляет ровно одно значение на вершине стека
 * и поддерживает счётчик @c FunctionCompiler::stackDepth. Инструкции, потребляющие
 * операнды, явно вызывают @c popStack. Это инвариант, проверяемый ассертом в
 * отладочной сборке и юнит-тестом `vm_stack_balance`.
 */
#include "Compiler.h"
#include "VM.h"

#include <cmath>

namespace lv {

namespace {
/// Глубина области временных значений, резервируемая VM на кадр.
constexpr std::uint32_t kTempSlots = 256;
} // namespace

// ---------------------------------------------------------------------------
//  Конструктор
// ---------------------------------------------------------------------------
Compiler::Compiler(VM& vm) : vm_(vm) {}
Compiler::~Compiler() = default;

// ---------------------------------------------------------------------------
//  Эмиссия
// ---------------------------------------------------------------------------
std::size_t Compiler::emit(Op op, std::uint32_t operand, SourceLoc loc) {
    auto& code = current_->fn->code;
    const std::size_t pos = code.size();
    code.push_back(static_cast<Byte>(op));
    emitOperand24(code, operand);
    if (opts_.debugInfo) current_->fn->lineStarts.push_back(loc.line);
    return pos;
}

std::size_t Compiler::emitAt(std::size_t pos, Op op, std::uint32_t operand) {
    auto& code = current_->fn->code;
    code[pos] = static_cast<Byte>(op);
    code[pos + 1] = static_cast<Byte>((operand >> 16) & 0xFF);
    code[pos + 2] = static_cast<Byte>((operand >> 8) & 0xFF);
    code[pos + 3] = static_cast<Byte>(operand & 0xFF);
    return pos;
}

std::size_t Compiler::emitJump(Op op, SourceLoc loc) { return emit(op, 0, loc); }

void Compiler::patchJumpTo(std::size_t pos, std::size_t target) {
    // Смещение знаковое: `continue` в for-in прыгает назад (на шаг итератора),
    // поэтому target < pos — легальная ситуация.
    const std::int64_t off = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pos + 4);
    if (off > static_cast<std::int64_t>(kMaxOperand24) / 2 ||
        off < -static_cast<std::int64_t>(kMaxOperand24) / 2) {
        error({}, "jump distance exceeds the 24-bit operand range", "split the function into smaller ones");
        return;
    }
    emitAt(pos, static_cast<Op>(current_->fn->code[pos]), static_cast<std::uint32_t>(off) & 0xFFFFFF);
}

void Compiler::patchJump(std::size_t pos) { patchJumpTo(pos, current_->fn->code.size()); }

void Compiler::emitLoop(std::size_t start, SourceLoc loc) {
    const std::size_t pos = current_->fn->code.size();
    const std::int64_t off = static_cast<std::int64_t>(start) - static_cast<std::int64_t>(pos + 4);
    emit(Op::Loop, static_cast<std::uint32_t>(off) & 0xFFFFFF, loc);
}

std::uint32_t Compiler::addConstant(Value v) {
    auto& consts = current_->fn->constants;
    for (std::uint32_t i = 0; i < consts.size(); ++i)
        if (valuesEqual(consts[i], v)) return i;
    consts.push_back(v);
    if (consts.size() > kMaxOperand24) error({}, "too many constants in one function");
    return static_cast<std::uint32_t>(consts.size() - 1);
}

std::uint32_t Compiler::internName(const std::string& s) { return vm_.internName(s); }

void Compiler::pushStack(int n) {
    current_->stackDepth += static_cast<std::uint32_t>(n);
    current_->maxStack = std::max(current_->maxStack, current_->stackDepth);
    if (current_->stackDepth > kTempSlots)
        error({}, "expression stack overflow: too many temporaries in a single expression");
}

void Compiler::popStack(int n) {
    const auto d = static_cast<std::uint32_t>(n);
    current_->stackDepth = current_->stackDepth > d ? current_->stackDepth - d : 0;
}

void Compiler::error(SourceLoc loc, std::string msg, std::string hint) {
    if (diags_)
        diags_->push_back(Diagnostic{DiagSeverity::Error,
                                     current_ ? current_->fn->source : SourceName{},
                                     loc, std::move(msg), std::move(hint)});
}

void Compiler::warning(SourceLoc loc, std::string msg, std::string hint) {
    if (diags_)
        diags_->push_back(Diagnostic{DiagSeverity::Warning,
                                     current_ ? current_->fn->source : SourceName{},
                                     loc, std::move(msg), std::move(hint)});
}

// ---------------------------------------------------------------------------
//  Области видимости
// ---------------------------------------------------------------------------
void Compiler::beginScope() { ++current_->scopeDepth; }

void Compiler::endScope() {
    // Освобождаем СЛОТЫ локалов, но НЕ трогаем стек значений.
    //
    // В этой VM локалы живут в слотах кадра [base, base+numLocals), а стек
    // значений начинается выше (tempBase) и служит только для временных
    // операндов. Объявление `let x = e` компилируется как `e; SetLocal; Pop`
    // (SetLocal — peek-запись), поэтому к концу области видимости на стеке НЕ
    // остаётся ничего от локалов.
    //
    // Прежняя версия выдавала здесь Pop/PopN «по числу закрытых локалов» —
    // это наследие стековой модели Crafting Interpreters, где локалы и есть
    // стек. В нашей модели такие Pop'ы снимали настоящие временные значения:
    // внутри циклов они съедали callee/аргументы следующего вызова, и уже на
    // второй итерации `let c = "{a},{i}"` читал мусор (в stdlib/pathfinding
    // это выглядело как «cannot compare Instance with Int», а в тестах — как
    // -9.2e18 и «съехавшие» строки лога).
    --current_->scopeDepth;
    while (!current_->locals.empty() && current_->locals.back().depth > current_->scopeDepth) {
        current_->locals.pop_back();
        if (current_->numLocals > 0) --current_->numLocals;
    }
}

void Compiler::closeScopesSilently(std::uint32_t targetDepth, std::uint32_t targetLocals) {
    current_->scopeDepth = targetDepth;
    current_->numLocals = targetLocals;
    if (current_->locals.size() > targetLocals) current_->locals.resize(targetLocals);
}

std::uint32_t Compiler::addLocal(const std::string& name, bool isConst, bool isRest) {
    if (current_->numLocals >= kMaxLocals) { error({}, "too many local variables in function"); return 0; }
    Local l;
    l.name = name;
    l.depth = current_->scopeDepth;
    l.isConst = isConst;
    l.isRest = isRest;
    current_->locals.push_back(l);
    const std::uint32_t slot = current_->numLocals++;
    current_->maxLocals = std::max(current_->maxLocals, current_->numLocals);
    return slot;
}

void Compiler::declareLocal(const std::string& name, bool isConst, bool isRest) {
    for (auto it = current_->locals.rbegin(); it != current_->locals.rend(); ++it) {
        if (it->depth != current_->scopeDepth) break;
        if (it->name == name && !name.empty() && name.front() != '<') {
            warning({}, format("local '{}' shadows a variable in the same scope", name));
            break;
        }
    }
    addLocal(name, isConst, isRest);
}

std::optional<std::uint32_t> Compiler::resolveLocal(FunctionCompiler* fc, const std::string& name) {
    for (std::size_t i = fc->locals.size(); i-- > 0;)
        if (fc->locals[i].name == name) return static_cast<std::uint32_t>(i);
    return std::nullopt;
}

std::uint32_t Compiler::addUpvalue(FunctionCompiler* fc, std::uint32_t index, bool isLocal) {
    for (std::uint32_t i = 0; i < fc->upvalues.size(); ++i)
        if (fc->upvalues[i].index == index && fc->upvalues[i].isLocal == isLocal) return i;
    fc->upvalues.push_back(UpvalueDesc{index, isLocal});
    return static_cast<std::uint32_t>(fc->upvalues.size() - 1);
}

std::optional<std::uint32_t> Compiler::resolveUpvalue(FunctionCompiler* fc, const std::string& name) {
    if (!fc->enclosing) return std::nullopt;
    if (auto local = resolveLocal(fc->enclosing, name)) {
        fc->enclosing->locals[*local].isCaptured = true;
        return addUpvalue(fc, *local, true);
    }
    if (auto up = resolveUpvalue(fc->enclosing, name)) return addUpvalue(fc, *up, false);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
//  Функции
// ---------------------------------------------------------------------------
void Compiler::beginFunction(const std::string& name, ObjFunction::Kind kind, bool isMethod,
                             bool isCoroutine, const std::vector<Param>& params) {
    auto fc = std::make_unique<FunctionCompiler>();
    fc->enclosing = current_;
    fc->isMethod = isMethod;
    fc->isCoroutine = isCoroutine;
    fc->fn = vm_.gc().allocate<ObjFunction>(sizeof(ObjFunction),
                                            ObjHeader(ObjHeader::Type::Function, sizeof(ObjFunction)));
    fc->fn->kind = kind;
    fc->fn->name = vm_.internRaw(name);
    fc->fn->source = current_ ? current_->fn->source : SourceName{"<anon>"};
    fc->fn->protoId = nextProtoId_++;
    if (current_) current_->fn->nested.push_back(fc->fn);
    fcs_.push_back(std::move(fc));
    current_ = fcs_.back().get();

    // Параметры занимают слоты 0..N-1.
    std::uint32_t required = 0;
    bool seenDefault = false;
    for (std::uint32_t i = 0; i < params.size(); ++i) {
        const Param& p = params[i];
        addLocal(p.name, /*isConst=*/false, p.isRest);
        if (p.isRest) {
            current_->fn->restParamSlot = static_cast<std::int32_t>(i);
        } else if (p.defaultValue) {
            seenDefault = true;
        } else if (!seenDefault) {
            ++required;
        }
    }
    current_->fn->arity = required;
    current_->fn->totalParams = static_cast<std::uint32_t>(params.size());
    // «Пол» числа локалов: после компиляции тела numLocals обязан вернуться
    // к этому значению, иначе VM неверно вычислит начало области временных.
    current_->paramSlotCount = current_->numLocals;
    current_->savedInCoroutine = savedCoroutineState_;
    savedCoroutineState_ = inCoroutine_;
    if (isCoroutine) inCoroutine_ = true;
}

ObjFunction* Compiler::endFunction() {
    FunctionCompiler* fc = current_;
    // numLocals к концу функции равен числу слотов параметров (см. compileFunction) —
    // именно оно определяет, где VM начинает область временных значений.
    // maxLocals используется для maxStack и проверки лимита слотов.
    // В прототип записывается ПИКОВОЕ число одновременно живых слотов.
    // Текущий numLocals к этому моменту уже уменьшен endScope() (локалы циклов
    // и блоков освобождены), но VM обязана зарезервировать под них место:
    // область временных значений начинается с base + numLocals, и если взять
    // «текущее» значение, временные наложатся на слоты циклов и параметров.
    LV_ASSERT(fc->maxLocals >= fc->paramSlotCount);
    fc->fn->numLocals = fc->maxLocals;
    fc->fn->maxStack = fc->maxLocals + fc->maxStack + 16;
    current_ = fc->enclosing;
    inCoroutine_ = fc->savedInCoroutine;
    ObjFunction* fn = fc->fn;
    allProtos_.push_back(fn);
    return fn;
}

void Compiler::emitClosure(ObjFunction* fn, const std::vector<UpvalueDesc>& upvalues, SourceLoc loc) {
    fn->numUpvalues = static_cast<std::uint32_t>(upvalues.size());
    const std::uint32_t fidx = addConstant(Value::object(fn));
    emit(Op::Closure, fidx, loc);
    auto& code = current_->fn->code;
    code.push_back(static_cast<Byte>(std::min<std::size_t>(upvalues.size(), 255)));
    for (const auto& uv : upvalues) {
        code.push_back(uv.isLocal ? 1 : 0);
        emitOperand24(code, uv.index);
    }
    if (opts_.debugInfo)
        for (std::size_t i = 0; i < 1 + upvalues.size() * 4; ++i)
            fn->lineStarts.push_back(loc.line); // синхронизируем таблицу строк
}

void Compiler::compileFunction(const std::string& name, const std::vector<Param>& params,
                               const std::vector<StmtPtr>& body, const TypeExprPtr& ret,
                               ObjFunction::Kind kind, bool isCoroutine, SourceLoc loc) {
    (void)ret;
    beginFunction(name, kind, kind == ObjFunction::Kind::Method || kind == ObjFunction::Kind::Initializer,
                  isCoroutine, params);

    // Проверки параметров по умолчанию: выполняются сразу после входа в функцию.
    for (std::uint32_t i = 0; i < params.size(); ++i) {
        if (!params[i].defaultValue) continue;
        // Op::JumpIfTrue СНИМАЕТ проверяемое значение (в отличие от
        // JumpIfTruePop/ JumpIfNil), поэтому дополнительного Pop здесь быть не
        // должно. Лишний Pop уводил вершину стека НИЖЕ базы кадра, и следующие
        // Constant/PokeSlot записывали значение по умолчанию в слот callee и в
        // СОСЕДНИЙ параметр: `new Slot()` оставлял в `count` мусор
        // («cannot compare String with Int» в stdlib/inventory).
        //
        // Баланс обеих ветвей одинаков:
        //   значение есть  -> PeekSlot(+1), JumpIfTrue(-1) = 0;
        //   значения нет   -> PeekSlot(+1), JumpIfTrue(-1), default(+1),
        //                     PokeSlot(-1) = 0.
        emit(Op::PeekSlot, i, loc);
        pushStack();
        const std::size_t j = emitJump(Op::JumpIfTrue, loc);   // не nil -> оставить как есть
        popStack();
        compileExpr(params[i].defaultValue);
        pushStack();
        emit(Op::PokeSlot, i, loc);
        popStack();
        patchJump(j);
    }

    beginScope();
    compileBody(body, /*pushValue=*/false);
    endScope();
    emit(Op::ReturnNil, 0, loc);

    const std::vector<UpvalueDesc> ups = current_->upvalues;
    ObjFunction* fn = endFunction();
    emitClosure(fn, ups, loc);
}

// ---------------------------------------------------------------------------
//  Тело блока (последнее expression-statement = значение блока)
// ---------------------------------------------------------------------------
void Compiler::compileBody(const std::vector<StmtPtr>& body, bool pushValue) {
    for (std::size_t i = 0; i < body.size(); ++i) {
        const bool isLast = (i + 1 == body.size());
        if (isLast && pushValue && body[i]->kind == StmtKind::ExprStmt) {
            compileExpr(body[i]->expr);
            return;
        }
        compileStmt(body[i]);
    }
    if (pushValue) { emit(Op::Nil); pushStack(); }
}

// ---------------------------------------------------------------------------
//  Инструкции
// ---------------------------------------------------------------------------
void Compiler::compileStmt(const StmtPtr& s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Pass:
            emit(Op::Nop, 0, s->loc);
            break;

        case StmtKind::Let: {
            // Объявления верхнего уровня модуля становятся ГЛОБАЛАМИ: они должны
            // быть видны из вложенных функций и переживать кадр прототипа <script>.
            // (Локалом модуля объявление делать нельзя: замыкание захватило бы слот
            // кадра, который умирает сразу после исполнения модуля.)
            const bool topLevel = isTopLevel();
            if (topLevel) for (const std::string& n : s->names) moduleGlobals_.insert(n);

            if (s->names.size() == 1) {
                const bool namedLambda = s->value && s->value->kind == ExprKind::Lambda;
                if (namedLambda && !topLevel) declareLocal(s->names[0], !s->isMutable);
                if (s->value) compileExpr(s->value);
                else { emit(Op::Nil, 0, s->loc); pushStack(); }
                if (topLevel) {
                    emit(Op::DefineGlobal, internName(s->names[0]), s->loc);
                } else {
                    if (!namedLambda) declareLocal(s->names[0], !s->isMutable);
                    emit(Op::SetLocal, current_->numLocals - 1, s->loc);
                    // SetLocal — peek-запись: объявление не является выражением,
                    // поэтому значение нужно снять явно. Без этого каждый `let`
                    // внутри функции протекал одним слотом стека, и в длинных
                    // функциях временные значения затирали ЛОКАЛЫ кадра
                    // (в stdlib/pathfinding это выглядело как «cannot index a Int»).
                    emit(Op::Pop, 0, s->loc);
                }
                popStack();
            } else {
                if (s->value) compileExpr(s->value);
                else { emit(Op::Nil, 0, s->loc); pushStack(); }
                for (std::size_t i = 0; i < s->names.size(); ++i) {
                    emit(Op::Dup, 0, s->loc);
                    pushStack();
                    emit(Op::Constant, addConstant(Value::integer(static_cast<std::int64_t>(i))), s->loc);
                    pushStack();
                    emit(Op::IndexGet, 0, s->loc);
                    popStack(2); pushStack();
                    if (topLevel) {
                        emit(Op::DefineGlobal, internName(s->names[i]), s->loc);
                    } else {
                        declareLocal(s->names[i], !s->isMutable);
                        emit(Op::SetLocal, current_->numLocals - 1, s->loc);
                        emit(Op::Pop, 0, s->loc);   // SetLocal — peek-запись
                    }
                    popStack();
                }
                emit(Op::Pop, 0, s->loc);
                popStack();
            }
            break;
        }

        case StmtKind::ExprStmt:
            compileExpr(s->expr);
            emit(Op::Pop, 0, s->loc);
            popStack();
            break;

        case StmtKind::Block:
            beginScope();
            for (auto& st : s->body) compileStmt(st);
            endScope();
            break;

        case StmtKind::If: {
            compileExpr(s->cond);
            pushStack();
            std::size_t j = emitJump(Op::JumpIfFalse, s->loc);
            popStack();
            beginScope();
            for (auto& st : s->body) compileStmt(st);
            endScope();
            std::vector<std::size_t> endJumps{emitJump(Op::Jump, s->loc)};
            patchJump(j);
            for (std::size_t i = 0; i < s->elifConds.size(); ++i) {
                compileExpr(s->elifConds[i]);
                pushStack();
                const std::size_t ej = emitJump(Op::JumpIfFalse, s->loc);
                popStack();
                beginScope();
                for (auto& st : s->elifBodies[i]) compileStmt(st);
                endScope();
                endJumps.push_back(emitJump(Op::Jump, s->loc));
                patchJump(ej);
            }
            if (!s->elseBody.empty()) {
                beginScope();
                for (auto& st : s->elseBody) compileStmt(st);
                endScope();
            }
            for (std::size_t ej : endJumps) patchJump(ej);
            break;
        }

        case StmtKind::While: {
            const std::uint32_t hiddenBase = current_->numLocals;
            const std::size_t start = current_->fn->code.size();
            compileExpr(s->cond);
            pushStack();
            const std::size_t exitJump = emitJump(Op::JumpIfFalse, s->loc);
            popStack();

            LoopContext lc;
            lc.startIp = start;
            lc.label = s->label;
            lc.hiddenBase = hiddenBase;
            loops_.push_back(lc);

            beginScope();
            const bool prev = inLoop_;
            inLoop_ = true;
            for (auto& st : s->body) compileStmt(st);
            inLoop_ = prev;
            endScope();
            emitLoop(start, s->loc);
            patchJump(exitJump);
            loops_.back().breakExitIp = current_->fn->code.size();
            for (std::size_t c : loops_.back().continueJumps) patchJumpTo(c, start);
            for (std::size_t b : loops_.back().breakJumps) patchJumpTo(b, loops_.back().breakExitIp);
            loops_.pop_back();
            break;
        }

        case StmtKind::For:
            compileFor(s->varName, s->varName2, s->iterable, s->body, s->label, s->loc);
            break;

        case StmtKind::Return: {
            if (s->expr) {
                if (current_->isInitializer)
                    warning(s->loc, "value returned from 'init' is ignored");
                compileExpr(s->expr);
                pushStack();
                emit(Op::Return, 0, s->loc);
                popStack();
            } else if (current_->isInitializer) {
                emit(Op::GetThis, 0, s->loc); pushStack();
                emit(Op::Return, 0, s->loc);   popStack();
            } else {
                emit(Op::ReturnNil, 0, s->loc);
            }
            break;
        }

        case StmtKind::Break:
        case StmtKind::Continue: {
            if (loops_.empty()) { error(s->loc, s->kind == StmtKind::Break ? "'break' outside of a loop"
                                                                          : "'continue' outside of a loop"); break; }
            std::size_t idx = loops_.size() - 1;
            if (!s->label.empty()) {
                bool found = false;
                for (std::size_t i = loops_.size(); i-- > 0;)
                    if (loops_[i].label == s->label) { idx = i; found = true; break; }
                if (!found) { error(s->loc, format("no loop labeled '{}'", s->label)); break; }
            }
            // Локалы цикла снимаются общим PopN в точке выхода (breakExitIp),
            // поэтому здесь достаточно сбалансировать только временные значения.
            if (s->kind == StmtKind::Break) loops_[idx].breakJumps.push_back(emitJump(Op::Jump, s->loc));
            else                            loops_[idx].continueJumps.push_back(emitJump(Op::Jump, s->loc));
            break;
        }

        case StmtKind::Func:
        case StmtKind::Coroutine: {
            const bool isCo = (s->kind == StmtKind::Coroutine);
            compileFunction(s->name, s->params, s->body, s->returnType, ObjFunction::Kind::Script, isCo, s->loc);
            if (isCo) emit(Op::MakeCoroutine, 0, s->loc);
            if (isTopLevel()) {
                moduleGlobals_.insert(s->name);
                emit(Op::DefineGlobal, internName(s->name), s->loc);
            } else {
                declareLocal(s->name);
                emit(Op::SetLocal, current_->numLocals - 1, s->loc);
                emit(Op::Pop, 0, s->loc);   // SetLocal — peek-запись
            }
            popStack();
            break;
        }

        case StmtKind::Class:
            compileClass(s);
            break;

        case StmtKind::Import: {
            if (!vm_.sandbox().allowImport) { error(s->loc, "'import' is disabled in this sandbox"); break; }
            const auto& allow = vm_.sandbox().allowedImports;
            if (!allow.empty() && std::find(allow.begin(), allow.end(), s->path) == allow.end()) {
                error(s->loc, format("module '{}' is not in the sandbox allow-list", s->path));
                break;
            }
            const std::string bindName = s->alias.empty() ? s->path : s->alias;
            emit(Op::GetModule, internName(s->path), s->loc);
            pushStack();
            if (isTopLevel()) { moduleGlobals_.insert(bindName); emit(Op::DefineGlobal, internName(bindName), s->loc); }
            else {
                declareLocal(bindName);
                emit(Op::SetLocal, current_->numLocals - 1, s->loc);
                emit(Op::Pop, 0, s->loc);   // SetLocal — peek-запись
            }
            popStack();
            break;
        }

        case StmtKind::Match:
            compileMatch(s->expr, s->cases);
            emit(Op::Pop, 0, s->loc);
            popStack();
            break;

        case StmtKind::Yield: {
            if (!inCoroutine_) error(s->loc, "'yield' is only allowed inside a 'coroutine' function");
            if (s->expr) compileExpr(s->expr);
            else { emit(Op::Nil, 0, s->loc); pushStack(); }
            emit(Op::Yield, 1, s->loc);
            break;
        }

        case StmtKind::Resume:
            compileExpr(s->expr);
            pushStack();
            emit(Op::Resume, 0, s->loc);
            break;

        case StmtKind::Global:
            break;
    }
}

// ---------------------------------------------------------------------------
//  for-in
// ---------------------------------------------------------------------------
void Compiler::emitArrayPushLocal(std::uint32_t accLocal, SourceLoc loc) {
    // [value] -> [] : acc.push(value)
    emit(Op::GetLocal, accLocal, loc);
    pushStack();
    emit(Op::Swap, 0, loc);          // [acc][value]
    emit(Op::ArrayPush, 0, loc);     // -> [acc]
    emit(Op::Pop, 0, loc);
    popStack();
}

void Compiler::compileLoopCommon(const ExprPtr& iterable, const std::string& var, const std::string& var2,
                                 const std::function<void()>& body, const std::string& label, SourceLoc loc) {
    const bool twoVars = !var2.empty();
    const std::uint32_t hiddenBase = current_->numLocals;
    const std::uint32_t localsBeforeLoop = current_->numLocals;
    const std::uint32_t scopeBeforeLoop = current_->scopeDepth;
    const bool isRange = opts_.specializeLoops && iterable->kind == ExprKind::Range;

    LoopContext lc;
    lc.label = label;
    lc.hiddenBase = hiddenBase;
    lc.isRange = isRange;
    loops_.push_back(lc);

    beginScope();
    const bool prevLoop = inLoop_;
    inLoop_ = true;

    if (isRange) {
        // Специализация: `for i in a..b` компилируется в числовой цикл
        // без аллокации итератора — критично для сгенерированного Visual Scripting кода.
        compileExpr(iterable->a);
        pushStack();
        compileExpr(iterable->b);
        pushStack();
        emit(Op::IntConst, static_cast<std::uint32_t>(iterable->inclusiveRange ? 1 : 0) & 0xFFFFFF, loc);
        pushStack();
        const std::uint32_t varSlot = current_->numLocals;
        declareLocal(var);
        addLocal("<end>", false, false);
        addLocal("<step>", false, false);
        emit(Op::ForRangePrep, varSlot, loc);
        popStack(3);

        // ForRangeLoop хранит в операнде смещение выхода, поэтому слот переменной
        // цикла передаём через кадр (так же, как итератор в общем for-in).
        emit(Op::SetLoopSlots, (varSlot & 0xFFFF) << 8, loc);

        const std::size_t loopIp = current_->fn->code.size();
        loops_.back().startIp = loopIp;
        loops_.back().continueIp = loopIp;   // числовой цикл: continue == следующий шаг
        const std::size_t exitJump = emitJump(Op::ForRangeLoop, loc);

        beginScope();
        body();
        endScope();

        for (std::size_t c : loops_.back().continueJumps) patchJumpTo(c, loops_.back().continueIp);
        emitLoop(loopIp, loc);
        patchJump(exitJump);
        // Точка выхода `break` — ПОСЛЕ обратной дуги и после патча выхода,
        // иначе break попадает на Op::Loop и цикл продолжается.
        loops_.back().breakExitIp = current_->fn->code.size();
        for (std::size_t b : loops_.back().breakJumps) patchJumpTo(b, loops_.back().breakExitIp);
    } else {
        compileExpr(iterable);
        pushStack();
        const std::uint32_t iterSlot = current_->numLocals;
        addLocal("<iter>", false, false);
        emit(Op::IterInit, 0, loc);
        emit(Op::SetLocal, iterSlot, loc);   // peek-запись
        emit(Op::Pop, 0, loc);               // итератор снят, стек чист
        popStack();

        const std::uint32_t varSlot = current_->numLocals;
        declareLocal(var);
        const std::uint32_t var2Slot = twoVars ? addLocal(var2, false, false) : 0xFFu;

        // Сообщаем VM, в каком слоте кадра лежит итератор.
        emit(Op::SetLoopSlots, (iterSlot & 0xFFFF) << 16, loc);

        const std::size_t loopIp = current_->fn->code.size();
        loops_.back().startIp = loopIp;
        const std::size_t exitJump = emitJump(twoVars ? Op::IterKeyNext : Op::IterNext, loc);
        // `continue` должен попасть НА САМ ИТЕРАТОР (loopIp), а не после него.
        //
        // Прежнее значение `loopIp + 4` указывало на SetLocal/SetStorePair, то
        // есть в ТЕЛО цикла: итератор не advances'ился, а SetLocal снимал со
        // стека значение, которого там не было. Каждый `continue` therefore
        // сдвигал стек на -1, и уже на следующей итерации временные значения
        // писались поверх локалов кадра (переменная цикла превращалась в чужой
        // объект: «'NativeFn' has no member 'id'» в stdlib/inventory,
        // «cannot index a Int» в stdlib/pathfinding).
        loops_.back().continueIp = loopIp;
        if (twoVars) {
            // Одна инструкция снимает обе величины: SetLocal не pop'ает стек,
            // поэтому пара «key,value» записывается атомарно.
            emit(Op::StorePair, ((varSlot & 0xFFF) << 12) | (var2Slot & 0xFFF), loc);
            popStack(2);
        } else {
            emit(Op::SetLocal, varSlot, loc);   // peek-запись
            emit(Op::Pop, 0, loc);              // значение итерации снято
            popStack();
        }
        beginScope();
        body();
        endScope();

        for (std::size_t c : loops_.back().continueJumps) patchJumpTo(c, loops_.back().continueIp);
        emitLoop(loopIp, loc);
        patchJump(exitJump);
        loops_.back().breakExitIp = current_->fn->code.size();
        for (std::size_t b : loops_.back().breakJumps) patchJumpTo(b, loops_.back().breakExitIp);
    }

    inLoop_ = prevLoop;
    loops_.pop_back();

    // Служебные слоты цикла (<iter>/var/<end>/<step>) НЕ освобождаются.
    //
    // Если закрыть их область, numLocals уменьшится, и СЛЕДУЮЩИЙ цикл в той же
    // функции получит те же номера слотов — но fn->numLocals (пиковое значение)
    // уже зафиксирован, поэтому область временных значений начинается выше, и
    // новые слоты цикла окажутся ВНУТРИ неё. Первый же push временного значения
    // затрёт итератор («broken iterator») или границы диапазона.
    //
    // Цена — несколько неиспользуемых слотов на каждый вложенный цикл; они
    // учтены в maxLocals и не влияют на производительность.
    closeScopesSilently(scopeBeforeLoop, localsBeforeLoop);
}

void Compiler::compileFor(const std::string& var, const std::string& var2, const ExprPtr& iterable,
                          const std::vector<StmtPtr>& body, const std::string& label, SourceLoc loc) {
    compileLoopCommon(iterable, var, var2, [&] { for (auto& st : body) compileStmt(st); }, label, loc);
}

void Compiler::compileForExpr(const std::string& var, const std::string& var2, const ExprPtr& iterable,
                              const std::vector<StmtPtr>& body, SourceLoc loc) {
    compileFor(var, var2, iterable, body, {}, loc);
}

// ---------------------------------------------------------------------------
//  Классы
// ---------------------------------------------------------------------------
void Compiler::compileClass(const StmtPtr& s) {
    // Класс создаётся ВО ВРЕМЯ КОМПИЛЯЦИИ и кладётся в пул констант: все
    // инструкции (ClassMethod/ClassField/ClassSuper) ссылаются на него через
    // индекс константы. Это убирает жонглирование стеком и связанные с ним
    // ошибки порядка (класс больше не может «потеряться» под замыканиями).
    ObjClass* cls = vm_.declareClass(s->name);
    const std::uint32_t clsConst = addConstant(Value::object(cls));
    const std::uint32_t nameIdx = internName(s->name);

    emit(Op::Class, clsConst, s->loc);

    const bool hasSuper = !s->superClass.empty();
    if (hasSuper) {
        if (s->superClass == s->name) { error(s->loc, "a class cannot inherit from itself"); return; }
        // [c12 = const index of the class][n12 = name index of the superclass]
        emit(Op::ClassSuper, ((clsConst & 0xFFF) << 12) | (internName(s->superClass) & 0xFFF), s->loc);
    }

    ++classDepth_;
    const bool prevInCoroutine = inCoroutine_;
    const std::string prevSuper = enclosingSuper_;
    const bool prevHasSuper = hasEnclosingSuper_;
    enclosingSuper_ = s->superClass;
    hasEnclosingSuper_ = hasSuper;

    // Значения полей по умолчанию (применяются при создании инстанса).
    for (const FieldDecl& f : s->fields) {
        if (!f.initializer) continue;
        // Изменяемое значение по умолчанию создаётся ОДИН РАЗ — здесь, при
        // определении класса, — и затем копируется в каждый инстанс как ОДНА И ТА
        // ЖЕ ссылка. Все экземпляры класса разделили бы один массив/map, что
        // выглядит как «призрачное» состояние (в stdlib/quest.lvs это давало
        // q.objectives.len() == 3 для квеста с двумя целями: цели второго квеста
        // попадали в первый). Предупреждаем и предлагаем пересоздавать в init().
        if (f.initializer->kind == ExprKind::ArrayLit || f.initializer->kind == ExprKind::MapLit)
            warning(f.loc,
                    format("field '{}' is initialised with a shared mutable default", f.name),
                    "create it in init() instead: `func init() do this." + f.name +
                    " = " + (f.initializer->kind == ExprKind::ArrayLit ? "[]" : "{}") + " end`");
        compileExpr(f.initializer);
        pushStack();
        emit(Op::ClassField, ((clsConst & 0xFFF) << 12) | (internName(f.name) & 0xFFF), f.loc);
        popStack();
    }
    // Методы.
    for (const MethodDecl& m : s->methods) {
        const bool isInit = (m.name == "init" && !m.isStatic);
        compileFunction(m.name, m.params, m.body, m.returnType,
                        isInit ? ObjFunction::Kind::Initializer : ObjFunction::Kind::Method,
                        m.isCoroutine, m.loc);
        if (m.isCoroutine) emit(Op::MakeCoroutine, 0, m.loc);
        emit(m.isStatic ? Op::StaticMethod : Op::ClassMethod,
             ((clsConst & 0xFFF) << 12) | (internName(m.name) & 0xFFF), m.loc);
        popStack();
    }

    inCoroutine_ = prevInCoroutine;
    enclosingSuper_ = prevSuper;
    hasEnclosingSuper_ = prevHasSuper;
    --classDepth_;

    // Класс становится глобалом (или локалом, если объявлен внутри функции).
    emit(Op::Constant, clsConst, s->loc);
    pushStack();
    if (isTopLevel()) {
        emit(Op::DefineGlobal, nameIdx, s->loc);
    } else {
        declareLocal(s->name);
        emit(Op::SetLocal, current_->numLocals - 1, s->loc);
        emit(Op::Pop, 0, s->loc);   // SetLocal — peek-запись
    }
    popStack();
}

// ---------------------------------------------------------------------------
//  match
// ---------------------------------------------------------------------------
void Compiler::compilePattern(const Pattern& p, std::size_t subjectSlot,
                              std::vector<std::size_t>& failJumps, std::vector<std::string>& bindings) {
    auto failOnType = [&](const char* typeName, SourceLoc loc) {
        emit(Op::Constant, addConstant(vm_.internString(std::string_view(typeName))), loc);
        pushStack();
        emit(Op::IsInstance, 0, loc);
        popStack(2); pushStack();
        failJumps.push_back(emitJump(Op::JumpIfFalse, loc));
        popStack();
    };

    switch (p.kind) {
        case Pattern::Kind::Wildcard:
            emit(Op::Pop, 0, p.loc);
            popStack();
            break;

        case Pattern::Kind::Literal:
            compileExpr(p.literal);
            pushStack();
            emit(Op::Equal, 0, p.loc);
            popStack(2); pushStack();
            failJumps.push_back(emitJump(Op::JumpIfFalse, p.loc));
            popStack();
            break;

        case Pattern::Kind::Binding: {
            // Паттерн-связывание НЕ снимает субъект: все паттерны обязаны
            // потреблять ровно одно значение (дубликат субъекта), иначе баланс
            // стека между ветвями match разъезжается.
            beginScope();
            declareLocal(p.binding);
            emit(Op::PeekSlot, subjectSlot, p.loc);
            pushStack();
            emit(Op::SetLocal, current_->numLocals - 1, p.loc);
            popStack();
            emit(Op::Pop, 0, p.loc);   // съедаем дубликат
            popStack();
            bindings.push_back(p.binding);
            break;
        }

        case Pattern::Kind::Type: {
            const std::string tn = p.type ? p.type->toString() : std::string("Any");
            emit(Op::Constant, addConstant(vm_.internString(tn)), p.loc);
            pushStack();
            emit(Op::IsInstance, 0, p.loc);
            popStack(2); pushStack();
            failJumps.push_back(emitJump(Op::JumpIfFalse, p.loc));
            popStack();
            if (!p.binding.empty()) {
                beginScope();
                declareLocal(p.binding);
                emit(Op::PeekSlot, subjectSlot, p.loc);
                pushStack();
                emit(Op::SetLocal, current_->numLocals - 1, p.loc);
                popStack();
            }
            emit(Op::Pop, 0, p.loc);
            popStack();
            break;
        }

        case Pattern::Kind::Array:
        case Pattern::Kind::Map: {
            // Деструктурирующий паттерн. Проверка типа НЕ потребляет дубликат
            // (Op::IsInstancePeek) — так баланс стека между ветвями сохраняется.
            const bool isArray = (p.kind == Pattern::Kind::Array);
            emit(Op::Constant, addConstant(vm_.internString(isArray ? "Array" : "Map")), p.loc);
            pushStack();
            emit(Op::IsInstancePeek, 0, p.loc);
            popStack(2); pushStack();
            failJumps.push_back(emitJump(Op::JumpIfFalse, p.loc));
            popStack();

            for (std::size_t i = 0; i < p.sub.size(); ++i) {
                const Pattern& sub = *p.sub[i];
                // Элемент берётся из СЛОТА субъекта (PeekSlot), а не из стека:
                // дубликат при этом не расходуется.
                emit(Op::PeekSlot, subjectSlot, p.loc);
                pushStack();
                if (isArray)
                    emit(Op::Constant, addConstant(Value::integer(static_cast<std::int64_t>(i))), p.loc);
                else
                    emit(Op::Constant, addConstant(vm_.internString(i < p.keys.size() ? p.keys[i] : "")), p.loc);
                pushStack();
                emit(Op::IndexGet, 0, p.loc);
                popStack(2); pushStack();

                beginScope();
                declareLocal(sub.binding.empty() ? format("<destr{}>", i) : sub.binding);
                const std::uint32_t slot = current_->numLocals - 1;
                emit(Op::SetLocal, slot, p.loc);
                popStack();
                if (!sub.binding.empty()) bindings.push_back(sub.binding);

                // Литеральный под-паттерн: сравниваем со слотом.
                if (sub.kind == Pattern::Kind::Literal && sub.literal) {
                    emit(Op::GetLocal, slot, p.loc);
                    pushStack();
                    compileExpr(sub.literal);
                    pushStack();
                    emit(Op::Equal, 0, p.loc);
                    popStack(2); pushStack();
                    failJumps.push_back(emitJump(Op::JumpIfFalse, p.loc));
                    popStack();
                }
            }
            emit(Op::Pop, 0, p.loc);   // съедаем дубликат субъекта
            popStack();
            break;
        }
    }
}

void Compiler::compileMatch(const ExprPtr& subject, const std::vector<std::unique_ptr<MatchCase>>& cases) {
    const SourceLoc loc = subject ? subject->loc : SourceLoc{};
    compileExpr(subject);
    pushStack();
    std::vector<std::size_t> endJumps;

    // Слот кадра, в котором живёт субъект match: нужен паттернам-связываниям,
    // читающим его через PeekSlot без потребления из стека.
    beginScope();
    declareLocal("<matchSubject>");
    const std::uint32_t subjectSlot = current_->numLocals - 1;
    emit(Op::SetLocal, subjectSlot, subject ? subject->loc : SourceLoc{});
    popStack();

    for (const auto& c : cases) {
        emit(Op::GetLocal, subjectSlot, c->loc);
        pushStack();
        std::vector<std::size_t> failJumps;
        std::vector<std::string> bindings;
        const std::uint32_t localsBefore = current_->numLocals;
        const std::uint32_t depthBefore = current_->scopeDepth;
        const std::uint32_t depthAtCaseStart = current_->stackDepth;

        compilePattern(*c->pattern, subjectSlot, failJumps, bindings);
        if (c->guard) {
            compileExpr(c->guard);
            pushStack();
            // JumpIfFalse снимает условие; дубликат субъекта уже потреблён
            // паттерном, поэтому на входе в точку провала стек чист.
            failJumps.push_back(emitJump(Op::JumpIfFalse, c->loc));
            popStack();
        }

        // Ветвь совпала: дубликат субъекта уже потреблён паттерном.
        beginScope();
        bool produced = false;
        for (std::size_t i = 0; i < c->body.size(); ++i) {
            const bool last = (i + 1 == c->body.size());
            if (last && c->body[i]->kind == StmtKind::ExprStmt) { compileExpr(c->body[i]->expr); produced = true; }
            else compileStmt(c->body[i]);
        }
        if (!produced) { emit(Op::Nil, 0, c->loc); pushStack(); }
        endScope();
        endJumps.push_back(emitJump(Op::Jump, c->loc));

        // Точка провала ветви. Сюда прыгают из СЕРЕДИНЫ проверки паттерна,
        // поэтому стек может содержать лишние значения, а таблица локалов —
        // незакрытые связывания. Приводим всё в порядок перед следующей ветвью.
        // Инвариант: каждый паттерн потребляет ровно один дубликат субъекта,
        // guard снимает своё условие через JumpIfFalse. Значит к точке провала
        // стек УЖЕ сбалансирован, и закрывать области паттерна нужно «тихо» —
        // обычный endScope() выдал бы лишний Pop и сломал баланс ветвей.
        const std::size_t failIp = current_->fn->code.size();
        for (std::size_t j : failJumps) patchJumpTo(j, failIp);
        closeScopesSilently(depthBefore, localsBefore);
        (void)depthAtCaseStart;
    }

    // Ни одна ветвь не совпала -> nil.
    emit(Op::Nil, 0, loc);
    pushStack();
    const std::size_t endIp = current_->fn->code.size();
    for (std::size_t j : endJumps) patchJumpTo(j, endIp);
    // Значение match (результат ветви или nil) уже лежит на вершине стека, а
    // endScope() освобождает только слоты локалов — значение не трогает.
    endScope();
}

// ---------------------------------------------------------------------------
//  Присваивание
// ---------------------------------------------------------------------------
void Compiler::compileAssignTarget(const ExprPtr& t, bool compound, CompoundOp op) {
    (void)compound; (void)op;
    switch (t->kind) {
        case ExprKind::Identifier: {
            // Контракт присваивания как ВЫРАЖЕНИЯ: оно потребляет одно значение
            // и оставляет его же на вершине (`let y = (x = 5)`).
            //   SetLocal   — peek (значение остаётся) -> ничего добавлять не нужно;
            //   SetGlobal / SetUpvalue — значение СНИМАЮТ -> возвращаем его Get'ом.
            if (moduleGlobals_.count(t->name)) {
                const std::uint32_t nm = internName(t->name);
                emit(Op::SetGlobal, nm, t->loc);
                emit(Op::GetGlobal, nm, t->loc);
            } else if (auto l = resolveLocal(current_, t->name)) {
                if (current_->locals[*l].isConst)
                    error(t->loc, format("cannot assign to immutable binding '{}'", t->name),
                          "declare it with 'var' instead of 'let'");
                emit(Op::SetLocal, *l, t->loc);
            } else if (auto u = resolveUpvalue(current_, t->name)) {
                emit(Op::SetUpvalue, *u, t->loc);
                emit(Op::GetUpvalue, *u, t->loc);
            } else {
                const std::uint32_t nm = internName(t->name);
                emit(Op::SetGlobal, nm, t->loc);
                emit(Op::GetGlobal, nm, t->loc);
            }
            popStack(); pushStack();
            break;
        }
        case ExprKind::Index:
            // На входе стек: [v] (значение положила вызывающая сторона).
            // Контракт Op::IndexSet — `[obj][key][val]`, поэтому obj и key
            // вычисляются ПОСЛЕ значения и ложатся поверх него; никаких ротаций
            // не требуется. IndexSet возвращает значение на вершину, что и
            // нужно присваиванию-как-выражению (`let y = (a[i] = v)`).
            compileExpr(t->a);
            pushStack();                       // [v][obj]
            compileExpr(t->b);
            pushStack();                       // [v][obj][key]
            emit(Op::IndexSet, 0, t->loc);     // -> [v]
            popStack(3); pushStack();
            break;
        case ExprKind::Member:
            // На входе стек: [v]. Добавляем obj и приводим к контракту
            // Op::MemberSet — `[obj][v]` -> `[v]` (значение на вершине,
            // как у Op::IndexSet). Swap меняет их местами.
            compileExpr(t->a);
            pushStack();                       // [v][obj]
            emit(Op::Swap, 0, t->loc);         // [obj][v]
            emit(Op::MemberSet, internName(t->name), t->loc);
            popStack(2); pushStack();          // [v]
            break;
        default:
            error(t->loc, "invalid assignment target");
            break;
    }
}

// ---------------------------------------------------------------------------
//  Константное свёртывание
// ---------------------------------------------------------------------------
bool Compiler::tryFold(Expr& e) {
    if (!opts_.foldConstants || e.kind != ExprKind::Binary || !e.a || !e.b) return false;
    if (e.a->kind != ExprKind::Number || e.b->kind != ExprKind::Number) return false;
    const double x = e.a->number, y = e.b->number;
    double r = 0;
    switch (e.binOp) {
        case BinOp::Add: r = x + y; break;
        case BinOp::Sub: r = x - y; break;
        case BinOp::Mul: r = x * y; break;
        case BinOp::Div: if (y == 0.0) return false; r = x / y; break;
        case BinOp::Mod: if (y == 0.0) return false; r = std::fmod(x, y); break;
        case BinOp::Pow: r = std::pow(x, y); break;
        default: return false;
    }
    e.kind = ExprKind::Number;
    e.number = r;
    e.a.reset();
    e.b.reset();
    return true;
}

// ---------------------------------------------------------------------------
//  Выражения
// ---------------------------------------------------------------------------
void Compiler::compileExpr(const ExprPtr& e) {
    if (!e) { emit(Op::Nil); pushStack(); return; }
    tryFold(*e);

    switch (e->kind) {
        case ExprKind::Nil:  emit(Op::Nil, 0, e->loc); pushStack(); break;
        case ExprKind::Bool: emit(e->boolean ? Op::True : Op::False, 0, e->loc); pushStack(); break;

        case ExprKind::Number: {
            const double d = e->number;
            if (d == std::floor(d) && std::fabs(d) < 8388607.0)
                emit(Op::IntConst, static_cast<std::uint32_t>(static_cast<std::int32_t>(d)) & 0xFFFFFF, e->loc);
            else
                emit(Op::Constant, addConstant(Value::fromNumber(d)), e->loc);
            pushStack();
            break;
        }

        case ExprKind::String:
            emit(Op::Constant, addConstant(vm_.internString(e->name)), e->loc);
            pushStack();
            break;

        case ExprKind::InterpolatedString: {
            std::uint32_t parts = 0;
            for (std::size_t i = 0; i < e->strs.size(); ++i) {
                if (!e->strs[i].empty()) {
                    emit(Op::Constant, addConstant(vm_.internString(e->strs[i])), e->loc);
                    pushStack(); ++parts;
                }
                // ВАЖНО: pushStack() обязателен — compileExpr кладёт ровно одно
                // значение. Без него счётчик глубины стека расходился с реальным
                // стеком на КАЖДЫЙ интерполированный аргумент, и последующие
                // popStack/endScope (PopN) снимали лишнее: внутри циклов это
                // портило итератор и локалы («for iter 1 <мусор>», -9.2e18).
                if (i < e->args.size()) { compileExpr(e->args[i]); pushStack(); ++parts; }
            }
            if (parts == 0) { emit(Op::Constant, addConstant(vm_.internString("")), e->loc); pushStack(); break; }
            emit(Op::Concat, parts, e->loc);
            popStack(parts); pushStack();
            break;
        }

        case ExprKind::Identifier: {
            // Имена верхнего уровня модуля — ВСЕГДА глобалы, даже если физически
            // занимают слот локалов прототипа <script> (см. moduleGlobals_).
            if (moduleGlobals_.count(e->name))                     emit(Op::GetGlobal, internName(e->name), e->loc);
            else if (auto l = resolveLocal(current_, e->name))     emit(Op::GetLocal, *l, e->loc);
            else if (auto u = resolveUpvalue(current_, e->name))   emit(Op::GetUpvalue, *u, e->loc);
            else                                                   emit(Op::GetGlobal, internName(e->name), e->loc);
            pushStack();
            break;
        }

        case ExprKind::This:
            if (!current_->isMethod) error(e->loc, "'this' is only valid inside a class method");
            emit(Op::GetThis, 0, e->loc);
            pushStack();
            break;

        case ExprKind::Super:
            // Компилятор подставляет ссылку на суперкласс; для `super.method()`
            // VM выполнит поиск метода начиная с него, сохранив this инстанса.
            if (!hasEnclosingSuper_) {
                error(e->loc, "'super' used in a class without a superclass");
                emit(Op::Nil, 0, e->loc);
            } else {
                emit(Op::GetGlobal, internName(enclosingSuper_), e->loc);
            }
            pushStack();
            break;

        case ExprKind::Group:
            compileExpr(e->a);
            break;

        case ExprKind::Unary: {
            compileExpr(e->a);
            emit(e->unOp == UnOp::Neg ? Op::Neg : e->unOp == UnOp::Not ? Op::Not : Op::BitNot, 0, e->loc);
            break;
        }

        case ExprKind::Binary: {
            compileExpr(e->a);
            compileExpr(e->b);
            static const Op kMap[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Mod, Op::Pow,
                                      Op::Equal, Op::NotEqual, Op::Less, Op::Greater, Op::LessEq, Op::GreaterEq,
                                      Op::BitAnd, Op::BitOr, Op::BitXor, Op::Shl, Op::Shr, Op::Concat};
            emit(kMap[static_cast<int>(e->binOp)], 0, e->loc);
            popStack(2); pushStack();
            break;
        }

        case ExprKind::Logical: {
            compileExpr(e->a);
            const bool isOr = (e->name == "or");
            const std::size_t j = emitJump(isOr ? Op::JumpIfTruePop : Op::JumpIfFalsePop, e->loc);
            popStack();
            compileExpr(e->b);
            patchJump(j);
            break;
        }

        case ExprKind::Coalesce: {
            compileExpr(e->a);
            emit(Op::Dup, 0, e->loc);
            pushStack();
            emit(Op::Nil, 0, e->loc);
            pushStack();
            emit(Op::NotEqual, 0, e->loc);
            popStack(2); pushStack();
            const std::size_t j = emitJump(Op::JumpIfTruePop, e->loc);
            popStack();
            compileExpr(e->b);
            patchJump(j);
            break;
        }

        case ExprKind::Ternary: {
            compileExpr(e->a);
            if (e->optionalChain) {          // элвис `a ?: b`
                const std::size_t je = emitJump(Op::Elvis, e->loc);
                compileExpr(e->c);
                patchJump(je);
                break;
            }
            pushStack();
            const std::size_t jf = emitJump(Op::JumpIfFalse, e->loc);
            popStack();
            compileExpr(e->b);
            const std::size_t je = emitJump(Op::Jump, e->loc);
            patchJump(jf);
            compileExpr(e->c);
            patchJump(je);
            break;
        }

        case ExprKind::Assign:
            compileExpr(e->b);
            compileAssignTarget(e->a, false, CompoundOp::Add);
            break;

        case ExprKind::CompoundAssign: {
            static const Op kOp[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Mod, Op::Pow};
            const Op op = kOp[static_cast<int>(e->cmpOp)];
            if (e->a->kind == ExprKind::Identifier) {
                compileExpr(e->a);
                compileExpr(e->b);
                emit(op, 0, e->loc);
                popStack(2); pushStack();
                compileAssignTarget(e->a, true, e->cmpOp);
            } else if (e->a->kind == ExprKind::Member) {
                const std::uint32_t n = internName(e->a->name);
                compileExpr(e->a->a);
                pushStack();
                emit(Op::Dup, 0, e->loc); pushStack();
                emit(Op::MemberGet, n, e->loc);
                compileExpr(e->b);
                emit(op, 0, e->loc);
                popStack(2);                       // [obj] [new]
                emit(Op::MemberSet, n, e->loc);
                popStack(2); pushStack();          // [new]
            } else if (e->a->kind == ExprKind::Index) {
                // `obj[key] <op>= rhs`
                //
                // obj и key вычисляются РОВНО ОДИН РАЗ и всё время лежат в стеке:
                //   Op::IndexGetPeek читает старое значение, НЕ потребляя их
                //   (`[obj][key]` -> `[obj][key][old]`),
                //   Op::IndexSetTop записывает новое в естественном порядке
                //   (`[obj][key][val]` -> `[val]`).
                // Никаких ротаций: прежние попытки перекладывать стек через
                // Dup/Dup1/Swap приводили к тому, что IndexGet получал key
                // вместо obj («string index out of range» на `p["hp"] -= dt`).
                compileExpr(e->a->a);
                pushStack();                              // [obj]
                compileExpr(e->a->b);
                pushStack();                              // [obj] [key]
                emit(Op::IndexGetPeek, 0, e->loc); pushStack();  // [obj] [key] [old]
                compileExpr(e->b);
                pushStack();                              // [obj] [key] [old] [rhs]
                emit(op, 0, e->loc);
                popStack(2); pushStack();                 // [obj] [key] [new]
                emit(Op::IndexSetTop, 0, e->loc);
                popStack(3); pushStack();                 // [new]
                break;
            } else {
                error(e->loc, "invalid compound-assignment target");
            }
            break;
        }

        case ExprKind::Call: {
            // `super.method(args)`: парсер уже поглотил `.method` в узел Super,
            // поэтому вызов супер-метода распознаётся здесь, а не в MethodCall.
            if (e->a && e->a->kind == ExprKind::Super && !e->a->name.empty()) {
                const std::uint32_t n = static_cast<std::uint32_t>(e->args.size());
                const std::uint32_t nameIdx = internName(e->a->name);
                compileExpr(e->a);                    // -> суперкласс
                for (auto& arg : e->args) compileExpr(arg);
                // Та же раскладка, что у Op::CallMethod:
                // `[superClass][a0..aN-1][callee]` (callee = Dup1 от суперкласса).
                emit(Op::Dup1, 0, e->loc); pushStack();
                emit(Op::InvokeSuper, ((nameIdx & 0xFFFF) << 8) | (n & 0xFF), e->loc);
                popStack(static_cast<int>(n) + 2);
                pushStack();
                break;
            }
            compileExpr(e->a);
            for (auto& arg : e->args) compileExpr(arg);
            const std::uint32_t n = static_cast<std::uint32_t>(e->args.size());
            emit(e->variadic ? Op::CallSpread : Op::Call, n, e->loc);
            popStack(static_cast<int>(n) + 1); pushStack();
            break;
        }

        case ExprKind::MethodCall: {
            // Единая раскладка вызова метода: `[receiver][a0..aN-1][callee]`.
            //
            // Компилятор кладёт приёмник, затем аргументы, затем Dup1 копирует
            // приёмник ИЗ-ПОД аргументов на вершину — этот верхний слот VM
            // перезаписывает найденным замыканием метода (callee-слот).
            // База кадра = recvPos+1 (аргументы становятся локалами «на месте»,
            // поэтому первый параметр не затирается callee), а doReturn пишет
            // результат в base-1 = слот приёмника и ставит top = recvPos+1.
            // Итоговый баланс для вызывающего кода: -(n+2) значений, +1 результат.
            const std::uint32_t n = static_cast<std::uint32_t>(e->args.size());
            const std::uint32_t nameIdx = internName(e->name);
            const std::uint32_t operand = ((nameIdx & 0xFFFF) << 8) | (n & 0xFF);
            const bool isSuper = (e->a && e->a->kind == ExprKind::Super);

            std::size_t nilJump = 0;
            compileExpr(e->a);                       // приёмник (или суперкласс)
            if (e->optionalChain)
                nilJump = emitJump(Op::JumpIfNil, e->loc);  // nil остаётся = результат
            for (auto& arg : e->args) compileExpr(arg);
            emit(Op::Dup1, 0, e->loc);               // callee-слот = копия приёмника
            emit(isSuper ? Op::InvokeSuper : Op::CallMethod, operand, e->loc);
            popStack(static_cast<int>(n) + 2);
            pushStack();
            if (e->optionalChain) patchJump(nilJump);
            break;
        }


        case ExprKind::Index:
            compileExpr(e->a);
            compileExpr(e->b);
            emit(Op::IndexGet, 0, e->loc);
            popStack(2); pushStack();
            break;

        case ExprKind::Member:
            compileExpr(e->a);
            emit(e->optionalChain ? Op::MemberGetOpt : Op::MemberGet, internName(e->name), e->loc);
            break;

        case ExprKind::ArrayLit: {
            const std::uint32_t n = static_cast<std::uint32_t>(e->args.size());
            std::uint32_t spreads = 0;
            for (auto& el : e->args) {
                if (el->kind == ExprKind::Spread) { ++spreads; compileExpr(el->a); }
                else compileExpr(el);
            }
            emit(Op::NewArray, n, e->loc);
            popStack(n); pushStack();
            for (std::uint32_t i = 0; i < spreads; ++i) emit(Op::SpreadAppend, 0, e->loc);
            break;
        }

        case ExprKind::MapLit: {
            const std::uint32_t n = static_cast<std::uint32_t>(e->keys.size());
            for (std::uint32_t i = 0; i < n; ++i) { compileExpr(e->keys[i]); compileExpr(e->args[i]); }
            emit(Op::NewMap, n, e->loc);
            popStack(2 * static_cast<int>(n)); pushStack();
            break;
        }

        case ExprKind::TupleLit: {
            const std::uint32_t n = static_cast<std::uint32_t>(e->args.size());
            for (auto& el : e->args) compileExpr(el);
            emit(Op::NewArray, n, e->loc);
            popStack(n); pushStack();
            break;
        }

        case ExprKind::Spread:
            compileExpr(e->a);
            break;

        case ExprKind::New: {
            // Имя класса разрешается во ВРЕМЯ ВЫПОЛНЕНИЯ: `new Foo()` может стоять
            // в файле, который компилируется до объявления Foo (forward reference).
            emit(Op::LoadName, internName(e->name), e->loc);
            pushStack();
            for (auto& arg : e->args) compileExpr(arg);
            const std::uint32_t n = static_cast<std::uint32_t>(e->args.size());
            emit(Op::NewInstance, n, e->loc);
            popStack(static_cast<int>(n) + 1); pushStack();
            break;
        }

        case ExprKind::Range:
            compileExpr(e->a);
            compileExpr(e->b);
            emit(Op::NewRange, e->inclusiveRange ? 1 : 0, e->loc);
            popStack(2); pushStack();
            break;

        case ExprKind::Is: {
            compileExpr(e->a);
            emit(Op::Constant, addConstant(vm_.internString(e->type ? e->type->toString() : "Any")), e->loc);
            pushStack();
            emit(Op::IsInstance, 0, e->loc);
            popStack(2); pushStack();
            break;
        }

        case ExprKind::As: {
            compileExpr(e->a);
            emit(Op::Constant, addConstant(vm_.internString(e->type ? e->type->toString() : "Any")), e->loc);
            pushStack();
            emit(Op::Cast, 0, e->loc);
            popStack(2); pushStack();
            break;
        }

        case ExprKind::Cast:
            if (e->name == "?") { compileExpr(e->a); emit(Op::ResultCheck, 0, e->loc); }
            else compileExpr(e->a);
            break;

        case ExprKind::Lambda:
            compileFunction("<lambda>", e->params, e->body, e->returnType, ObjFunction::Kind::Lambda,
                            /*isCoroutine=*/false, e->loc);
            break;

        case ExprKind::If: {
            compileExpr(e->a);
            pushStack();
            std::size_t j = emitJump(Op::JumpIfFalse, e->loc);
            popStack();
            beginScope(); compileBody(e->stmts, true); endScope();
            std::vector<std::size_t> endJumps{emitJump(Op::Jump, e->loc)};
            patchJump(j);
            for (std::size_t i = 0; i < e->elifConds.size(); ++i) {
                compileExpr(e->elifConds[i]);
                pushStack();
                const std::size_t ej = emitJump(Op::JumpIfFalse, e->loc);
                popStack();
                beginScope(); compileBody(e->elifBodies[i], true); endScope();
                endJumps.push_back(emitJump(Op::Jump, e->loc));
                patchJump(ej);
            }
            if (!e->elseBody.empty()) { beginScope(); compileBody(e->elseBody, true); endScope(); }
            else { emit(Op::Nil, 0, e->loc); pushStack(); }
            for (std::size_t ej : endJumps) patchJump(ej);
            break;
        }

        case ExprKind::While: {
            // while-выражение возвращает значение последней итерации тела.
            // Аккумулятор живёт в СКРЫТОМ ЛОКАЛЕ, а не в стеке: это делает
            // дисциплину стека тривиальной (break/continue не требуют Swap/Pop).
            emit(Op::Nil, 0, e->loc);
            pushStack();
            beginScope();
            declareLocal("<whileAcc>");
            const std::uint32_t accSlot = current_->numLocals - 1;
            emit(Op::SetLocal, accSlot, e->loc);   // peek-запись
            emit(Op::Pop, 0, e->loc);
            popStack();

            const std::uint32_t hiddenBase = current_->numLocals;
            const std::size_t start = current_->fn->code.size();
            compileExpr(e->a);
            pushStack();
            const std::size_t exitJump = emitJump(Op::JumpIfFalse, e->loc);
            popStack();

            LoopContext lc;
            lc.startIp = start;
            lc.hiddenBase = hiddenBase;
            lc.isExprLoop = true;
            loops_.push_back(lc);

            beginScope();
            compileBody(e->stmts, true);           // [bodyValue]
            emit(Op::SetLocal, accSlot, e->loc);   // acc = bodyValue (peek)
            emit(Op::Pop, 0, e->loc);              // значение итерации снято
            popStack();
            endScope();

            loops_.back().breakExitIp = current_->fn->code.size();
            emitLoop(start, e->loc);
            patchJump(exitJump);
            for (std::size_t c : loops_.back().continueJumps) patchJumpTo(c, start);
            for (std::size_t b : loops_.back().breakJumps) patchJumpTo(b, loops_.back().breakExitIp);
            loops_.pop_back();

            // Тело while-выражения оставляет своё значение на вершине стека, а
            // endScope() больше не снимает ничего — значит значение уже там,
            // где его ожидает вызывающий код. Аккумулятор нужен только для
            // ветки «цикл не выполнился ни разу» (см. Op::Nil перед beginScope).
            (void)accSlot;
            endScope();
            break;
        }

        case ExprKind::For: {
            // List comprehension: `[for x in coll do expr]` -> массив результатов.
            //
            // Аккумулятор объявляется ДО служебных слотов цикла: ForRangePrep
            // пишет в [varSlot..varSlot+2], и если бы acc оказался внутри этой
            // области, он был бы затёрт. Порядок слотов: [<acc>][i][<end>][<step>].
            beginScope();
            emit(Op::EmptyArray, 0, e->loc);
            pushStack();
            declareLocal("<acc>");
            const std::uint32_t accSlot = current_->numLocals - 1;
            emit(Op::SetLocal, accSlot, e->loc);   // peek-запись
            emit(Op::Pop, 0, e->loc);
            popStack();

            const std::string var2 = e->strs.empty() ? std::string() : e->strs[0];
            compileLoopCommon(
                e->a, e->name, var2,
                [&] {
                    for (std::size_t i = 0; i + 1 < e->stmts.size(); ++i) compileStmt(e->stmts[i]);
                    if (!e->stmts.empty() && e->stmts.back()->kind == StmtKind::ExprStmt && e->stmts.back()->expr)
                        compileExpr(e->stmts.back()->expr);
                    else { emit(Op::Nil, 0, e->loc); pushStack(); }
                    emitArrayPushLocal(accSlot, e->loc);
                },
                {}, e->loc);

            // endScope() освобождает слоты (включая acc и служебные слоты цикла),
            // но НЕ трогает стек: после цикла стек пуст, поэтому читаем
            // аккумулятор из слота явно.
            endScope();
            emit(Op::GetLocal, accSlot, e->loc);
            pushStack();
            break;
        }

        case ExprKind::Match:
            compileMatch(e->a, e->cases);
            break;

        case ExprKind::Block:
            beginScope();
            compileBody(e->stmts, true);
            endScope();
            break;

        case ExprKind::Yield:
            if (!inCoroutine_) error(e->loc, "'yield' is only allowed inside a 'coroutine' function");
            if (e->a) compileExpr(e->a);
            else { emit(Op::Nil, 0, e->loc); pushStack(); }
            emit(Op::Yield, 1, e->loc);
            break;

        case ExprKind::Resume:
            compileExpr(e->a);
            pushStack();
            emit(Op::Resume, 0, e->loc);
            break;

        case ExprKind::TryResult: {
            const std::size_t tb = emitJump(Op::TryBegin, e->loc);
            compileExpr(e->a);
            emit(Op::ResultOk, 0, e->loc);
            const std::size_t je = emitJump(Op::Jump, e->loc);
            patchJump(tb);
            emit(Op::TryEnd, 0, e->loc);
            emit(Op::ResultErr, 0, e->loc);
            patchJump(je);
            break;
        }

        case ExprKind::Await:
        case ExprKind::IndexAssign:
        case ExprKind::MemberAssign:
            compileExpr(e->a);
            break;
    }
}

// ---------------------------------------------------------------------------
//  Точка входа
// ---------------------------------------------------------------------------
ObjFunction* Compiler::compile(const Module& m, DiagnosticList& diags, const Options& opts) {
    diags_ = &diags;
    opts_ = opts;
    allProtos_.clear();
    nextProtoId_ = 0;
    loops_.clear();
    inCoroutine_ = false;
    inLoop_ = false;
    moduleGlobals_.clear();

    beginFunction("<script>", ObjFunction::Kind::Script, false, false, {});
    current_->fn->source = m.name;
    // numLocals/paramSlotCount уже выставлены beginFunction (для скрипта = 0);
    // сбрасывать их вручную нельзя: declareLocal увеличивает оба счётчика.
    current_->scopeDepth = 0;

    // ВНИМАНИЕ: тело модуля НЕ оборачивается в beginScope/endScope.
    // Объявления верхнего уровня живут в слотах кадра на протяжении всего
    // скрипта (это аналог «globals модуля»), поэтому numLocals прототипа
    // должен равняться ПИКОВОМУ числу занятых слотов. Если обернуть тело
    // в область и снять её, endScope() обнулил бы numLocals, и VM положила бы
    // область временных значений прямо на слоты локалов.
    for (const auto& st : m.body) compileStmt(st);
    emit(Op::Halt, 0, {});

    ObjFunction* fn = endFunction();
    return fn;
}

ObjFunction* Compiler::compileExpression(const ExprPtr& e, DiagnosticList& diags, const Options& opts) {
    diags_ = &diags;
    opts_ = opts;
    allProtos_.clear();
    nextProtoId_ = 0;
    loops_.clear();
    inCoroutine_ = false;
    inLoop_ = false;
    moduleGlobals_.clear();

    beginFunction("<repl>", ObjFunction::Kind::Script, false, false, {});
    current_->fn->source = "<repl>";
    current_->scopeDepth = 0;

    compileExpr(e);
    emit(Op::Return, 0, e->loc);
    return endFunction();
}

} // namespace lv
