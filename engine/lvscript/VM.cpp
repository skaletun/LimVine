/**
 * @file    VM.cpp
 * @brief   Реализация стековой виртуальной машины LV Script: интерпретатор,
 *          встроенные типы, корутины, песочница, трейсбек.
 *
 * Соглашения стека (инварианты интерпретатора)
 * --------------------------------------------
 * 1. Каждая инструкция оставляет стек сбалансированным относительно своей
 *    документированной сигнатуры (см. Bytecode.h).
 * 2. При вызове callee лежит СРАЗУ ПОД аргументами: `[callee][a0..aN-1]`.
 *    `callClosure` копирует аргументы в слоты локалов нового кадра.
 * 3. Переключение контекста (корутина) всегда восстанавливает `framePtr/fn/code`
 *    через `refreshFrame()`. Пропуск этого шага — классический источник багов,
 *    поэтому он вынесен в одну функцию и вызывается во всех ветках.
 */
#include "VM.h"
#include "Scheduler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace lv {

namespace {
constexpr std::uint32_t kFrameTempSlots = 256;

/// Безопасное нисходящее приведение кучевого объекта.
/// (Тип уже проверен вызывающим кодом через `ObjHeader::type`.)
template <class T>
inline T* objAs(ObjHeader* h) noexcept { return static_cast<T*>(h); }
template <class T>
inline T* objAs(const Value& v) noexcept { return static_cast<T*>(v.asObject()); }
template <class T>
inline T* objAs(Value* v) noexcept { return static_cast<T*>(v->asObject()); }
} // namespace

// ---------------------------------------------------------------------------
//  Конструктор / деструктор
// ---------------------------------------------------------------------------
VM::VM(SandboxConfig sandbox) : gc_(this), sandbox_(std::move(sandbox)) {
    gc_.setMemoryLimit(sandbox_.memoryLimit);
    rngState_ = sandbox_.randomSeed;

    globals_      = lv::makeMap(gc_);
    modules_      = lv::makeMap(gc_);
    natives_      = lv::makeMap(gc_);
    methodTables_ = lv::makeMap(gc_);
    GC::pin(globals_); GC::pin(modules_); GC::pin(natives_); GC::pin(methodTables_);

    auto mainCtx = std::make_unique<ExecutionContext>();
    mainCtx->reserve();
    for (auto& v : mainCtx->values) v = Value::nil();
    ctx_ = mainCtx.get();
    mainCtx_ = ctx_;
    ownedContexts_.push_back(std::move(mainCtx));
    contexts_.push_back(ctx_);
}

VM::~VM() = default;

void VM::setSandbox(const SandboxConfig& s) {
    sandbox_ = s;
    gc_.setMemoryLimit(s.memoryLimit);
    rngState_ = s.randomSeed;
}

void VM::requireCapability(std::uint32_t cap, std::string_view what) {
    if (hasCapability(cap)) return;
    char hexbuf[16];
    std::snprintf(hexbuf, sizeof(hexbuf), "0x%X", cap);
    runtimeError(format("sandbox violation: '{}' requires capability {}", what, std::string(hexbuf)));
}

std::uint64_t VM::nextRandom() noexcept {
    // xorshift64* — быстрый детерминированный PRNG (воспроизводимость в песочнице).
    std::uint64_t& s = rngState_;
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 2685821657736338717ULL;
}

// ---------------------------------------------------------------------------
//  Интернирование и фабрики
// ---------------------------------------------------------------------------
ObjString* VM::internRaw(std::string_view s) {
    const std::uint64_t h = hashString(s);
    auto it = interned_.find(h);
    if (it != interned_.end() && it->second->view() == s) return it->second;
    ObjString* str = makeRawString(gc_, s, h);
    GC::pin(str);
    interned_[h] = str;
    return str;
}

Value VM::internString(std::string_view s) { return Value::object(internRaw(s)); }

std::uint32_t VM::internName(const std::string& s) {
    ObjString* str = internRaw(s);
    for (std::uint32_t i = 0; i < names_.size(); ++i)
        if (names_[i] == str) return i;
    names_.push_back(str);
    return static_cast<std::uint32_t>(names_.size() - 1);
}

Value VM::makeArray(std::uint32_t capacity) { return Value::object(lv::makeArray(gc_, capacity)); }
Value VM::makeMap()                         { return Value::object(lv::makeMap(gc_)); }
Value VM::makeResult(bool ok, Value v, Value e) { return Value::object(lv::makeResult(gc_, ok, v, e)); }

Value VM::makeClosure(ObjFunction* fn) {
    auto* c = gc_.allocate<ObjClosure>(sizeof(ObjClosure), ObjHeader(ObjHeader::Type::Closure, sizeof(ObjClosure)));
    c->function = fn;
    c->upvalues = nullptr;
    if (fn->numUpvalues) {
        c->upvalues = static_cast<ObjUpvalue**>(gc_.reallocate(nullptr, 0, fn->numUpvalues * sizeof(ObjUpvalue*)));
        for (std::uint32_t i = 0; i < fn->numUpvalues; ++i) c->upvalues[i] = nullptr;
    }
    return Value::object(c);
}

Value VM::makeCoroutine(ObjClosure* root) {
    auto* co = gc_.allocate<ObjCoroutine>(sizeof(ObjCoroutine), ObjHeader(ObjHeader::Type::Coroutine, sizeof(ObjCoroutine)));
    co->root = root;
    co->state = ObjCoroutine::State::Created;
    co->resumeValue = Value::nil();
    co->yieldValue = Value::nil();
    return Value::object(co);
}

ObjClass* VM::declareClass(std::string_view name, ObjClass* super) {
    ObjString* n = internRaw(name);
    // Повторное объявление (hot reload / REPL) переиспользует объект класса,
    // чтобы живые инстансы не потеряли идентичность.
    if (Value* existing = globals_->find(Value::object(n))) {
        if (existing->isObject() && existing->asObject()->type == ObjHeader::Type::Class) {
            auto* c = existing->as<ObjClass>();
            if (super) c->superclass = super;
            return c;
        }
    }
    ObjClass* c = lv::makeClass(gc_, n);
    c->superclass = super;
    return c;
}

// ---------------------------------------------------------------------------
//  Глобалы / модули / нативные функции
// ---------------------------------------------------------------------------
void VM::setGlobal(std::string_view name, Value v) { globals_->set(gc_, internString(name), v); }

Value* VM::findGlobal(std::string_view name) {
    ObjString* s = internRaw(name);
    return globals_->findString(s->view(), s->hash);
}

Value VM::getGlobal(std::string_view name) {
    if (Value* v = findGlobal(name)) return *v;
    return Value::nil();
}

bool VM::removeGlobal(std::string_view name) { return globals_->remove(internString(name)); }

void VM::setModule(std::string_view name, Value v) { modules_->set(gc_, internString(name), v); }

Value* VM::findModule(std::string_view name) {
    ObjString* s = internRaw(name);
    return modules_->findString(s->view(), s->hash);
}

void VM::registerNative(std::string_view name, std::uint32_t arity, ObjNativeFn::Fn fn,
                        std::uint32_t cap, bool yielding) {
    auto* n = gc_.allocate<ObjNativeFn>(sizeof(ObjNativeFn), ObjHeader(ObjHeader::Type::NativeFn, sizeof(ObjNativeFn)));
    n->name = internRaw(name);
    n->arity = arity;
    n->fn = std::move(fn);
    n->yielding = yielding;
    n->requiredCapability = cap;
    GC::pin(n);
    natives_->set(gc_, Value::object(n->name), Value::object(n));
    setGlobal(name, Value::object(n));
}

void VM::registerMethod(std::string_view typeName, std::string_view methodName, std::uint32_t arity,
                        ObjNativeFn::Fn fn, std::uint32_t cap) {
    const Value tblKey = internString(typeName);
    ObjMap* tbl = nullptr;
    if (Value* slot = methodTables_->find(tblKey)) tbl = slot->as<ObjMap>();
    if (!tbl) {
        tbl = lv::makeMap(gc_);
        GC::pin(tbl);
        methodTables_->set(gc_, tblKey, Value::object(tbl));
    }
    auto* n = gc_.allocate<ObjNativeFn>(sizeof(ObjNativeFn), ObjHeader(ObjHeader::Type::NativeFn, sizeof(ObjNativeFn)));
    n->name = internRaw(methodName);
    n->arity = arity;
    n->fn = std::move(fn);
    n->requiredCapability = cap;
    GC::pin(n);
    tbl->set(gc_, Value::object(n->name), Value::object(n));
}

// ---------------------------------------------------------------------------
//  Корни GC
// ---------------------------------------------------------------------------
void VM::markRoots(GC& gc) {
    gc.markObject(globals_);
    gc.markObject(modules_);
    gc.markObject(natives_);
    gc.markObject(methodTables_);
    for (ObjString* s : names_) gc.markObject(s);
    for (auto& [h, s] : interned_) gc.markObject(s);

    for (ExecutionContext* c : contexts_) {
        if (!c) continue;
        const std::size_t top = std::min(c->top, c->values.size());
        for (std::size_t i = 0; i < top; ++i) gc.mark(c->values[i]);
        for (std::size_t i = 0; i < c->frameCount; ++i) {
            CallFrame& f = c->frames[i];
            gc.markObject(f.closure);
            gc.mark(f.thisValue);
            gc.mark(f.superValue);
        }
    }
    for (ObjUpvalue* uv = openUpvalues_; uv; uv = uv->nextOpen) gc.markObject(uv);
    for (Value v : scratchArgs_) gc.mark(v);
    gc.mark(pendingInstanceOverride_);
    if (scheduler_) scheduler_->markRoots(gc);
}

// ---------------------------------------------------------------------------
//  Ошибки и трейсбек
// ---------------------------------------------------------------------------
void VM::runtimeError(std::string msg) {
    error_.message = std::move(msg);
    if (ctx_ && ctx_->frameCount > 0) {
        const CallFrame& f = ctx_->frames[ctx_->frameCount - 1];
        if (f.closure && f.closure->function) {
            error_.source = f.closure->function->source;
            error_.line = f.closure->function->lineAt(f.ip);
        }
    }
    if (errorHandler_) errorHandler_(error_, traceback());
}

std::string VM::traceback() const {
    std::string out;
    if (!ctx_) return out;
    for (std::size_t i = ctx_->frameCount; i-- > 0;) {
        const CallFrame& f = ctx_->frames[i];
        const ObjFunction* fn = f.closure ? f.closure->function : nullptr;
        out += format("  at {} ({}:{})\n",
                           fn && fn->name ? std::string(fn->name->view()) : std::string("<script>"),
                           fn ? fn->source : std::string("?"),
                           fn ? fn->lineAt(f.ip) : 0);
    }
    return out;
}

void VM::resetStack() {
    // Сбрасываем ТОЛЬКО основной контекст.
    //
    // Кадры приостановленных корутин трогать нельзя: раньше здесь был цикл по
    // всем контекстам, из-за чего ЛЮБОЙ вызов скриптового колбэка из C++
    // (dispatch("update"), callMethod, callValue) обнулял frameCount спящих
    // корутин. После этого tickRun() возвращался немедленно, и wait() больше
    // никогда не возобновлялся — корутина «зависала» в состоянии Running.
    if (mainCtx_) { mainCtx_->top = 0; mainCtx_->frameCount = 0; }
    openUpvalues_ = nullptr;
    pendingInstanceOverride_ = Value::nil();
    pendingOverrideFrameDepth_ = 0;
}

// ---------------------------------------------------------------------------
//  Вспомогательные функции интерпретатора
// ---------------------------------------------------------------------------
ObjClosure* VM::findMethod(ObjClass* klass, ObjString* name) {
    for (ObjClass* c = klass; c; c = c->superclass) {
        if (Value* m = c->methods->find(Value::object(name))) {
            if (m->isObject() && m->asObject()->type == ObjHeader::Type::Closure)
                return m->as<ObjClosure>();
        }
    }
    return nullptr;
}

bool VM::callClosure(ObjClosure* closure, int argCount) {
    return callClosureAt(closure, argCount, ctx_->top - static_cast<std::size_t>(argCount));
}

