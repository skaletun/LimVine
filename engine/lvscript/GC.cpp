/**
 * @file    GC.cpp
 * @brief   Реализация инкрементального трёхцветного mark-and-sweep сборщика.
 */
#include "GC.h"

#include <chrono>
#include <cstdlib>
#include <limits>

namespace lv {

GC::GC(RootProvider* roots, std::size_t initialThreshold)
    : roots_(roots), nextThreshold_(initialThreshold) {
    grayStack_.reserve(256);
}

GC::~GC() {
    // Освобождаем все объекты. Деструкторы вызываются в два прохода, потому что
    // внутренние буферы (items/entries) принадлежат тому же аллокатору.
    ObjHeader* obj = objects_;
    while (obj) {
        ObjHeader* next = obj->next;
        switch (obj->type) {
            case ObjHeader::Type::Array: {
                auto* a = static_cast<ObjArray*>(obj);
                std::free(a->items);
                break;
            }
            case ObjHeader::Type::Map: {
                auto* m = static_cast<ObjMap*>(obj);
                std::free(m->entries);
                break;
            }
            case ObjHeader::Type::Function:
                static_cast<ObjFunction*>(obj)->~ObjFunction();
                break;
            case ObjHeader::Type::Coroutine:
                static_cast<ObjCoroutine*>(obj)->~ObjCoroutine();
                break;
            case ObjHeader::Type::Class:
                static_cast<ObjClass*>(obj)->~ObjClass();
                break;
            case ObjHeader::Type::NativeFn:
                static_cast<ObjNativeFn*>(obj)->~ObjNativeFn();
                break;
            case ObjHeader::Type::Iterator:
                static_cast<ObjIterator*>(obj)->~ObjIterator();
                break;
            default: break;
        }
        ::operator delete(obj);
        obj = next;
    }
    objects_ = nullptr;
    bytesAllocated_ = 0;
    objectCount_ = 0;
}

void* GC::reallocate(void* ptr, std::size_t oldSize, std::size_t newSize) {
    bytesAllocated_ -= oldSize;
    bytesAllocated_ += newSize;
    if (newSize > oldSize && bytesAllocated_ > memoryLimit_) {
        // Превышение лимита песочницы: сначала пытаемся собрать, затем — ошибка.
        collect();
        if (bytesAllocated_ > memoryLimit_) {
            throw std::bad_alloc(); // перехватывается VM и превращается в ScriptError
        }
    }
    if (newSize == 0) {
        std::free(ptr);
        return nullptr;
    }
    return std::realloc(ptr, newSize);
}

void GC::mark(Value v) noexcept {
    if (v.isObject()) markObject(v.asObject());
}

void GC::markObject(ObjHeader* obj) noexcept {
    if (!obj || obj->marked) return;
    obj->marked = true;
    grayStack_.push_back(obj);
}

void GC::traceObject(ObjHeader* obj) {
    switch (obj->type) {
        case ObjHeader::Type::String:
            break;
        case ObjHeader::Type::Array: {
            auto* a = static_cast<ObjArray*>(obj);
            for (std::uint32_t i = 0; i < a->count; ++i) mark(a->items[i]);
            break;
        }
        case ObjHeader::Type::Map: {
            auto* m = static_cast<ObjMap*>(obj);
            for (std::uint32_t i = 0; i < m->capacity; ++i) {
                if (!m->entries[i].used) continue;
                mark(m->entries[i].key);
                mark(m->entries[i].value);
            }
            break;
        }
        case ObjHeader::Type::Function: {
            auto* f = static_cast<ObjFunction*>(obj);
            markObject(f->name);
            for (Value c : f->constants) mark(c);
            for (ObjFunction* n : f->nested) markObject(n);
            break;
        }
        case ObjHeader::Type::Closure: {
            auto* c = static_cast<ObjClosure*>(obj);
            markObject(c->function);
            for (std::uint32_t i = 0; i < c->function->numUpvalues; ++i) markObject(c->upvalues[i]);
            break;
        }
        case ObjHeader::Type::Upvalue: {
            auto* uv = static_cast<ObjUpvalue*>(obj);
            mark(uv->closed);
            if (uv->location && uv->location != &uv->closed) mark(*uv->location);
            break;
        }
        case ObjHeader::Type::Class: {
            auto* c = static_cast<ObjClass*>(obj);
            markObject(c->name);
            markObject(c->superclass);
            markObject(c->methods);
            markObject(c->initializer);
            for (ObjString* f : c->fieldNames) markObject(f);
            break;
        }
        case ObjHeader::Type::Instance: {
            auto* i = static_cast<ObjInstance*>(obj);
            markObject(i->klass);
            markObject(i->fields);
            break;
        }
        case ObjHeader::Type::Coroutine: {
            auto* co = static_cast<ObjCoroutine*>(obj);
            markObject(co->root);
            for (const Value& v : co->stack) mark(v);
            // Собственный стек и кадры корутины живут в её ExecutionContext.
            if (co->ownerContext) {
                ExecutionContext* c = co->ownerContext;
                const std::size_t top = std::min(c->top, c->values.size());
                for (std::size_t i = 0; i < top; ++i) mark(c->values[i]);
                for (std::size_t i = 0; i < c->frameCount; ++i) {
                    mark(c->frames[i].thisValue);
                    mark(c->frames[i].superValue);
                    if (c->frames[i].closure) markObject(c->frames[i].closure);
                }
            }
            mark(co->resumeValue);
            mark(co->yieldValue);
            markObject(co->waitEvent);
            break;
        }
        case ObjHeader::Type::NativeFn:
            markObject(static_cast<ObjNativeFn*>(obj)->name);
            break;
        case ObjHeader::Type::Result: {
            auto* r = static_cast<ObjResult*>(obj);
            mark(r->value);
            mark(r->error);
            break;
        }
        case ObjHeader::Type::Iterator: {
            auto* it = static_cast<ObjIterator*>(obj);
            mark(it->source);
            markObject(it->generator);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
//  Освобождение одного объекта
// ---------------------------------------------------------------------------
namespace {

/// Вызвать деструктор и освободить внутренние буферы объекта.
void destroyObject(ObjHeader* obj) {
    switch (obj->type) {
        case ObjHeader::Type::Array:
            std::free(static_cast<ObjArray*>(obj)->items);
            break;
        case ObjHeader::Type::Map:
            std::free(static_cast<ObjMap*>(obj)->entries);
            break;
        case ObjHeader::Type::Function:
            static_cast<ObjFunction*>(obj)->~ObjFunction();
            break;
        case ObjHeader::Type::Coroutine:
            static_cast<ObjCoroutine*>(obj)->~ObjCoroutine();
            break;
        case ObjHeader::Type::Class:
            static_cast<ObjClass*>(obj)->~ObjClass();
            break;
        case ObjHeader::Type::NativeFn:
            static_cast<ObjNativeFn*>(obj)->~ObjNativeFn();
            break;
        case ObjHeader::Type::Iterator:
            static_cast<ObjIterator*>(obj)->~ObjIterator();
            break;
        default: break;
    }
    ::operator delete(obj);
}

} // namespace

std::size_t GC::workOf(const ObjHeader* obj) noexcept {
    // Стоимость обхода пропорциональна числу просматриваемых ссылок.
    // Точность здесь не нужна — важно, чтобы крупные контейнеры «стоили»
    // дороже, иначе один массив на миллион элементов съедал бы весь кадр.
    switch (obj->type) {
        case ObjHeader::Type::Array:
            return 32 + static_cast<const ObjArray*>(obj)->count * sizeof(Value);
        case ObjHeader::Type::Map:
            return 32 + static_cast<const ObjMap*>(obj)->capacity * sizeof(MapEntry);
        case ObjHeader::Type::Function: {
            const auto* f = static_cast<const ObjFunction*>(obj);
            return 32 + f->constants.size() * sizeof(Value) + f->nested.size() * sizeof(void*);
        }
        case ObjHeader::Type::Coroutine: {
            const auto* co = static_cast<const ObjCoroutine*>(obj);
            std::size_t w = 64 + co->stack.size() * sizeof(Value);
            if (co->ownerContext) w += co->ownerContext->top * sizeof(Value);
            return w;
        }
        default:
            return 32;
    }
}

void GC::notePause(double ms) noexcept {
    lastPauseMs_ = ms;
    if (ms > maxPauseMs_) maxPauseMs_ = ms;
}

void GC::setIncremental(bool on) noexcept {
    if (incremental_ == on) return;
    // Переключение посреди цикла оставило бы половину графа серой, поэтому
    // текущий цикл сначала доводится до конца.
    if (phase_ != Phase::Idle) collect();
    incremental_ = on;
}

void GC::markRootsIntoGray() {
    grayStack_.clear();
    if (roots_) roots_->markRoots(*this);

    // Закреплённые объекты — это ТОЖЕ корни.
    //
    // pin() гарантирует, что сам объект переживёт sweep, но не говорит ничего
    // о том, на что он ссылается. Модуль stdlib (`math`, `Array`, ...) —
    // закреплённая ObjMap, чьи значения обычными объектами не являются
    // корнями: без этого обхода нативные функции внутри модуля оказывались
    // белыми и освобождались, а скрипт падал на «undefined name 'math'».
    //
    // Раньше это не всплывало, потому что sweep оставлял такие объекты в покое
    // до первой же полной сборки — сейчас цикл честно завершается, и
    // недостижимые дети закреплённых объектов действительно удаляются.
    for (ObjHeader* obj = objects_; obj; obj = obj->next)
        if (obj->pinned) markObject(obj);
}

bool GC::markStep(std::size_t budget) {
    std::size_t spent = 0;
    while (!grayStack_.empty()) {
        ObjHeader* obj = grayStack_.back();
        grayStack_.pop_back();
        spent += workOf(obj);
        traceObject(obj);
        if (spent >= budget) return grayStack_.empty();
    }
    return true;
}

bool GC::sweepStep(std::size_t budget, std::size_t& freed) {
    std::size_t spent = 0;
    if (!sweepCursor_) sweepCursor_ = &objects_;

    while (*sweepCursor_) {
        ObjHeader* obj = *sweepCursor_;
        spent += 16;
        if (obj->marked || obj->pinned) {
            obj->marked = false;                  // подготовка к следующему циклу
            sweepCursor_ = &obj->next;
        } else {
            *sweepCursor_ = obj->next;            // выкусываем из списка
            bytesAllocated_ -= obj->bytes;
            destroyObject(obj);
            ++freed;
        }
        if (spent >= budget) return *sweepCursor_ == nullptr;
    }
    return true;
}

void GC::finishCycle() {
    phase_ = Phase::Idle;
    sweepCursor_ = nullptr;
    gcRequested_ = false;
    ++collections_;
    totalCollected_ += cycleFreed_;
    objectCount_ -= cycleFreed_;
    cycleFreed_ = 0;

    nextThreshold_ = static_cast<std::size_t>(static_cast<double>(bytesAllocated_) * growthFactor_);
    if (nextThreshold_ < bytesAllocated_ + 4096) nextThreshold_ = bytesAllocated_ + 4096;
}

bool GC::stepInternal(std::size_t budget) {
    if (budget == 0) budget = stepBudget_;
    const auto t0 = std::chrono::steady_clock::now();

    inCollection_ = true;
    bool finished = false;

    switch (phase_) {
        case Phase::Idle:
            // Новый цикл: корни красятся целиком (их немного и они должны быть
            // согласованным снимком), дальше граф обходится порциями.
            cycleFreed_ = 0;
            markRootsIntoGray();
            phase_ = Phase::Mark;
            if (markStep(budget)) {
                phase_ = Phase::Sweep;
                sweepCursor_ = &objects_;
            }
            break;

        case Phase::Mark:
            if (markStep(budget)) {
                phase_ = Phase::Sweep;
                sweepCursor_ = &objects_;
            }
            break;

        case Phase::Sweep:
            if (sweepStep(budget, cycleFreed_)) {
                finishCycle();
                finished = true;
            }
            break;
    }

    inCollection_ = false;
    ++steps_;
    notePause(std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0).count());
    return finished;
}

bool GC::step(std::size_t budget) {
    // Вне цикла шаг начинает сборку только при достижении порога — иначе
    // вызов раз в кадр крутил бы сборщик вхолостую.
    if (phase_ == Phase::Idle && bytesAllocated_ < nextThreshold_ && !gcRequested_) return false;
    return stepInternal(budget);
}

std::size_t GC::collect() {
    const auto t0 = std::chrono::steady_clock::now();
    inCollection_ = true;

    // Полный цикл. Если инкрементальная сборка была на середине, она
    // доводится до конца в этом же вызове: фаза Mark продолжается с текущего
    // grayStack (корни уже учтены), фаза Sweep — с текущего курсора.
    if (phase_ == Phase::Idle) {
        cycleFreed_ = 0;
        markRootsIntoGray();
        phase_ = Phase::Mark;
    }
    if (phase_ == Phase::Mark) {
        while (!markStep(std::numeric_limits<std::size_t>::max())) {}
        phase_ = Phase::Sweep;
        sweepCursor_ = &objects_;
    }
    while (!sweepStep(std::numeric_limits<std::size_t>::max(), cycleFreed_)) {}

    const std::size_t freed = cycleFreed_;
    finishCycle();

    inCollection_ = false;
    notePause(std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0).count());
    return freed;
}

GC::Stats GC::stats() const noexcept {
    Stats s;
    s.objects     = objectCount_;
    s.bytes       = bytesAllocated_;
    s.collections = collections_;
    s.totalFreed  = totalCollected_;
    s.lastPauseMs = lastPauseMs_;
    s.maxPauseMs  = maxPauseMs_;
    s.steps       = steps_;
    s.phase       = phase_;
    return s;
}

} // namespace lv
