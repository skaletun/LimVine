/**
 * @file    GC.cpp
 * @brief   Реализация mark-and-sweep сборщика.
 */
#include "GC.h"

#include <chrono>
#include <cstdlib>

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

void GC::markPhase() {
    if (roots_) roots_->markRoots(*this);
    while (!grayStack_.empty()) {
        ObjHeader* obj = grayStack_.back();
        grayStack_.pop_back();
        traceObject(obj);
    }
}

std::size_t GC::sweepPhase() {
    std::size_t freed = 0;
    ObjHeader** pp = &objects_;
    while (*pp) {
        ObjHeader* obj = *pp;
        if (obj->marked) {
            obj->marked = false;
            pp = &obj->next;
        } else if (obj->pinned) {
            obj->marked = false;
            pp = &obj->next;
        } else {
            *pp = obj->next;
            bytesAllocated_ -= obj->bytes;
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
            ++freed;
        }
    }
    return freed;
}

std::size_t GC::collect() {
    const auto t0 = std::chrono::steady_clock::now();
    inCollection_ = true;
    markPhase();
    const std::size_t freed = sweepPhase();
    inCollection_ = false;
    gcRequested_ = false;

    ++collections_;
    totalCollected_ += freed;
    objectCount_ -= freed;
    nextThreshold_ = static_cast<std::size_t>(static_cast<double>(bytesAllocated_) * growthFactor_);
    if (nextThreshold_ < bytesAllocated_ + 4096) nextThreshold_ = bytesAllocated_ + 4096;

    const auto t1 = std::chrono::steady_clock::now();
    lastPauseMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return freed;
}

GC::Stats GC::stats() const noexcept {
    return Stats{objectCount_, bytesAllocated_, collections_, totalCollected_, lastPauseMs_};
}

} // namespace lv