bool VM::callClosureAt(ObjClosure* closure, int argCount, std::size_t argsBase, std::size_t calleeSlot) {
    if (ctx_->frameCount >= static_cast<std::size_t>(sandbox_.maxCallDepth)) {
        runtimeError(format("maximum call depth ({}) exceeded — infinite recursion?", sandbox_.maxCallDepth));
        return false;
    }
    ObjFunction* fn = closure->function;
    if (ctx_->frameCount >= ctx_->frames.size()) { runtimeError("call frame overflow"); return false; }

    // Стековая раскладка вызова и слот результата
    // ---------------------------------------------------------------
    // Свободная функция:  [callee][a0..aN-1]      -> argsBase = top-argc
    // Метод:              [recv][a0..aN-1][callee] -> argsBase = recvPos+1
    //
    // В обоих случаях `base` нового кадра = argsBase (первый аргумент = локал №0,
    // поэтому параметр никогда не затирается callee), а СЛОТ РЕЗУЛЬТАТА =
    // argsBase-1 — ячейка непосредственно под аргументами:
    //   * для функции это слот callee,
    //   * для метода  — слот приёмника.
    // doReturn пишет результат туда и ставит top = argsBase, то есть «весь блок
    // вызова заменён ровно одним значением». Любое отклонение от этого правила
    // сдвигает стек вызывающей функции на единицу, и следующий вызов принимает
    // результат предыдущего за callee («attempt to call a Bool value»).
    const std::size_t base = argsBase;
    const std::size_t resultSlot = (argsBase > 0) ? argsBase - 1 : 0;
    (void)calleeSlot;
    CallFrame& frame = ctx_->frames[ctx_->frameCount++];
    frame.closure = closure;
    frame.ip = 0;
    frame.base = static_cast<std::uint32_t>(base);
    frame.calleeSlot = static_cast<std::uint32_t>(resultSlot);
    frame.numLocals = fn->numLocals;
    frame.tempBase = frame.base + fn->numLocals;
    frame.thisValue = Value::nil();
    frame.superValue = Value::nil();
    frame.iterSlot = frame.varSlot = frame.varSlot2 = 0;

    // ВЕРШИНА стека после входа в функцию = base + numLocals: область временных
    // значений ПУСТА. Раньше здесь резервировалось kFrameTempSlots слотов, из-за
    // чего первый же PUSH попадал в base+numLocals+256 и перезаписывал локалы.
    const std::size_t newTop = base + fn->numLocals;
    if (newTop + kFrameTempSlots > ctx_->values.size()) {
        runtimeError("value stack overflow");
        --ctx_->frameCount;
        return false;
    }

    // Сначала упаковываем variadic-хвост: слот rest-параметра перекрывает один
    // из аргументных слотов (для `func f(args...)` restParamSlot == 0 == argsBase),
    // поэтому копирование аргументов затёрло бы последний из них.
    Value restValue;
    if (fn->restParamSlot >= 0) {
        const std::int64_t named = fn->restParamSlot;
        const std::int64_t extra = static_cast<std::int64_t>(argCount) - named;
        ObjArray* arr = lv::makeArray(gc_, extra > 0 ? static_cast<std::uint32_t>(extra) : 0);
        for (std::int64_t i = 0; i < extra; ++i) {
            arr->grow(gc_, arr->count + 1);
            arr->items[arr->count++] = ctx_->values[argsBase + static_cast<std::size_t>(named + i)];
        }
        restValue = Value::object(arr);
    }

    // Копируем аргументы в слоты локалов; остальные слоты инициализируем nil.
    for (std::uint32_t i = 0; i < fn->numLocals; ++i) {
        if (static_cast<int>(i) < argCount) ctx_->values[base + i] = ctx_->values[argsBase + i];
        else ctx_->values[base + i] = Value::nil();
    }
    ctx_->top = newTop;

    if (fn->restParamSlot >= 0)
        ctx_->values[base + static_cast<std::uint32_t>(fn->restParamSlot)] = restValue;
    if (profiling_) ++profile_.calls;
    return true;
}

bool VM::callNative(ObjNativeFn* native, int argCount) {
    if (native->requiredCapability && !hasCapability(native->requiredCapability)) {
        runtimeError(format("sandbox violation: '{}' is not available in this context",
                            std::string(native->name->view())));
        return false;
    }
    const std::size_t argsBase = ctx_->top - static_cast<std::size_t>(argCount);
    std::span<const Value> args(ctx_->values.data() + argsBase, static_cast<std::size_t>(argCount));
    // ЕДИНЫЙ КОНТРАКТ ВЫЗОВА (см. также callNativeMethod/callNativeFree):
    //   стек до:    [calleeSlot][a0..aN-1]
    //   стек после: [result] на месте calleeSlot, top = calleeSlot + 1
    // Для свободной функции calleeSlot = argsBase-1, поэтому снимаем аргументы
    // И callee-слот, а push() ниже кладёт результат ровно на его место.
    ctx_->top = (argsBase > 0) ? argsBase - 1 : 0;
    Value result;
    try {
        result = native->fn(*this, args);
    } catch (const std::bad_alloc&) {
        runtimeError("script memory limit exceeded (sandbox)");
        return false;
    } catch (const std::exception& ex) {
        runtimeError(std::string("native error: ") + ex.what());
        return false;
    }
    if (suspendRequested_) return true;   // значение не пушим: вернёмся после resume
    ctx_->push(result);
    if (profiling_) ++profile_.calls;
    return true;
}

bool VM::invokeBuiltinMethod(ObjHeader* obj, ObjString* name, int argCount) {
    const char* tn = nullptr;
    switch (obj->type) {
        case ObjHeader::Type::Array:     tn = "Array"; break;
        case ObjHeader::Type::String:    tn = "String"; break;
        case ObjHeader::Type::Map:       tn = "Map"; break;
        case ObjHeader::Type::Instance:  tn = "Instance"; break;
        case ObjHeader::Type::Class:     tn = "Class"; break;
        case ObjHeader::Type::Coroutine: tn = "Coroutine"; break;
        case ObjHeader::Type::Result:    tn = "Result"; break;
        default: break;
    }
    if (tn) {
        if (Value* tblSlot = methodTables_->find(internString(tn))) {
            if (Value* m = objAs<ObjMap>(tblSlot)->find(Value::object(name)))
                return callNativeMethod(static_cast<ObjNativeFn*>(m->asObject()), argCount);
        }
    }
    runtimeError(format("'{}' has no method '{}'", tn ? tn : obj->typeName(), std::string(name->view())));
    return false;
}

bool VM::callNativeMethod(ObjNativeFn* native, int argCount) {
    if (native->requiredCapability && !hasCapability(native->requiredCapability)) {
        runtimeError(format("sandbox violation: '{}' is not available in this context",
                            std::string(native->name->view())));
        return false;
    }
    // Стек: [receiver][a0..aN-1][callee]. Нативному методу нужен span
    // {receiver, a0..aN-1}.
    //
    // ЕДИНЫЙ КОНТРАКТ ВЫЗОВА — результат всегда ложится в `argsBase - 1`,
    // то есть в слот НЕПОСРЕДСТВЕННО ПОД аргументами, а top = argsBase:
    //   свободная функция  [callee][a0..aN-1]           -> result в callee-слот;
    //   метод              [recv][a0..aN-1][callee]     -> result в слот recv;
    //   нативная ф-я поля  [recv][a0..aN-1][callee]     -> result в слот recv.
    // Для метода argsBase = recvPos+1, значит слот результата = recvPos, и
    // push() ниже обязан писать в values[recvPos] — для этого top = recvPos.
    //
    // Если вместо этого ставить top = recvPos+1 (как было в одной из ревизий),
    // результат оказывался на слот ВЫШЕ, чем у скриптового метода, и цепочки
    // вызовов (`[1,2,3].map(...).filter(...)`) принимали приёмник за callee:
    // «value of type Array is not callable».
    scratchArgs_.assign(static_cast<std::size_t>(argCount) + 1, Value::nil());
    const std::size_t recvPos = ctx_->top - static_cast<std::size_t>(argCount) - 2;
    scratchArgs_[0] = ctx_->values[recvPos];
    for (int i = 0; i < argCount; ++i)
        scratchArgs_[static_cast<std::size_t>(i) + 1] = ctx_->values[recvPos + 1 + static_cast<std::size_t>(i)];
    ctx_->top = recvPos;

    Value result;
    try {
        result = native->fn(*this, std::span<const Value>(scratchArgs_.data(), scratchArgs_.size()));
    } catch (const std::bad_alloc&) {
        runtimeError("script memory limit exceeded (sandbox)");
        scratchArgs_.clear();
        return false;
    } catch (const std::exception& ex) {
        runtimeError(std::string("native error: ") + ex.what());
        scratchArgs_.clear();
        return false;
    }
    scratchArgs_.clear();
    if (suspendRequested_) return true;
    ctx_->push(result);
    if (profiling_) ++profile_.calls;
    return true;
}

bool VM::callNativeFree(ObjNativeFn* native, int argCount) {
    if (native->requiredCapability && !hasCapability(native->requiredCapability)) {
        runtimeError(format("sandbox violation: '{}' is not available in this context",
                            std::string(native->name->view())));
        return false;
    }
    // Стек: [receiver][a0..aN-1][callee].
    const std::size_t recvPos = ctx_->top - static_cast<std::size_t>(argCount) - 2;
    scratchArgs_.assign(static_cast<size_t>(argCount), Value::nil());
    for (int i = 0; i < argCount; ++i)
        scratchArgs_[static_cast<std::size_t>(i)] = ctx_->values[recvPos + 1 + static_cast<std::size_t>(i)];
    // Единый контракт вызова: результат = argsBase-1. Здесь argsBase = recvPos+1
    // (приёмник НЕ передаётся в функцию-поле), поэтому результат ложится в
    // recvPos — слот приёмника, как и у скриптового метода.
    ctx_->top = recvPos;

    Value result;
    try {
        result = native->fn(*this, std::span<const Value>(scratchArgs_.data(), scratchArgs_.size()));
    } catch (const std::bad_alloc&) {
        runtimeError("script memory limit exceeded (sandbox)");
        scratchArgs_.clear();
        return false;
    } catch (const std::exception& ex) {
        runtimeError(std::string("native error: ") + ex.what());
        scratchArgs_.clear();
        return false;
    }
    scratchArgs_.clear();
    if (suspendRequested_) return true;
    ctx_->push(result);
    if (profiling_) ++profile_.calls;
    return true;
}

bool VM::invokeMethod(Value receiver, ObjString* name, int argCount) {
    if (!receiver.isObject()) {
        runtimeError(format("cannot call method '{}' on {}", std::string(name->view()), receiver.typeName()));
        return false;
    }
    ObjHeader* o = receiver.asObject();

    // Раскладка стека: [receiver][a0..aN-1][callee].
    //   recvPos   = приёмник, recvPos+1 = первый аргумент, top-1 = callee-слот.
    // doReturn пишет результат в base-1 = recvPos, то есть приёмник заменяется
    // результатом: баланс «receiver + args + callee» -> «1 результат».
    const std::size_t recvPos = ctx_->top - static_cast<std::size_t>(argCount) - 2;

    ObjClass* cls = nullptr;
    if (o->type == ObjHeader::Type::Instance) cls = static_cast<ObjInstance*>(o)->klass;
    else if (o->type == ObjHeader::Type::Class) cls = static_cast<ObjClass*>(o);

    if (cls) {
        if (ObjClosure* m = findMethod(cls, name)) {
            // Слот приёмника становится callee-слотом, база кадра — recvPos+1:
            // аргументы копируются в локалы «на месте», и первый параметр НЕ
            // затирается замыканием метода.
            ctx_->values[ctx_->top - 1] = Value::object(m);
            if (!callClosureAt(m, argCount, recvPos + 1, ctx_->top - 1)) return false;
            CallFrame& f = ctx_->frames[ctx_->frameCount - 1];
            f.thisValue = receiver;
            f.superValue = cls->superclass ? Value::object(cls->superclass) : Value::nil();
            return true;
        }
    }
    // Поле-колбэк инстанса: `this.onHit(x)`, `obj.handler(a, b)`.
    //
    // Иначе вызов падал с «'Instance' has no method 'onPlayerHit'», хотя поле
    // существовало и содержало лямбду: findMethod() ищет только в таблице
    // методов КЛАССА. Семантика та же, что у функции-поля map'а: приёмник НЕ
    // передаётся неявным первым аргументом (это не «метод», а значение-функция,
    // которое владелец объекта подставил в поле).
    if (o->type == ObjHeader::Type::Instance) {
        auto* inst = static_cast<ObjInstance*>(o);
        if (Value* fv = inst->fields ? inst->fields->find(Value::object(name)) : nullptr) {
            if (fv->isObject()) {
                ObjHeader* fo = fv->asObject();
                if (fo->type == ObjHeader::Type::Closure) {
                    ctx_->values[ctx_->top - 1] = *fv;
                    return callClosureAt(objAs<ObjClosure>(fo), argCount, recvPos + 1);
                }
                if (fo->type == ObjHeader::Type::NativeFn)
                    return callNativeFree(objAs<ObjNativeFn>(fo), argCount);
            }
        }
    }

    // Модули и таблицы колбэков: `math.min(a, b)`, `handlers.onHit(x)`.
    // Поле map'а, содержащее вызываемое значение, трактуется как метод:
    //   * Closure  — вызывается БЕЗ приёмника (это обычная функция-поле);
    //   * NativeFn — вызывается КАК метод, то есть приёмник передаётся
    //     первым аргументом (см. callNativeMethod).
    if (o->type == ObjHeader::Type::Map) {
        if (Value* fv = static_cast<ObjMap*>(o)->find(Value::object(name))) {
            if (fv->isObject()) {
                ObjHeader* fo = fv->asObject();
                if (fo->type == ObjHeader::Type::Closure) {
                    ctx_->values[ctx_->top - 1] = *fv;
                    return callClosureAt(objAs<ObjClosure>(fo), argCount, recvPos + 1);
                }
                if (fo->type == ObjHeader::Type::NativeFn)
                    return callNativeFree(objAs<ObjNativeFn>(fo), argCount);
            }
        }
    }
    return invokeBuiltinMethod(o, name, argCount);
}

void VM::closeUpvalues(std::uint32_t fromIndex) {
    Value* limit = &ctx_->values[fromIndex];
    while (openUpvalues_ && openUpvalues_->location >= limit) {
        ObjUpvalue* uv = openUpvalues_;
        uv->closed = *uv->location;
        uv->location = &uv->closed;
        // Значение переезжает из стека (корень) в кучу: если upvalue уже
        // чёрный, для инкрементального GC это новая ссылка.
        gc_.writeBarrier(uv, uv->closed);
        openUpvalues_ = uv->nextOpen;
    }
}

bool VM::doReturn() {
    if (ctx_->frameCount == 0) return false;
    CallFrame& frame = ctx_->frames[--ctx_->frameCount];
    Value result = ctx_->pop();

    // Инициализатор ВСЕГДА возвращает свой инстанс.
    //
    // Правило покадровое (kind кадра), а не глобальное: прежний механизм
    // «pendingInstanceOverride_» срабатывал по глубине стека кадров и поэтому
    // ломался на ВЛОЖЕННЫХ конструкторах — `new Inventory()` внутри своего init
    // вызывал `new Slot()`, подмена результата init'а Slot'а «съедала» флаг, и
    // внешний `new Inventory(...)` возвращал nil.
    if (frame.closure && frame.closure->function &&
        frame.closure->function->kind == ObjFunction::Kind::Initializer &&
        !frame.thisValue.isNil())
        result = frame.thisValue;
    closeUpvalues(frame.base);
    // Результат записывается в слот ПОД callee-слотом, а вершина стека
    // устанавливается на calleeSlot: ровно туда, где вызывающий код ожидает
    // увидеть одно значение вместо всего блока «callee/receiver + аргументы».
    //   свободная функция: [callee][a0..aN-1] -> результат в callee-слот, top = argsBase;
    //   метод:             [recv][a0..aN-1][callee] -> результат в слот recv, top = recv+1.
    // Дополнительные push() здесь НЕ нужны — иначе стек «съезжает» на единицу и
    // следующий вызов читает результат предыдущего как callee.
    ctx_->values[frame.calleeSlot] = result;
    ctx_->top = frame.calleeSlot + 1;
    if (ctx_->frameCount == 0) {
        if (ctx_->returnTo) {
            // Вспомогательный контекст нативного колбэка: отдаём результат
            // владельцу и завершаем run() (см. VM::callFunction).
            ctx_->push(result);
            ctx_ = ctx_->returnTo;
            return false;
        }
        if (ctx_->coroutine) {
            ObjCoroutine* co = ctx_->coroutine;
            co->state = ObjCoroutine::State::Dead;
            co->yieldValue = result;
            gc_.writeBarrier(co, result);
            ExecutionContext* parent = co->parent;
            if (parent && parent->frameCount > 0) {
                parent->top = co->savedTop;
                parent->push(result);   // значение, которое получит выражение `resume co`
                ctx_ = parent;
            } else {
                // Корутина запущена планировщиком: результат некому возвращать.
                ctx_ = co->returnTo ? co->returnTo : contexts_.front();
            }
            return false;
        }
        return false;
    }
    if (profiling_) ++profile_.returns;
    return true;
}

void VM::popTry() {
    if (tryHandlers_.empty()) return;
    tryHandlers_.pop_back();
    tryCtx_.pop_back();
    tryFrameDepth_.pop_back();
    tryStackDepth_.pop_back();
}

// ---------------------------------------------------------------------------
//  Публичные точки входа
// ---------------------------------------------------------------------------
RunStatus VM::execute(ObjFunction* fn, std::span<const Value> args) {
    auto* c = gc_.allocate<ObjClosure>(sizeof(ObjClosure), ObjHeader(ObjHeader::Type::Closure, sizeof(ObjClosure)));
    c->function = fn;
    c->upvalues = nullptr;
    return callValue(Value::object(c), args);
}

RunStatus VM::callValue(Value callable, std::span<const Value> args, Value* out) {
    resetStack();
    error_ = {};
    fuel_ = sandbox_.fuelPerCall;
    suspendRequested_ = false;
    ctx_ = contexts_.front();
    // Стек должен начинаться С НУЛЯ: callClosure вычисляет base нового кадра как
    // (top - argCount - 1), предполагая, что под callee нет посторонних значений.
    ctx_->top = 0;
    ctx_->frameCount = 0;

    if (callable.isObject() && callable.asObject()->type == ObjHeader::Type::Closure) {
        ctx_->push(callable);
        for (const Value& a : args) ctx_->push(a);
        if (!callClosure(objAs<ObjClosure>(callable), static_cast<int>(args.size())))
            return RunStatus::RuntimeError;
    } else if (callable.isObject() && callable.asObject()->type == ObjHeader::Type::NativeFn) {
        ctx_->push(callable);
        for (const Value& a : args) ctx_->push(a);
        if (!callNative(objAs<ObjNativeFn>(callable), static_cast<int>(args.size())))
            return RunStatus::RuntimeError;
        if (out) *out = ctx_->top > 0 ? ctx_->peek() : Value::nil();
        return RunStatus::Ok;
    } else {
        runtimeError("attempt to call a non-callable value");
        return RunStatus::RuntimeError;
    }

    const RunStatus st = run();
    if (out) *out = (ctx_->top > 0) ? ctx_->peek() : Value::nil();
    // Кадр верхнего уровня завершён: обнуляем счётчик, иначе планировщик
    // корутин впоследствии продолжил бы исполнение за пределами Halt.
    ctx_->top = 0;
    ctx_->frameCount = 0;
    return st;
}

void VM::logLine(std::string text) {
    if (logSink_) logSink_(std::move(text));
    else { std::fputs(text.c_str(), stdout); std::fputc('\n', stdout); std::fflush(stdout); }
}

RunStatus VM::callFunction(Value fn, const std::vector<Value>& args, Value* out) {
    if (!fn.isObject()) { runtimeError("attempt to call a non-callable value"); return RunStatus::RuntimeError; }
    ObjHeader* o = fn.asObject();
    if (o->type != ObjHeader::Type::Closure && o->type != ObjHeader::Type::NativeFn) {
        runtimeError("attempt to call a non-callable value");
        return RunStatus::RuntimeError;
    }

    // Колбэк исполняется в ОТДЕЛЬНОМ вспомогательном контексте. Если использовать
    // текущий, кадр колбэка лёг бы в область временных значений вызывающей
    // функции и перезаписал бы её аргументы.
    ExecutionContext* savedCtx = ctx_;

    std::unique_ptr<ExecutionContext> owned;
    ExecutionContext* h = nullptr;
    if (!ownedHelpers_.empty()) {
        owned = std::move(ownedHelpers_.back());
        ownedHelpers_.pop_back();
        h = owned.get();
        h->top = 0;
        h->frameCount = 0;
    } else {
        owned = std::make_unique<ExecutionContext>();
        h = owned.get();
        h->reserve();
    }
    h->returnTo = savedCtx;
    contexts_.push_back(h);
    ctx_ = h;

    h->push(fn);
    for (const Value& a : args) h->push(a);
    const bool ok = (o->type == ObjHeader::Type::Closure)
                        ? callClosure(static_cast<ObjClosure*>(o), static_cast<int>(args.size()))
                        : callNative(static_cast<ObjNativeFn*>(o), static_cast<int>(args.size()));
    RunStatus st = RunStatus::Ok;
    if (!ok) st = RunStatus::RuntimeError;
    else if (o->type == ObjHeader::Type::Closure) st = run();

    const Value result = (h->top > 0) ? h->values[h->top - 1] : Value::nil();

    h->top = 0;
    h->frameCount = 0;
    h->returnTo = nullptr;
    ctx_ = savedCtx;
    contexts_.erase(std::remove(contexts_.begin(), contexts_.end(), h), contexts_.end());
    ownedHelpers_.push_back(std::move(owned));

    if (out) *out = result;
    return st;
}

RunStatus VM::callMethod(Value receiver, std::string_view method, std::span<const Value> args, Value* out) {
    resetStack();
    error_ = {};
    fuel_ = sandbox_.fuelPerCall;
    suspendRequested_ = false;
    ctx_ = contexts_.front();
    ctx_->top = 0;
    ctx_->frameCount = 0;

    ObjString* name = internRaw(method);
    // Раскладка должна совпадать с тем, что генерирует компилятор для
    // `recv.m(args)`: `[receiver][a0..aN-1][callee]`. invokeMethod вычисляет
    // recvPos как top-argc-2 и перезаписывает ВЕРХНИЙ слот найденным
    // замыканием, поэтому без callee-слота recvPos указал бы на первый
    // аргумент (или вообще за пределы стека при argc == 0).
    ctx_->push(receiver);
    for (const Value& a : args) ctx_->push(a);
    ctx_->push(receiver);            // callee-слот (будет перезаписан)
    if (!invokeMethod(receiver, name, static_cast<int>(args.size()))) return RunStatus::RuntimeError;
    const RunStatus st = run();
    if (out) *out = (ctx_->top > 0) ? ctx_->peek() : Value::nil();
    ctx_->top = 0;
    ctx_->frameCount = 0;
    return st;
}

RunStatus VM::resumeCoroutine(ObjCoroutine* co, Value v, Value* out) {
    if (!co) { runtimeError("resume(nil)"); return RunStatus::RuntimeError; }
    if (co->state == ObjCoroutine::State::Dead) { if (out) *out = co->yieldValue; return RunStatus::Ok; }
    if (co->state == ObjCoroutine::State::Running) { runtimeError("cannot resume a running coroutine"); return RunStatus::RuntimeError; }
    error_ = {};
    suspendRequested_ = false;

    if (co->state == ObjCoroutine::State::Created) {
        auto owned = std::make_unique<ExecutionContext>();
        owned->reserve();
        for (auto& x : owned->values) x = Value::nil();
        owned->coroutine = co;
        ExecutionContext* raw = owned.get();
        ownedContexts_.push_back(std::move(owned));
        contexts_.push_back(raw);
        co->ownerContext = raw;

        co->state = ObjCoroutine::State::Running;
        co->parent = ctx_;
        co->savedTop = ctx_->top;
        ctx_ = raw;
        ctx_->top = 0;
        ctx_->frameCount = 0;
        ctx_->push(Value::object(co->root));
        ctx_->push(v);
        if (!callClosure(co->root, 1)) return RunStatus::RuntimeError;
    } else {
        co->state = ObjCoroutine::State::Running;
        co->parent = ctx_;
        co->savedTop = ctx_->top;
        ctx_ = co->ownerContext;
        if (!ctx_) { runtimeError("lost coroutine context"); return RunStatus::RuntimeError; }
        ctx_->push(v);   // значение, которое получит выражение `yield`
    }
    if (out) *out = co->yieldValue;
    return RunStatus::Ok;
}

bool VM::handleSuspend(CallFrame*& framePtr, ObjFunction*& fn, const Byte*& code) {
    suspendRequested_ = false;
    suspendSeconds_ = suspendSeconds_;
    ObjCoroutine* co = ctx_->coroutine;
    if (!co) {
        error_.message = "'wait'/suspend is only allowed inside a coroutine";
        if (ctx_ && ctx_->frameCount) error_.source = ctx_->frames[ctx_->frameCount - 1].closure->function->source;
        if (errorHandler_) errorHandler_(error_, traceback());
        return false;
    }
    co->state = ObjCoroutine::State::Suspended;
    co->waitUntil = gameTime_ + suspendSeconds_;
    co->waitEvent = suspendEvent_;
    suspendSeconds_ = 0.0;
    suspendEvent_ = nullptr;
    ExecutionContext* parent = co->parent;
    if (parent && parent->frameCount > 0) {
        parent->top = co->savedTop;
        parent->push(Value::nil());   // баланс стека: нативный вызов «вернул» nil
        ctx_ = parent;
    } else {
        ctx_ = co->returnTo ? co->returnTo : contexts_.front();
        if (ctx_->frameCount == 0) return true;
    }
    framePtr = &ctx_->frames[ctx_->frameCount - 1];
    fn = framePtr->closure->function;
    code = fn->code.data();
    return true;
}

bool VM::handleTryError(CallFrame*& framePtr, ObjFunction*& fn, const Byte*& code) {
    while (!tryHandlers_.empty()) {
        if (tryCtx_.back() != ctx_) { popTry(); continue; }
        const std::size_t targetIp = static_cast<std::size_t>(tryHandlers_.back().asInt());
        const std::size_t fr = tryFrameDepth_.back();
        const std::size_t st = tryStackDepth_.back();
        popTry();
        while (ctx_->frameCount > fr) { closeUpvalues(ctx_->frames[ctx_->frameCount - 1].base); --ctx_->frameCount; }
        ctx_->top = st;
        ctx_->push(internString(error_.message));
        framePtr = &ctx_->frames[ctx_->frameCount - 1];
        fn = framePtr->closure->function;
        code = fn->code.data();
        framePtr->ip = targetIp;
        return true;
    }
    return false;
}

RunStatus VM::tickRun() {
    if (!ctx_ || ctx_->frameCount == 0) return RunStatus::Ok;
    return run();
}

// ---------------------------------------------------------------------------
//  Главный цикл интерпретатора
// ---------------------------------------------------------------------------
RunStatus VM::run() {
    // Внешняя обёртка: любое исключение, всплывшее из интерпретатора,
    // превращается в ошибку скрипта. Главный источник — std::bad_alloc из
    // GC::reallocate при исчерпании лимита памяти песочницы; мод не должен
    // ронять редактор или игру. Сам цикл живёт в runUnsafe().
    try {
        return runUnsafe();
    } catch (const std::bad_alloc&) {
        runtimeError("script memory limit exceeded (sandbox)");
        return RunStatus::RuntimeError;
    } catch (const std::exception& ex) {
        runtimeError(std::string("internal error: ") + ex.what());
        return RunStatus::RuntimeError;
    }
}

RunStatus VM::runUnsafe() {
    if (!ctx_ || ctx_->frameCount == 0) return RunStatus::Ok;

#define BINARY_ARITH(OPSYM, EXPR)                                                     \
    case Op::OPSYM: {                                                                 \
        const Value b = POP(); const Value a = POP();                                 \
        if (!a.isNumber() || !b.isNumber())                                           \
            RUNTIME_ERR(format("arithmetic on non-numeric values ({} " #OPSYM    \
                                    " {})", a.typeName(), b.typeName()));             \
        PUSH(Value::fromNumber(EXPR));                                                \
        break;                                                                        \
    }

#define READ_U24()   (readOperand24(code + frame->ip))
#define READ_I24()   (readSigned24(code + frame->ip))
#define CONST(i)     (fn->constants[i])
#define STACK(i)     (ctx_->values[ctx_->top - 1 - (i)])
#define PUSH(v)      (ctx_->values[ctx_->top++] = (v))
#define POP()        (ctx_->values[--ctx_->top])
// ВАЖНО: операнд уже прочитан в `oper` в начале итерации. Повторное чтение
// READ_I24() сдвинуло бы ip ещё на 3 байта и взяло мусор — это была причина
// падения всех переходов. Поэтому прыжки используют готовый `oper`.
#define JUMP_FWD()   (frame->ip = static_cast<std::size_t>(static_cast<std::int64_t>(frame->ip) + jumpOffset))
#define JUMP_BACK()  JUMP_FWD()
// ОБЯЗАТЕЛЬНО: после любого вызова/возврата/переключения корутины нужно
// обновить И указатель на кадр, и ссылку `frame`. Раньше здесь обновлялся
// только framePtr, а `frame` оставалась ссылкой на предыдущий кадр — ip
// продолжал жить в старом кадре, и VM исполняла байткод не той функции.
#define REFRESH()    do { framePtr = &ctx_->frames[ctx_->frameCount - 1];              \
                          frame = framePtr;                                            \
                          fn = framePtr->closure->function;                            \
                          code = fn->code.data(); } while (0)
#define RUNTIME_ERR(msg) do { runtimeError(msg); goto onRuntimeError; } while (0)

    CallFrame* framePtr = &ctx_->frames[ctx_->frameCount - 1];
    ObjFunction* fn = framePtr->closure->function;
    const Byte* code = fn->code.data();
    CallFrame* frame = framePtr;
    bool suspendFlag = false;

    for (;;) {
        if (fuel_ <= 0) {
            error_.message = "out of fuel: instruction budget exhausted (sandbox)";
            error_.source = fn->source;
            if (errorHandler_) errorHandler_(error_, traceback());
            return RunStatus::OutOfFuel;
        }
        --fuel_;
        if (profiling_) ++profile_.instructions;

        const Op op = static_cast<Op>(code[frame->ip++]);
        std::uint32_t oper = READ_U24();
        frame->ip += 3;
        // Инструкции со знаковым 24-битным смещением.
        // ВАЖНО: `static_cast<std::int64_t>(std::uint32_t)` НЕ расширяет знак,
        // поэтому sign extension выполняется в 32-битном домене и только потом
        // преобразуется в int64_t. Иначе отрицательный прыжок назад превращается
        // в огромный положительный и интерпретатор улетает за конец байткода.
        std::int64_t jumpOffset = 0;
        switch (op) {
            case Op::Jump: case Op::Loop: case Op::JumpIfFalse: case Op::JumpIfTrue:
            case Op::JumpIfFalsePop: case Op::JumpIfTruePop: case Op::JumpIfNil:
            case Op::IterNext: case Op::IterKeyNext: case Op::ForRangeLoop:
            case Op::TryBegin: case Op::Elvis: {
                std::uint32_t u = oper;
                if (u & 0x800000u) u |= 0xFF000000u;
                jumpOffset = static_cast<std::int32_t>(u);
                break;
            }
            default: break;
        }

        switch (op) {
        // ---------------------------------------------------------------- Стек
        case Op::Nop: break;
        case Op::Dup:  { const Value v = STACK(0); PUSH(v); break; }
        case Op::Dup1: { const Value v = STACK(1); PUSH(v); break; }
        case Op::Swap: { std::swap(STACK(0), STACK(1)); break; }

        case Op::Pop:  { (void)POP(); break; }
        case Op::PopN: { ctx_->top -= oper; break; }
        case Op::PeekSlot: {
            PUSH(ctx_->values[frame->base + oper]); break; }
        case Op::PokeSlot: { ctx_->values[frame->base + oper] = POP(); break; }

        // ----------------------------------------------------------- Константы
        case Op::Constant: { const Value v = CONST(oper); PUSH(v); break; }
        case Op::Nil:      { PUSH(Value::nil()); break; }
        case Op::True:     { PUSH(Value::boolean(true)); break; }
        case Op::False:    { PUSH(Value::boolean(false)); break; }
        case Op::IntConst: {
            // Операнд — знаковое 24-битное целое; расширяем знак явно.
            std::uint32_t u = oper;
            if (u & 0x800000u) u |= 0xFF000000u;
            PUSH(Value::fromNumber(static_cast<double>(static_cast<std::int32_t>(u))));
            break;
        }
        case Op::EmptyArray: { PUSH(makeArray(0)); break; }
        case Op::EmptyMap:   { PUSH(makeMap()); break; }

        // ---------------------------------------------------------- Переменные
        case Op::GetLocal:   {
            PUSH(ctx_->values[frame->base + oper]); break; }
        case Op::SetLocal: {
            // PEEK-запись: значение КОПИРУЕТСЯ в слот кадра и ОСТАЁТСЯ на стеке.
            // Контракт намеренно отличается от SetGlobal/SetUpvalue/DefineGlobal
            // (те значение СНИМАЮТ): SetLocal используется там, где одно и то же
            // значение нужно и сохранить, и оставить доступным — паттерны match
            // (связывание читает дубликат субъекта), аккумулятор while-выражения,
            // присваивание как выражение. Компилятор обязан уравновешивать его
            // явным Op::Pop там, где значение не нужно (объявления `let`/`var`,
            // переменная цикла, итератор). Раньше эти Pop отсутствовали, и каждый
            // `let` внутри функции протекал слотом стека: в длинных функциях
            // (stdlib/pathfinding — 25 локалов) временные значения начинали
            // писать поверх локалов, что выглядело как «cannot index a Int».
            ctx_->values[frame->base + oper] = STACK(0);
            break;
        }
        case Op::GetUpvalue: { PUSH(*frame->closure->upvalues[oper]->location); break; }
        case Op::SetUpvalue: {
            // Снимает значение (единый контракт со SetGlobal); см. Op::SetLocal.
            ObjUpvalue* uv = frame->closure->upvalues[oper];
            const Value v = POP();
            *uv->location = v;
            // Закрытый upvalue — обычный кучевой объект и может быть уже чёрным.
            gc_.writeBarrier(uv, v);
            break;
        }
        case Op::GetGlobal: {
            ObjString* nm = names_[oper];
            Value* v = globals_->findString(nm->view(), nm->hash);
            if (!v) RUNTIME_ERR(format("undefined name '{}'", std::string(nm->view())));
            PUSH(*v);
            break;
        }
        case Op::DefineGlobal: {
            ObjString* nm = names_[oper];
            globals_->set(gc_, Value::object(nm), POP());
            break;
        }
        case Op::SetGlobal: {
            ObjString* nm = names_[oper];
            // Присваивание необъявленному имени создаёт глобал: язык допускает
            // `counter = 0` на уровне модуля после объявления в другом файле,
            // а опечатки ловит тайп-чекер, а не рантайм.
            // Снимает значение со стека (как DefineGlobal): если присваивание
            // используется как выражение, компилятор заново кладёт его GetGlobal'ом.
            globals_->set(gc_, Value::object(nm), POP());
            break;
        }
        case Op::GetModule: {
            ObjString* nm = names_[oper];
            Value* v = modules_->findString(nm->view(), nm->hash);
            if (!v) RUNTIME_ERR(format("module '{}' is not loaded", std::string(nm->view())));
            PUSH(*v);
            break;
        }
        case Op::LoadName: {
            ObjString* nm = names_[oper];
            Value* v = globals_->findString(nm->view(), nm->hash);
            PUSH(v ? *v : Value::nil());
            break;
        }
        case Op::GetThis:  { PUSH(frame->thisValue); break; }
        case Op::GetSuper: { PUSH(frame->superValue); break; }
        case Op::GetSuperOf: {
            // `super` разрешается во время выполнения: берём класс значения
            // (инстанс или класс) и возвращаем его суперкласс.
            const Value v = POP();
            ObjClass* cls = nullptr;
            if (v.isObject()) {
                if (v.asObject()->type == ObjHeader::Type::Instance) cls = objAs<ObjInstance>(v)->klass;
                else if (v.asObject()->type == ObjHeader::Type::Class) cls = objAs<ObjClass>(v);
            }
            if (!cls) RUNTIME_ERR("'super' used outside of a class method");
            if (!cls->superclass) RUNTIME_ERR(format("class '{}' has no superclass", std::string(cls->name->view())));
            PUSH(Value::object(cls->superclass));
            break;
        }

        // ----------------------------------------------------------- Арифметика
        case Op::Add: {
            const Value b = POP(); const Value a = POP();
            if (a.isNumber() && b.isNumber()) { PUSH(Value::fromNumber(a.asNumber() + b.asNumber())); break; }
            const bool aStr = a.isObject() && a.asObject()->type == ObjHeader::Type::String;
            const bool bStr = b.isObject() && b.asObject()->type == ObjHeader::Type::String;
            if (aStr && bStr) {
                std::string str(objAs<ObjString>(a)->view());
                str.append(objAs<ObjString>(b)->view());
                PUSH(internString(std::move(str)));
                break;
            }
            if (a.isObject() && a.asObject()->type == ObjHeader::Type::Array &&
                b.isObject() && b.asObject()->type == ObjHeader::Type::Array) {
                auto* x = static_cast<ObjArray*>(a.asObject()); auto* y = static_cast<ObjArray*>(b.asObject());
                ObjArray* out = lv::makeArray(gc_, x->count + y->count);
                for (std::uint32_t i = 0; i < x->count; ++i) out->items[out->count++] = x->items[i];
                for (std::uint32_t i = 0; i < y->count; ++i) out->items[out->count++] = y->items[i];
                PUSH(Value::object(out));
                break;
            }
            RUNTIME_ERR(format("cannot add {} and {}", a.typeName(), b.typeName()));
            break;
        }
        BINARY_ARITH(Sub, a.asNumber() - b.asNumber())
        BINARY_ARITH(Mul, a.asNumber() * b.asNumber())
        case Op::Div: {
            const Value b = POP(); const Value a = POP();
            if (!a.isNumber() || !b.isNumber()) RUNTIME_ERR("division on non-numeric values");
            if (b.asNumber() == 0.0) RUNTIME_ERR("division by zero");
            PUSH(Value::fromNumber(a.asNumber() / b.asNumber()));
            break;
        }
        case Op::Mod: {
            const Value b = POP(); const Value a = POP();
            if (!a.isNumber() || !b.isNumber()) RUNTIME_ERR("modulo on non-numeric values");
            if (b.asNumber() == 0.0) RUNTIME_ERR("modulo by zero");
            PUSH(Value::fromNumber(std::fmod(a.asNumber(), b.asNumber())));
            break;
        }
        BINARY_ARITH(Pow, std::pow(a.asNumber(), b.asNumber()))
        case Op::Neg: {
            const Value a = POP();
            if (!a.isNumber()) RUNTIME_ERR("unary '-' on a non-numeric value");
            PUSH(Value::fromNumber(-a.asNumber()));
            break;
        }
        case Op::Not:    { const Value a = POP(); PUSH(Value::boolean(!a.truthy())); break; }
        case Op::BitNot: { const Value a = POP();
                           if (!a.isNumber()) RUNTIME_ERR("'~' on a non-numeric value");
                           PUSH(Value::integer(~a.asInt())); break; }
        case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: {
            const Value b = POP(); const Value a = POP();
            if (!a.isNumber() || !b.isNumber()) RUNTIME_ERR("bitwise operation on non-numeric values");
            const std::int64_t x = a.asInt(), y = b.asInt();
            std::int64_t r = 0;
            switch (op) {
                case Op::BitAnd: r = x & y; break;
                case Op::BitOr:  r = x | y; break;
                case Op::BitXor: r = x ^ y; break;
                case Op::Shl:    r = x << (y & 63); break;
                case Op::Shr:    r = x >> (y & 63); break;
                default: break;
            }
            PUSH(Value::integer(r));
            break;
        }
        case Op::Equal:    { const Value b = POP(); const Value a = POP(); PUSH(Value::boolean(valuesEqual(a, b))); break; }
        case Op::NotEqual: { const Value b = POP(); const Value a = POP(); PUSH(Value::boolean(!valuesEqual(a, b))); break; }
        case Op::Greater: case Op::Less: case Op::GreaterEq: case Op::LessEq: {
            const Value b = POP(); const Value a = POP();
            bool r = false;
            if (a.isNumber() && b.isNumber()) {
                const double x = a.asNumber(), y = b.asNumber();
                r = op == Op::Greater ? x > y : op == Op::Less ? x < y : op == Op::GreaterEq ? x >= y : x <= y;
            } else if (a.isObject() && b.isObject() &&
                       a.asObject()->type == ObjHeader::Type::String &&
                       b.asObject()->type == ObjHeader::Type::String) {
                const int c = objAs<ObjString>(a)->view().compare(objAs<ObjString>(b)->view());
                r = op == Op::Greater ? c > 0 : op == Op::Less ? c < 0 : op == Op::GreaterEq ? c >= 0 : c <= 0;
            } else RUNTIME_ERR(format("cannot compare {} with {}", a.typeName(), b.typeName()));
            PUSH(Value::boolean(r));
            break;
        }

        // -------------------------------------------------- Индексация и поля
        case Op::IndexGetPeek: {
            // `[obj][key]` -> `[obj][key][value]`.
            //
            // Составное присваивание `obj[key] op= rhs` должно вычислить obj и
            // key РОВНО ОДИН РАЗ (иначе `f()[g()] += 1` вызвал бы побочные
            // эффекты дважды), а затем и прочитать старое значение, и записать
            // новое — то есть операнды нужны ДВАЖДЫ. Вместо жонглирования
            // стеком ротациями введён этот peek-вариант IndexGet.
            if (ctx_->top < 2) RUNTIME_ERR("stack underflow in IndexGetPeek");
            const Value key = STACK(0);
            const Value obj = STACK(1);
            if (!obj.isObject()) RUNTIME_ERR(format("cannot index a {}", obj.typeName()));
            ObjHeader* o = obj.asObject();
            switch (o->type) {
                case ObjHeader::Type::Array: {
                    auto* arr = static_cast<ObjArray*>(o);
                    std::int64_t i = key.asInt();
                    if (i < 0) i += arr->count;
                    if (i < 0 || i >= static_cast<std::int64_t>(arr->count))
                        RUNTIME_ERR(format("array index {} out of range (length {})", key.asInt(), arr->count));
                    PUSH(arr->items[i]);
                    break;
                }
                case ObjHeader::Type::Map: {
                    Value* v = static_cast<ObjMap*>(o)->find(key);
                    PUSH(v ? *v : Value::nil());
                    break;
                }
                case ObjHeader::Type::String: {
                    auto* s2 = static_cast<ObjString*>(o);
                    std::int64_t i = key.asInt();
                    if (i < 0) i += s2->length;
                    if (i < 0 || i >= static_cast<std::int64_t>(s2->length))
                        RUNTIME_ERR("string index out of range");
                    PUSH(internString(std::string_view(s2->data + i, 1)));
                    break;
                }
                case ObjHeader::Type::Instance: {
                    Value* v = static_cast<ObjInstance*>(o)->fields->find(key);
                    PUSH(v ? *v : Value::nil());
                    break;
                }
                default: RUNTIME_ERR(format("cannot index a {}", o->typeName()));
            }
            break;
        }
        case Op::IndexGet: {
            const Value key = POP(); const Value obj = POP();
            if (!obj.isObject()) RUNTIME_ERR(format("cannot index a {}", obj.typeName()));
            ObjHeader* o = obj.asObject();
            switch (o->type) {
                case ObjHeader::Type::Array: {
                    auto* arr = static_cast<ObjArray*>(o);
                    std::int64_t i = key.asInt();
                    if (i < 0) i += arr->count;
                    if (i < 0 || i >= static_cast<std::int64_t>(arr->count))
                        RUNTIME_ERR(format("array index {} out of range (length {})", key.asInt(), arr->count));
                    PUSH(arr->items[i]);
                    break;
                }
                case ObjHeader::Type::Map: {
                    Value* v = static_cast<ObjMap*>(o)->find(key);
                    PUSH(v ? *v : Value::nil());
                    break;
                }
                case ObjHeader::Type::String: {
                    auto* s = static_cast<ObjString*>(o);
                    std::int64_t i = key.asInt();
                    if (i < 0) i += s->length;
                    if (i < 0 || i >= static_cast<std::int64_t>(s->length))
                        RUNTIME_ERR("string index out of range");
                    PUSH(internString(std::string_view(s->data + i, 1)));
                    break;
                }
                case ObjHeader::Type::Instance: {
                    Value* v = static_cast<ObjInstance*>(o)->fields->find(key);
                    PUSH(v ? *v : Value::nil());
                    break;
                }
                default: RUNTIME_ERR(format("cannot index a {}", o->typeName()));
            }
            break;
        }
        case Op::IndexSetTop:
            // То же, что IndexSet, но порядок снятия соответствует естественному
            // порядку ВЫЧИСЛЕНИЯ (`obj`, затем `key`, затем значение наверху).
            // Используется составным присваиванием `obj[key] op= rhs`, где obj и
            // key уже лежат в стеке и не могут быть переставлены без потери
            // однократности их вычисления.
            [[fallthrough]];
        case Op::IndexSet: {
            // Результат: `[val]` на вершине (присваивание — выражение).
            //
            // Два порядка входа существуют потому, что «естественный» порядок
            // вычисления (`obj`, `key`, затем значение) не совпадает с порядком
            // `[val][obj][key]`, который исторически использует обычное
            // присваивание. Составное присваивание `obj[key] op= rhs` не может
            // переложить уже вычисленные obj/key, поэтому для него есть
            // Op::IndexSetTop. Оба варианта ОБЯЗАНЫ оставлять на вершине
            // записанное значение — иначе разъезжается учёт стека в компиляторе.
            // Op::IndexSet:    стек `[val][obj][key]` (key на вершине).
            // Op::IndexSetTop: стек `[obj][key][val]` (val на вершине) —
            //   естественный порядок вычисления, нужен составному присваиванию,
            //   где obj и key уже лежат ниже и не могут быть переставлены.
            Value val, key, obj;
            if (op == Op::IndexSetTop) { val = POP(); key = POP(); obj = POP(); }
            else                       { key = POP(); obj = POP(); val = POP(); }
            if (!obj.isObject()) RUNTIME_ERR("cannot index-assign a non-object");
            ObjHeader* o = obj.asObject();
            switch (o->type) {
                case ObjHeader::Type::Array: {
                    auto* arr = static_cast<ObjArray*>(o);
                    std::int64_t i = key.asInt();
                    if (i < 0) i += arr->count;
                    if (i < 0 || i >= static_cast<std::int64_t>(arr->count))
                        RUNTIME_ERR("array index out of range in assignment");
                    arr->items[i] = val;
                    gc_.writeBarrier(arr, val);   // трёхцветная инварианта
                    break;
                }
                case ObjHeader::Type::Map:     static_cast<ObjMap*>(o)->set(gc_, key, val); break;
                case ObjHeader::Type::Instance:static_cast<ObjInstance*>(o)->fields->set(gc_, key, val); break;
                default: RUNTIME_ERR(format("cannot index-assign a {}", o->typeName()));
            }
            PUSH(val);
            break;
        }
        case Op::MemberGet: case Op::MemberGetOpt: {
            ObjString* nm = names_[oper];
            const Value obj = POP();
            if (obj.isNil()) {
                if (op == Op::MemberGetOpt) { PUSH(Value::nil()); break; }
                RUNTIME_ERR(format("attempt to read field '{}' of nil", std::string(nm->view())));
            }
            if (!obj.isObject()) RUNTIME_ERR(format("attempt to read field '{}' of {}",
                                                         std::string(nm->view()), obj.typeName()));
            ObjHeader* o = obj.asObject();
            if (o->type == ObjHeader::Type::Instance) {
                auto* inst = static_cast<ObjInstance*>(o);
                if (Value* v = inst->fields->find(Value::object(nm))) { PUSH(*v); break; }
                if (ObjClosure* m = findMethod(inst->klass, nm))      { PUSH(Value::object(m)); break; }
                RUNTIME_ERR(format("'{}' has no field or method '{}'",
                                        std::string(inst->klass->name->view()), std::string(nm->view())));
            } else if (o->type == ObjHeader::Type::Class) {
                auto* cls = static_cast<ObjClass*>(o);
                if (Value* v = cls->methods->find(Value::object(nm))) { PUSH(*v); break; }
                RUNTIME_ERR(format("class '{}' has no static member '{}'",
                                        std::string(cls->name->view()), std::string(nm->view())));
            } else if (o->type == ObjHeader::Type::Map) {
                Value* v = static_cast<ObjMap*>(o)->find(Value::object(nm));
                PUSH(v ? *v : Value::nil());
            } else {
                // Встроенные типы: метод берётся из таблицы как значение (для `.method` без вызова).
                const char* tn = o->type == ObjHeader::Type::String ? "String"
                               : o->type == ObjHeader::Type::Array  ? "Array"
                               : o->type == ObjHeader::Type::Map    ? "Map" : nullptr;
                if (tn) {
                    if (Value* tbl = methodTables_->find(internString(tn))) {
                        if (Value* m = objAs<ObjMap>(*tbl)->find(Value::object(nm))) { PUSH(*m); break; }
                    }
                }
                RUNTIME_ERR(format("'{}' has no member '{}'", o->typeName(), std::string(nm->view())));
            }
            break;
        }
        case Op::MemberSet: {
            // Контракт: стек `[obj][val]` -> `[val]`.
            // ПОРЯДОК ВАЖЕН и должен совпадать с Op::IndexSet (`[obj][key][val]`):
            // значение лежит НА ВЕРШИНЕ. Раньше здесь сначала снимался obj, и
            // обычное присваивание `a.b = v` (компилятор кладёт [v][obj])
            // «работало», а составное `a.b += v` (кладёт [obj][new]) падало с
            // «cannot set field on Float». Теперь оба пути дают [obj][val].
            ObjString* nm = names_[oper];
            const Value val = POP();
            const Value obj = POP();
            if (!obj.isObject()) RUNTIME_ERR(format("cannot set field '{}' on {}",
                                                         std::string(nm->view()), obj.typeName()));
            ObjHeader* o = obj.asObject();
            if (o->type == ObjHeader::Type::Instance)   static_cast<ObjInstance*>(o)->fields->set(gc_, Value::object(nm), val);
            else if (o->type == ObjHeader::Type::Map)   static_cast<ObjMap*>(o)->set(gc_, Value::object(nm), val);
            else if (o->type == ObjHeader::Type::Class) static_cast<ObjClass*>(o)->methods->set(gc_, Value::object(nm), val);
            else RUNTIME_ERR(format("cannot set field '{}' on {}", std::string(nm->view()), o->typeName()));
            PUSH(val);
            break;
        }

        // --------------------------------------------------------- Коллекции
        case Op::ArrayPush: {
            const Value val = POP(); const Value arr = POP();
            if (!arr.isObject() || arr.asObject()->type != ObjHeader::Type::Array) RUNTIME_ERR("push on a non-array");
            auto* a = static_cast<ObjArray*>(arr.asObject());
            a->grow(gc_, a->count + 1);
            a->items[a->count++] = val;
            gc_.writeBarrier(a, val);   // массив мог уже почернеть в фазе Mark
            PUSH(arr);
            break;
        }
        case Op::NewArray: {
            ObjArray* arr = lv::makeArray(gc_, oper);
            for (std::uint32_t i = 0; i < oper; ++i) arr->items[oper - 1 - i] = POP();
            arr->count = oper;
            PUSH(Value::object(arr));
            break;
        }
        case Op::NewMap: {
            ObjMap* m = lv::makeMap(gc_);
            scratchPairs_.resize(oper);
            for (std::uint32_t i = 0; i < oper; ++i) {
                Value v = POP(); Value k = POP();
                scratchPairs_[oper - 1 - i] = {k, v};
            }
            for (auto& [k, v] : scratchPairs_) m->set(gc_, k, v);
            PUSH(Value::object(m));
            break;
        }
        case Op::SpreadAppend: {
            const Value src = POP();
            const Value arr = POP();
            if (!arr.isObject() || arr.asObject()->type != ObjHeader::Type::Array) RUNTIME_ERR("spread into a non-array");
            if (!src.isObject() || src.asObject()->type != ObjHeader::Type::Array) RUNTIME_ERR("spread of a non-array");
            auto* dst = static_cast<ObjArray*>(arr.asObject());
            auto* s = static_cast<ObjArray*>(src.asObject());
            dst->grow(gc_, dst->count + s->count);
            for (std::uint32_t i = 0; i < s->count; ++i) {
                dst->items[dst->count++] = s->items[i];
                gc_.writeBarrier(dst, s->items[i]);
            }
            PUSH(arr);
            break;
        }
        case Op::Len: {
            const Value v = POP();
            if (!v.isObject()) RUNTIME_ERR("'len' of a non-object");
            switch (v.asObject()->type) {
                case ObjHeader::Type::Array:  PUSH(Value::integer(objAs<ObjArray>(v)->count)); break;
                case ObjHeader::Type::String: PUSH(Value::integer(objAs<ObjString>(v)->length)); break;
                case ObjHeader::Type::Map:    PUSH(Value::integer(objAs<ObjMap>(v)->count)); break;
                default: RUNTIME_ERR(format("'len' is not defined for {}", v.asObject()->typeName()));
            }
            break;
        }
        case Op::Concat: {
            scratchStr_.clear();
            for (std::uint32_t i = 0; i < oper; ++i) {
                const Value v = POP();
                if (v.isObject() && v.asObject()->type == ObjHeader::Type::String)
                    scratchStr_.insert(0, static_cast<ObjString*>(v.asObject())->view());
                else
                    scratchStr_.insert(0, v.toString());
            }
            PUSH(internString(scratchStr_));
            break;
        }
        case Op::Slice: {
            const Value to = POP(); const Value from = POP(); const Value obj = POP();
            if (!obj.isObject()) RUNTIME_ERR("slice of a non-object");
            if (obj.asObject()->type == ObjHeader::Type::String) {
                auto* s = static_cast<ObjString*>(obj.asObject());
                std::int64_t a = from.asInt(), b = to.asInt();
                if (a < 0) a += s->length;
                if (b < 0) b += s->length;
                a = std::clamp<std::int64_t>(a, 0, s->length);
                b = std::clamp<std::int64_t>(b, a, s->length);
                PUSH(internString(std::string_view(s->data + a, static_cast<std::size_t>(b - a))));
            } else if (obj.asObject()->type == ObjHeader::Type::Array) {
                auto* arr = static_cast<ObjArray*>(obj.asObject());
                std::int64_t a = from.asInt(), b = to.asInt();
                if (a < 0) a += arr->count;
                if (b < 0) b += arr->count;
                a = std::clamp<std::int64_t>(a, 0, arr->count);
                b = std::clamp<std::int64_t>(b, a, arr->count);
                ObjArray* out = lv::makeArray(gc_, static_cast<std::uint32_t>(b - a));
                for (std::int64_t i = a; i < b; ++i) out->items[out->count++] = arr->items[i];
                PUSH(Value::object(out));
            } else RUNTIME_ERR("slice is only defined for String and Array");
            break;
        }

        // --------------------------------------------------------- Итераторы
        case Op::NewRange: {
            const Value to = POP(); const Value from = POP();
            auto* it = gc_.allocate<ObjIterator>(sizeof(ObjIterator), ObjHeader(ObjHeader::Type::Iterator, sizeof(ObjIterator)));
            it->kind = ObjIterator::Kind::Range;
            it->source = Value::nil();
            const std::int64_t a = from.asInt();
            const std::int64_t b = to.asInt() + (oper ? 1 : 0);
            it->index = a;
            it->end = b;
            // Нисходящий диапазон (`for i in 5..0`) идёт вниз автоматически.
            it->step = (a <= b) ? 1 : -1;
            it->done = false;
            PUSH(Value::object(it));
            break;
        }
        case Op::IterInit: {
            const Value v = STACK(0);
            if (v.isObject() && v.asObject()->type == ObjHeader::Type::Iterator) break; // уже итератор
            auto* it = gc_.allocate<ObjIterator>(sizeof(ObjIterator), ObjHeader(ObjHeader::Type::Iterator, sizeof(ObjIterator)));
            it->source = v;
            it->index = 0;
            it->step = 1;
            it->done = false;
            it->mapCursor = 0;
            if (v.isObject()) {
                switch (v.asObject()->type) {
                    case ObjHeader::Type::Array:  it->kind = ObjIterator::Kind::Array;  it->end = static_cast<ObjArray*>(v.asObject())->count; break;
                    case ObjHeader::Type::String: it->kind = ObjIterator::Kind::String; it->end = static_cast<ObjString*>(v.asObject())->length; break;
                    case ObjHeader::Type::Map:    it->kind = ObjIterator::Kind::Map;    it->end = static_cast<ObjMap*>(v.asObject())->capacity; break;
                    default: RUNTIME_ERR(format("value of type {} is not iterable", v.asObject()->typeName()));
                }
            } else RUNTIME_ERR(format("value of type {} is not iterable", v.typeName()));
            STACK(0) = Value::object(it);
            break;
        }
        case Op::IterNext: case Op::IterKeyNext: {
            const Value iter = ctx_->values[frame->base + frame->iterSlot];
            if (!iter.isObject() || iter.asObject()->type != ObjHeader::Type::Iterator)
                RUNTIME_ERR("broken iterator");
            auto* it = static_cast<ObjIterator*>(iter.asObject());
            bool has = false;
            Value key = Value::nil(), val = Value::nil();
            switch (it->kind) {
                case ObjIterator::Kind::Range:
                    has = it->step > 0 ? it->index < it->end : it->index > it->end;
                    key = val = Value::integer(it->index);
                    it->index += it->step;
                    break;
                case ObjIterator::Kind::Array: {
                    auto* a = static_cast<ObjArray*>(it->source.asObject());
                    has = it->index < static_cast<std::int64_t>(a->count);
                    if (has) { key = Value::integer(it->index); val = a->items[it->index++]; }
                    break;
                }
                case ObjIterator::Kind::String: {
                    auto* s = static_cast<ObjString*>(it->source.asObject());
                    has = it->index < static_cast<std::int64_t>(s->length);
                    if (has) { key = Value::integer(it->index); val = internString(std::string_view(s->data + it->index++, 1)); }
                    break;
                }
                case ObjIterator::Kind::Map: {
                    auto* m = static_cast<ObjMap*>(it->source.asObject());
                    while (it->mapCursor < m->capacity && !m->entries[it->mapCursor].used) ++it->mapCursor;
                    has = it->mapCursor < m->capacity;
                    if (has) { key = m->entries[it->mapCursor].key; val = m->entries[it->mapCursor].value; ++it->mapCursor; }
                    break;
                }
                default: break;
            }
            if (!has) { JUMP_FWD(); break; }
            // Порядок важен: SetLocal для ПЕРВОЙ переменной (`k`) выполняется сразу
            // после инструкции и снимает вершину. Значит на вершине должен быть KEY:
            // сначала пушим value, затем key.
            // IterKeyNext кладёт значение на вершину, ключ — под ним (см. Op::StorePair).
            if (op == Op::IterKeyNext) { PUSH(key); PUSH(val); }
            else PUSH(val);
            break;
        }
        case Op::SetLoopSlots: {
            frame->iterSlot = (oper >> 16) & 0xFFFF;
            frame->varSlot  = (oper >> 8) & 0xFFFF;
            frame->varSlot2 = 0;
            break;
        }
        case Op::StorePair: {
            // Стек: [...][key][value] (value на вершине). SetLocal не снимает
            // значение, поэтому пара записывается одной инструкцией.
            const std::uint32_t vSlot = oper & 0xFFF;
            const std::uint32_t kSlot = (oper >> 12) & 0xFFF;
            ctx_->values[frame->base + vSlot] = STACK(0);
            ctx_->values[frame->base + kSlot] = STACK(1);
            ctx_->top -= 2;
            break;
        }
        case Op::ForRangePrep: {
            const Value incl = POP();
            const Value endV = POP();
            const Value startV = POP();
            const std::int64_t start = startV.asInt();
            std::int64_t end = endV.asInt();
            // `a..=b` -> полуинтервал [a, b+1); флаг приходит отдельным операндом.
            if (incl.asInt() != 0) ++end;
            const std::int64_t step = (start <= end) ? 1 : -1;
            // Op::ForRangeLoop работает по схеме «сначала инкремент, потом
            // проверка», поэтому счётчик инициализируется значением start-step:
            // первая итерация увидит ровно start.
            ctx_->values[frame->base + oper]     = Value::integer(start - step);
            ctx_->values[frame->base + oper + 1] = Value::integer(end);
            ctx_->values[frame->base + oper + 2] = Value::integer(step);
            break;
        }
        case Op::ForRangeLoop: {
            // Слот переменной цикла берётся из кадра (операнд занят смещением выхода).
            const std::uint32_t slot = frame->varSlot;
            Value& cur = ctx_->values[frame->base + slot];
            Value& end = ctx_->values[frame->base + slot + 1];
            Value& stp = ctx_->values[frame->base + slot + 2];
            const std::int64_t c = cur.asInt() + stp.asInt();
            cur = Value::integer(c);
            const bool ok = stp.asInt() > 0 ? c < end.asInt() : c > end.asInt();
            if (!ok) JUMP_FWD();
            break;
        }

        // ------------------------------------------------------------ Переходы
        case Op::Jump:            { JUMP_FWD(); break; }
        case Op::Loop:            { JUMP_BACK(); break; }
        case Op::JumpIfFalse:     { if (!POP().truthy())    JUMP_FWD(); break; }
        case Op::JumpIfTrue:      { if (POP().truthy())     JUMP_FWD(); break; }
        case Op::JumpIfFalsePop:  { if (!STACK(0).truthy()) JUMP_FWD(); else (void)POP(); break; }
        case Op::JumpIfTruePop:   { if (STACK(0).truthy())  JUMP_FWD(); else (void)POP(); break; }
        case Op::JumpIfNil:       { if (STACK(0).isNil())   JUMP_FWD(); break; }
        case Op::Elvis: {
            // `a ?: b`: a уже на вершине. Если истинно — перейти к коду после b,
            // иначе снять a и исполнить b (который оставит своё значение).
            if (STACK(0).truthy()) JUMP_FWD();
            else (void)POP();
            break;
        }

        // -------------------------------------------------------------- Вызовы
        case Op::Call: {
            const int argc = static_cast<int>(oper);
            const Value callee = ctx_->values[ctx_->top - 1 - static_cast<std::size_t>(argc)];
            if (!callee.isObject()) RUNTIME_ERR(format("attempt to call a {} value", callee.typeName()));
            ObjHeader* o = callee.asObject();
            if (o->type == ObjHeader::Type::Closure) {
                if (!callClosure(objAs<ObjClosure>(o), argc)) goto onRuntimeError;
                REFRESH();
            } else if (o->type == ObjHeader::Type::NativeFn) {
                if (!callNative(objAs<ObjNativeFn>(o), argc)) goto onRuntimeError;
                if (suspendRequested_) { suspendFlag = true; break; }
            } else if (o->type == ObjHeader::Type::Coroutine) {
                // `coroutine grow() do ... end` объявляет ПЕРВИЧНУЮ корутину:
                // вызов grow() немедленно стартует её и возвращает первое
                // значение yield (или итоговый результат).
                //
                // ВАЖНО: никакого вложенного run()! resumeCoroutine лишь
                // переключает ctx_ и готовит кадр, а текущий цикл интерпретатора
                // продолжает работу уже с байткодом корутины. Вложенный run()
                // исполнял бы Halt скрипта и оставлял внешний цикл за концом кода.
                auto* co = objAs<ObjCoroutine>(callee);
                if (co->state == ObjCoroutine::State::Dead) {
                    ctx_->top -= static_cast<std::size_t>(argc) + 1;
                    PUSH(co->yieldValue);
                    break;
                }
                const Value firstArg = (argc > 0) ? ctx_->values[ctx_->top - static_cast<std::size_t>(argc)] : Value::nil();
                ctx_->top -= static_cast<std::size_t>(argc) + 1;
                co->returnTo = ctx_;
                if (resumeCoroutine(co, firstArg) != RunStatus::Ok) goto onRuntimeError;
                REFRESH();
            } else if (o->type == ObjHeader::Type::Class) {
                // `MyClass(a, b)` — то же самое, что `new MyClass(a, b)`.
                auto* cls = static_cast<ObjClass*>(o);
                ObjInstance* inst = lv::makeInstance(gc_, cls);
                applyFieldDefaults(inst, cls);
                const std::size_t clsPos = ctx_->top - 1 - static_cast<std::size_t>(argc);
                ObjString* initName = internRaw("init");
                if (ObjClosure* init = findMethod(cls, initName)) {
                    const std::size_t callerDepth = ctx_->frameCount;
                    ctx_->values[clsPos] = Value::object(inst);
                    if (!callClosure(init, argc)) goto onRuntimeError;
                    CallFrame& nf = ctx_->frames[ctx_->frameCount - 1];
                    nf.thisValue = Value::object(inst);
                    nf.superValue = cls->superclass ? Value::object(cls->superclass) : Value::nil();
                    (void)callerDepth;   // результат init подменяется в doReturn
                    REFRESH();
                } else {
                    ctx_->top = clsPos;
                    PUSH(Value::object(inst));
                }
            } else RUNTIME_ERR(format("value of type {} is not callable", o->typeName()));
            break;
        }
        case Op::CallSpread: {
            const int argc = static_cast<int>(oper);
            const Value last = POP();
            if (!last.isObject() || last.asObject()->type != ObjHeader::Type::Array)
                RUNTIME_ERR("spread argument must be an Array");
            auto* arr = static_cast<ObjArray*>(last.asObject());
            for (std::uint32_t i = 0; i < arr->count; ++i) PUSH(arr->items[i]);
            const int total = argc - 1 + static_cast<int>(arr->count);
            const Value callee = ctx_->values[ctx_->top - 1 - static_cast<std::size_t>(total)];
            if (!callee.isObject()) RUNTIME_ERR("spread call of a non-callable value");
            if (callee.asObject()->type == ObjHeader::Type::Closure) {
                if (!callClosure(objAs<ObjClosure>(callee), total)) goto onRuntimeError;
                REFRESH();
            } else if (callee.asObject()->type == ObjHeader::Type::NativeFn) {
                if (!callNative(objAs<ObjNativeFn>(callee), total)) goto onRuntimeError;
                if (suspendRequested_) { suspendFlag = true; break; }
            } else RUNTIME_ERR("spread call of a non-callable value");
            break;
        }
        case Op::CallMethod: {
            const std::uint32_t nameIdx = oper >> 8;
            const int argc = static_cast<int>(oper & 0xFF);
            ObjString* nm = names_[nameIdx];
            // Раскладка `[receiver][a0..aN-1][callee]`.
            const std::size_t recvPos = ctx_->top - static_cast<std::size_t>(argc) - 2;
            const Value receiver = ctx_->values[recvPos];
            if (!invokeMethod(receiver, nm, argc)) goto onRuntimeError;
            if (suspendRequested_) { suspendFlag = true; break; }
            REFRESH();
            break;
        }
        case Op::InvokeSuper: {
            // Раскладка та же, что у Op::CallMethod: `[superClass][a0..aN-1][callee]`.
            const std::uint32_t nameIdx = oper >> 8;
            const int argc = static_cast<int>(oper & 0xFF);
            ObjString* nm = names_[nameIdx];
            const std::size_t clsPos = ctx_->top - static_cast<std::size_t>(argc) - 2;
            const Value superV = ctx_->values[clsPos];
            if (!superV.isObject() || superV.asObject()->type != ObjHeader::Type::Class)
                RUNTIME_ERR("'super' is not bound to a class here");
            auto* cls = static_cast<ObjClass*>(superV.asObject());
            ObjClosure* m = findMethod(cls, nm);
            if (!m) RUNTIME_ERR(format("superclass has no method '{}'", std::string(nm->view())));
            const Value self = frame->thisValue;
            ctx_->values[ctx_->top - 1] = Value::object(m);
            if (!callClosureAt(m, argc, clsPos + 1, ctx_->top - 1)) goto onRuntimeError;
            CallFrame& nf = ctx_->frames[ctx_->frameCount - 1];
            nf.thisValue = self;
            nf.superValue = cls->superclass ? Value::object(cls->superclass) : Value::nil();
            REFRESH();
            break;
        }
        case Op::Closure: {
            ObjFunction* f = static_cast<ObjFunction*>(CONST(oper).asObject());
            auto* cl = gc_.allocate<ObjClosure>(sizeof(ObjClosure), ObjHeader(ObjHeader::Type::Closure, sizeof(ObjClosure)));
            cl->function = f;
            cl->upvalues = nullptr;
            const std::uint8_t numUp = code[frame->ip++];
            if (numUp) {
                cl->upvalues = static_cast<ObjUpvalue**>(gc_.reallocate(nullptr, 0, numUp * sizeof(ObjUpvalue*)));
                for (std::uint8_t i = 0; i < numUp; ++i) {
                    const bool isLocal = code[frame->ip++] != 0;
                    const std::uint32_t index = readOperand24(code + frame->ip);
                    frame->ip += 3;
                    if (isLocal) {
                        Value* slot = &ctx_->values[frame->base + index];
                        ObjUpvalue** pp = &openUpvalues_;
                        while (*pp && (*pp)->location < slot) pp = &(*pp)->nextOpen;
                        if (*pp && (*pp)->location == slot) {
                            cl->upvalues[i] = *pp;
                        } else {
                            auto* uv = gc_.allocate<ObjUpvalue>(sizeof(ObjUpvalue), ObjHeader(ObjHeader::Type::Upvalue, sizeof(ObjUpvalue)));
                            uv->location = slot;
                            uv->closed = Value::nil();
                            uv->nextOpen = *pp;
                            *pp = uv;
                            cl->upvalues[i] = uv;
                        }
                    } else {
                        cl->upvalues[i] = frame->closure->upvalues[index];
                    }
                    // Замыкание могло почернеть между аллокацией и заполнением
                    // массива upvalue (аллокация ObjUpvalue выше способна
                    // продвинуть инкрементальную разметку).
                    gc_.writeBarrierObject(cl, cl->upvalues[i]);
                }
            }
            PUSH(Value::object(cl));
            break;
        }
        case Op::MakeCoroutine: {
            const Value cl = POP();
            if (!cl.isObject() || cl.asObject()->type != ObjHeader::Type::Closure)
                RUNTIME_ERR("'coroutine' requires a function value");
            const Value co = makeCoroutine(objAs<ObjClosure>(cl));
            if (scheduler_) scheduler_->registerCoroutine(objAs<ObjCoroutine>(co));
            PUSH(co);
            break;
        }
        case Op::Return: {
            if (!doReturn()) return RunStatus::Ok;
            REFRESH();
            break;
        }
        case Op::ReturnNil: {
            PUSH(Value::nil());
            if (!doReturn()) return RunStatus::Ok;
            REFRESH();
            break;
        }

        // -------------------------------------------------------------- Классы
        case Op::Class: {
            // oper — индекс константы, в которой лежит предсозданный ObjClass.
            // Класс регистрируется в рантайме и записывается обратно в константу.
            Value& slot = fn->constants[oper];
            ObjClass* cls = slot.isObject() && slot.asObject()->type == ObjHeader::Type::Class
                                ? static_cast<ObjClass*>(slot.asObject())
                                : lv::makeClass(gc_, internRaw("<class>"));
            cls = declareClass(cls->name->view(), cls->superclass);
            slot = Value::object(cls);
            break;
        }
        case Op::ClassSuper: {
            // [c12 = индекс константы класса][n12 = имя суперкласса в names_].
            // Суперкласс ищется во время выполнения, поэтому порядок объявления
            // файлов не важен (но к моменту создания инстанса он должен быть).
            const std::uint32_t clsConst = (oper >> 12) & 0xFFF;
            ObjString* superName = names_[oper & 0xFFF];
            ObjClass* cls = static_cast<ObjClass*>(fn->constants[clsConst].asObject());
            Value* sv = globals_->findString(superName->view(), superName->hash);
            if (!sv || !sv->isObject() || sv->asObject()->type != ObjHeader::Type::Class)
                RUNTIME_ERR(format("unknown superclass '{}'", std::string(superName->view())));
            if (static_cast<ObjClass*>(sv->asObject()) == cls)
                RUNTIME_ERR(format("class '{}' cannot inherit from itself", std::string(cls->name->view())));
            cls->superclass = static_cast<ObjClass*>(sv->asObject());
            gc_.writeBarrierObject(cls, cls->superclass);
            break;
        }
        case Op::ClassMethod: case Op::StaticMethod: {
            // [c12 = индекс константы класса][n12 = имя метода]; метод = POP().
            const std::uint32_t clsConst = (oper >> 12) & 0xFFF;
            ObjString* nm = names_[oper & 0xFFF];
            const Value closure = POP();
            auto* cls = static_cast<ObjClass*>(fn->constants[clsConst].asObject());
            cls->methods->set(gc_, Value::object(nm), closure);
            if (nm->view() == "init" && closure.isObject() &&
                closure.asObject()->type == ObjHeader::Type::Closure) {
                cls->initializer = static_cast<ObjClosure*>(closure.asObject());
                gc_.writeBarrierObject(cls, cls->initializer);
            }
            break;
        }
        case Op::ClassField: {
            const std::uint32_t clsConst = (oper >> 12) & 0xFFF;
            ObjString* nm = names_[oper & 0xFFF];
            const Value v = POP();
            static_cast<ObjClass*>(fn->constants[clsConst].asObject())->defaults->set(gc_, Value::object(nm), v);
            break;
        }
        case Op::NewInstance: {
            const int argc = static_cast<int>(oper);
            const std::size_t clsPos = ctx_->top - 1 - static_cast<std::size_t>(argc);
            const Value clsV = ctx_->values[clsPos];
            if (!clsV.isObject() || clsV.asObject()->type != ObjHeader::Type::Class)
                RUNTIME_ERR(format("cannot instantiate '{}' — the class is not defined at this point",
                                   clsV.isNil() ? std::string("<unknown>") : clsV.toString()));
            auto* cls = static_cast<ObjClass*>(clsV.asObject());
            ObjInstance* inst = lv::makeInstance(gc_, cls);
            applyFieldDefaults(inst, cls);
            if (ObjClosure* init = findMethod(cls, internRaw("init"))) {
                const std::size_t callerDepth = ctx_->frameCount;
                ctx_->values[clsPos] = Value::object(inst);
                if (!callClosure(init, argc)) goto onRuntimeError;
                CallFrame& nf = ctx_->frames[ctx_->frameCount - 1];
                nf.thisValue = Value::object(inst);
                nf.superValue = cls->superclass ? Value::object(cls->superclass) : Value::nil();
                (void)callerDepth;       // результат init подменяется в doReturn
                REFRESH();
            } else {
                ctx_->top = clsPos;
                PUSH(Value::object(inst));
            }
            break;
        }
        case Op::GetClass: {
            ObjString* nm = names_[oper];
            Value* v = globals_->find(Value::object(nm));
            if (!v) RUNTIME_ERR(format("unknown class '{}'", std::string(nm->view())));
            PUSH(*v);
            break;
        }
        case Op::IsInstance: case Op::IsInstancePeek: {
            const Value t = POP();
            const Value v = (op == Op::IsInstance) ? POP() : STACK(0);
            const std::string tn = (t.isObject() && t.asObject()->type == ObjHeader::Type::String)
                                       ? std::string(objAs<ObjString>(t)->view()) : t.toString();
            PUSH(Value::boolean(isTypeOf(v, tn)));
            break;
        }
        case Op::Cast: {
            const Value t = POP();
            const Value v = POP();
            if (profiling_) ++profile_.calls;   // в строгом режиме типы уже проверены компилятором
            (void)t;
            PUSH(v);
            break;
        }

        // -------------------------------------------------------- Result / try
        case Op::ResultOk:  { const Value v = POP(); PUSH(makeResult(true, v, Value::nil())); break; }
        case Op::ResultErr: { const Value v = POP(); PUSH(makeResult(false, Value::nil(), v)); break; }
        case Op::ResultCheck: {
            const Value v = POP();
            if (v.isObject() && v.asObject()->type == ObjHeader::Type::Result) {
                auto* r = static_cast<ObjResult*>(v.asObject());
                if (!r->ok) {
                    // `?` прерывает функцию и ВОЗВРАЩАЕТ тот же Err(...),
                    // поэтому наружу уходит сам Result, а не «сырая» ошибка.
                    PUSH(v);
                    if (!doReturn()) return RunStatus::Ok;
                    REFRESH();
                    break;
                }
                PUSH(r->value);
            } else PUSH(v);
            break;
        }
        case Op::TryBegin: {
            tryHandlers_.push_back(Value::integer(static_cast<std::int64_t>(frame->ip) + jumpOffset));
            tryCtx_.push_back(ctx_);
            tryFrameDepth_.push_back(ctx_->frameCount);
            tryStackDepth_.push_back(ctx_->top);
            break;
        }
        case Op::TryEnd: popTry(); break;
        case Op::Throw: {
            const Value v = POP();
            RUNTIME_ERR((v.isObject() && v.asObject()->type == ObjHeader::Type::String)
                            ? std::string(objAs<ObjString>(v)->view()) : v.toString());
            break;
        }

        // ------------------------------------------------------------ Корутины
        case Op::Yield: {
            ObjCoroutine* co = ctx_->coroutine;
            if (!co) RUNTIME_ERR("'yield' used outside of a coroutine");
            const Value yielded = oper > 0 ? POP() : Value::nil();
            co->yieldValue = yielded;
            gc_.writeBarrier(co, yielded);
            co->state = ObjCoroutine::State::Suspended;
            ExecutionContext* parent = co->parent;
            if (parent && parent->frameCount > 0) {
                parent->top = co->savedTop;
                parent->push(yielded);   // значение, которое получит выражение `resume co`
                ctx_ = parent;
                REFRESH();
            } else {
                // Корутина приостановлена планировщиком: выходим из run().
                ctx_ = co->returnTo ? co->returnTo : contexts_.front();
                return RunStatus::Yielded;
            }
            break;
        }
        case Op::Resume: {
            const Value v = POP();
            if (!v.isObject() || v.asObject()->type != ObjHeader::Type::Coroutine)
                RUNTIME_ERR("'resume' expects a coroutine");
            auto* co = static_cast<ObjCoroutine*>(v.asObject());
            if (co->state == ObjCoroutine::State::Dead) { PUSH(co->yieldValue); break; }
            if (resumeCoroutine(co, Value::nil()) != RunStatus::Ok) goto onRuntimeError;
            const RunStatus st = run();
            if (st == RunStatus::RuntimeError || st == RunStatus::OutOfFuel) goto onRuntimeError;
            // После возврата управление уже переключено обратно в текущий контекст.
            REFRESH();
            break;
        }
        case Op::Suspend: suspendFlag = true; break;
        case Op::Breakpoint: break;
        case Op::Halt: return RunStatus::Ok;

        default: RUNTIME_ERR(format("unknown opcode {}", static_cast<int>(op)));
        }


        // ---------------------------------------------------- Пост-обработка
        // (Подмена результата `init` на инстанс больше не нужна: это делается
        //  покадрово в doReturn по ObjFunction::Kind::Initializer.)
        if (suspendFlag) {
            suspendFlag = false;
            if (!handleSuspend(framePtr, fn, code)) return RunStatus::RuntimeError;
            if (ctx_->frameCount == 0) return RunStatus::Ok;
            frame = framePtr;
            continue;
        }
        if (gc_.gcRequested()) {
            const auto t0 = std::chrono::steady_clock::now();
            gc_.collect();
            if (profiling_) {
                ++profile_.gcPauses;
                profile_.gcTimeMs +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
        }
        continue;

    onRuntimeError:
        if (handleTryError(framePtr, fn, code)) { error_ = {}; frame = framePtr; continue; }
        return RunStatus::RuntimeError;
    }

#undef BINARY_ARITH
#undef READ_U24
#undef READ_I24
#undef CONST
#undef STACK
#undef PUSH
#undef POP
#undef JUMP_FWD
#undef JUMP_BACK
#undef REFRESH
#undef RUNTIME_ERR
}

// ---------------------------------------------------------------------------
//  Вспомогательные проверки типов
// ---------------------------------------------------------------------------
bool VM::isTypeOf(const Value& v, const std::string& tn) {
    if (v.isObject() && v.asObject()->type == ObjHeader::Type::Instance) {
        for (ObjClass* c = static_cast<ObjInstance*>(v.asObject())->klass; c; c = c->superclass)
            if (c->name->view() == tn) return true;
        return false;
    }
    if (tn == "Any")    return true;
    if (tn == "Nil")    return v.isNil();
    if (tn == "Bool")   return v.isBool();
    if (tn == "Float")  return v.isNumber();
    if (tn == "Int")    return v.isNumber() && v.asNumber() == std::floor(v.asNumber());
    if (!v.isObject())  return false;
    switch (v.asObject()->type) {
        case ObjHeader::Type::String:    return tn == "String";
        case ObjHeader::Type::Array:     return tn == "Array";
        case ObjHeader::Type::Map:       return tn == "Map";
        case ObjHeader::Type::Class:     return tn == "Class";
        case ObjHeader::Type::Coroutine: return tn == "Coroutine";
        case ObjHeader::Type::Result:    return tn == "Result";
        case ObjHeader::Type::Closure:
        case ObjHeader::Type::Function:
        case ObjHeader::Type::NativeFn:  return tn == "Function";
        case ObjHeader::Type::Instance:  return false;
        default: return false;
    }
}

void VM::applyFieldDefaults(ObjInstance* inst, ObjClass* cls) {
    std::vector<ObjClass*> chain;
    for (ObjClass* c = cls; c; c = c->superclass) chain.push_back(c);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        ObjMap* d = (*it)->defaults;
        for (std::uint32_t i = 0; i < d->capacity; ++i) {
            const MapEntry& e = d->entries[i];
            if (e.used) inst->fields->set(gc_, e.key, e.value);
        }
    }
}

// ---------------------------------------------------------------------------
//  Дизассемблер
// ---------------------------------------------------------------------------
const char* opName(Op op) noexcept {
    switch (op) {
#define LV_OP(n) case Op::n: return #n;
        LV_OP(Nop) LV_OP(Dup) LV_OP(Dup1) LV_OP(Swap) LV_OP(Pop) LV_OP(PopN) LV_OP(PeekSlot) LV_OP(PokeSlot) LV_OP(IndexSetTop) LV_OP(IndexGetPeek)
        LV_OP(Constant) LV_OP(Nil) LV_OP(True) LV_OP(False) LV_OP(IntConst) LV_OP(EmptyArray) LV_OP(EmptyMap)
        LV_OP(GetLocal) LV_OP(SetLocal) LV_OP(GetUpvalue) LV_OP(SetUpvalue) LV_OP(GetGlobal)
        LV_OP(DefineGlobal) LV_OP(SetGlobal) LV_OP(GetModule) LV_OP(LoadName) LV_OP(GetThis) LV_OP(GetSuper)
        LV_OP(GetSuperOf)
        LV_OP(Add) LV_OP(Sub) LV_OP(Mul) LV_OP(Div) LV_OP(Mod) LV_OP(Pow) LV_OP(Neg) LV_OP(Not)
        LV_OP(BitAnd) LV_OP(BitOr) LV_OP(BitXor) LV_OP(BitNot) LV_OP(Shl) LV_OP(Shr)
        LV_OP(Equal) LV_OP(NotEqual) LV_OP(Greater) LV_OP(Less) LV_OP(GreaterEq) LV_OP(LessEq)
        LV_OP(IndexGet) LV_OP(IndexSet) LV_OP(MemberGet) LV_OP(MemberSet) LV_OP(MemberGetOpt)
        LV_OP(ArrayPush) LV_OP(ArraySetAt) LV_OP(MapInsert) LV_OP(NewArray) LV_OP(NewMap)
        LV_OP(NewRange) LV_OP(Slice) LV_OP(Len) LV_OP(IterInit) LV_OP(IterNext) LV_OP(IterKeyNext)
        LV_OP(SetLoopSlots) LV_OP(StorePair) LV_OP(ForRangePrep) LV_OP(ForRangeLoop)
        LV_OP(ForIterPrep) LV_OP(ForIterNext) LV_OP(IsInstancePeek)
        LV_OP(Call) LV_OP(CallSpread) LV_OP(CallMethod) LV_OP(InvokeSuper) LV_OP(PackRest)
        LV_OP(Closure) LV_OP(MakeCoroutine) LV_OP(Return) LV_OP(ReturnNil)
        LV_OP(NewInstance) LV_OP(GetClass) LV_OP(Class) LV_OP(ClassSuper) LV_OP(ClassMethod)
        LV_OP(ClassField) LV_OP(StaticMethod) LV_OP(IsInstance) LV_OP(Cast)
        LV_OP(ResultOk) LV_OP(ResultErr) LV_OP(ResultCheck) LV_OP(TryBegin) LV_OP(TryEnd) LV_OP(Throw)
        LV_OP(Concat) LV_OP(SpreadAppend)
        LV_OP(Jump) LV_OP(Loop) LV_OP(JumpIfFalse) LV_OP(JumpIfTrue) LV_OP(JumpIfFalsePop)
        LV_OP(JumpIfTruePop) LV_OP(JumpIfNil) LV_OP(Elvis)
        LV_OP(Yield) LV_OP(Resume) LV_OP(Suspend) LV_OP(Halt) LV_OP(Breakpoint)
#undef LV_OP
    }
    return "?op?";
}

std::size_t Disassembler::disassembleInstruction(const ObjFunction& fn, std::size_t offset,
                                                 std::string& out, const std::vector<ObjString*>* names) {
    const Byte* code = fn.code.data();
    const Op op = static_cast<Op>(code[offset]);
    const std::uint32_t oper = readOperand24(code + offset + 1);
    const std::uint32_t line = fn.lineAt(offset);
    // Ручное выравнивание: lv::format приводит аргументы к строкам до подстановки
    // спецификатора, поэтому printf-флаги ширины применяются к уже готовым полям.
    char head[64];
    std::snprintf(head, sizeof(head), "%05zu %4u %-16s", offset, line, opName(op));
    out += head;

    switch (op) {
        case Op::Constant: {
            out += format("{} '", oper);
            out += oper < fn.constants.size() ? fn.constants[oper].inspect() : std::string("?");
            out += "'";
            break;
        }
        case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal: case Op::GetModule:
        case Op::LoadName:
        case Op::MemberGet: case Op::MemberSet: case Op::MemberGetOpt: case Op::GetClass:
            if (names && oper < names->size()) out += std::string((*names)[oper]->view());
            else out += format("<name {}>", oper);
            break;
        case Op::ClassMethod: case Op::ClassField: case Op::StaticMethod: case Op::ClassSuper: {
            const std::uint32_t cIdx = (oper >> 12) & 0xFFF;
            const std::uint32_t nIdx = oper & 0xFFF;
            out += format("class=const[{}] ", cIdx);
            if (names && nIdx < names->size()) out += std::string((*names)[nIdx]->view());
            else out += format("<name#{}>", nIdx);
            break;
        }
        case Op::Jump: case Op::Loop: case Op::JumpIfFalse: case Op::JumpIfTrue:
        case Op::JumpIfFalsePop: case Op::JumpIfTruePop: case Op::JumpIfNil: case Op::Elvis:
        case Op::IterNext: case Op::IterKeyNext: case Op::ForRangeLoop: case Op::TryBegin:
            out += format("-> {}", static_cast<std::int64_t>(offset + 4) + readSigned24(code + offset + 1));
            break;
        case Op::CallMethod: case Op::InvokeSuper: {
            const std::uint32_t nameIdx = oper >> 8;
            if (names && nameIdx < names->size()) out += std::string((*names)[nameIdx]->view());
            else out += format("<name {}>", nameIdx);
            out += format(" argc={}", oper & 0xFF);
            break;
        }
        case Op::IntConst: {
            std::uint32_t u = oper;
            if (u & 0x800000u) u |= 0xFF000000u;
            out += std::to_string(static_cast<std::int32_t>(u));
            break;
        }
        case Op::Closure: {
            const std::uint8_t numUp = (offset + 4 < fn.code.size()) ? fn.code[offset + 4] : 0;
            if (oper < fn.constants.size() && fn.constants[oper].isObject() &&
                fn.constants[oper].asObject()->type == ObjHeader::Type::Function) {
                auto* nested = static_cast<ObjFunction*>(fn.constants[oper].asObject());
                out += nested->name ? std::string(nested->name->view()) : std::string("<anon>");
            } else {
                out += format("const[{}]", oper);
            }
            out += format(" upvalues={}", numUp);
            out += "\n";
            return 4 + 1 + static_cast<std::size_t>(numUp) * 4;
        }
        case Op::Nop: case Op::Nil: case Op::True: case Op::False: case Op::Add: case Op::Sub:
        case Op::Mul: case Op::Div: case Op::Mod: case Op::Pow: case Op::Neg: case Op::Not:
        case Op::Equal: case Op::NotEqual: case Op::Greater: case Op::Less: case Op::GreaterEq:
        case Op::LessEq: case Op::Pop: case Op::Dup: case Op::Swap: case Op::IndexSetTop: case Op::IndexGetPeek: case Op::Return:
        case Op::ReturnNil: case Op::Halt: case Op::IndexGet: case Op::IndexSet: case Op::Len:
        case Op::IterInit: case Op::TryEnd: case Op::ResultOk:
        case Op::ResultErr: case Op::Suspend: case Op::Breakpoint: case Op::GetThis:
        case Op::GetSuper: case Op::EmptyArray: case Op::EmptyMap: case Op::SpreadAppend:
        case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::BitNot: case Op::Shl:
        case Op::Shr: case Op::MakeCoroutine: case Op::ResultCheck: case Op::Dup1:
        case Op::StorePair: case Op::SetLoopSlots:
            break;
        case Op::Class:
            out += format("class=const[{}]", oper);
            break;
        default:
            out += std::to_string(oper);
            break;
    }
    out += "\n";
    return 4;
}

std::string Disassembler::disassemble(const ObjFunction& fn, const std::vector<ObjString*>* names,
                                      bool recursive) {
    std::string out;
    out += format("== function {} (arity={}, locals={}, upvalues={}, maxStack={}) ==\n",
                       fn.name ? std::string(fn.name->view()) : "<anon>",
                       fn.arity, fn.numLocals, fn.numUpvalues, fn.maxStack);
    std::size_t offset = 0;
    while (offset < fn.code.size()) {
        const std::size_t len = disassembleInstruction(fn, offset, out, names);
        offset += len;
    }
    if (recursive) {
        for (Value c : fn.constants) {
            if (c.isObject() && c.asObject()->type == ObjHeader::Type::Function)
                out += disassemble(*static_cast<ObjFunction*>(c.asObject()), names, true);
        }
    }
    return out;
}

} // namespace lv
