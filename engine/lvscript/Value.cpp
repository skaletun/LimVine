/**
 * @file    Value.cpp
 * @brief   Реализация объектной модели, сравнения и печати значений.
 */
#include "Value.h"
#include "GC.h"

#include <sstream>

namespace lv {

const char* ObjHeader::typeName() const noexcept {
    switch (type) {
        case Type::String:    return "String";
        case Type::Array:     return "Array";
        case Type::Map:       return "Map";
        case Type::Function:  return "Function";
        case Type::Closure:   return "Function";
        case Type::Upvalue:   return "Upvalue";
        case Type::Class:     return "Class";
        case Type::Instance:  return "Instance";
        case Type::Coroutine: return "Coroutine";
        case Type::NativeFn:  return "NativeFn";
        case Type::Result:    return "Result";
        case Type::Iterator:  return "Iterator";
    }
    return "Object";
}

// ---------------------------------------------------------------------------
//  ObjArray
// ---------------------------------------------------------------------------
void ObjArray::grow(GC& gc, std::uint32_t needed) {
    std::uint32_t cap = capacity == 0 ? 8 : capacity;
    while (cap < needed) cap *= 2;
    if (cap == capacity) return;
    const std::size_t oldBytes = capacity * sizeof(Value);
    const std::size_t newBytes = cap * sizeof(Value);
    items = static_cast<Value*>(gc.reallocate(items, oldBytes, newBytes));
    capacity = cap;
}

// ---------------------------------------------------------------------------
//  ObjMap (открытая адресация, линейное пробирование)
// ---------------------------------------------------------------------------
static inline std::uint32_t mapProbe(std::uint64_t hash, std::uint32_t capacity) noexcept {
    return static_cast<std::uint32_t>(hash) & (capacity - 1u);
}

Value* ObjMap::find(Value key) {
    if (capacity == 0) return nullptr;
    const std::uint64_t h = key.hash();
    std::uint32_t idx = mapProbe(h, capacity);
    for (;;) {
        MapEntry& e = entries[idx];
        if (!e.used) return nullptr;
        if (e.hash == (h & 0x7fffffffffffffffULL) && valuesEqual(e.key, key)) return &e.value;
        idx = (idx + 1) & (capacity - 1u);
    }
}

Value* ObjMap::findString(std::string_view name, std::uint64_t hash) {
    if (capacity == 0) return nullptr;
    std::uint32_t idx = mapProbe(hash, capacity);
    for (;;) {
        MapEntry& e = entries[idx];
        if (!e.used) return nullptr;
        if (e.key.isObject() && e.key.asObject()->type == ObjHeader::Type::String) {
            auto* s = e.key.as<ObjString>();
            if (s->hash == hash && s->view() == name) return &e.value;
        }
        idx = (idx + 1) & (capacity - 1u);
    }
}

void ObjMap::resize(GC& gc, std::uint32_t newCapacity) {
    MapEntry* old = entries;
    const std::uint32_t oldCap = capacity;

    entries = static_cast<MapEntry*>(gc.reallocate(nullptr, 0, newCapacity * sizeof(MapEntry)));
    capacity = newCapacity;
    for (std::uint32_t i = 0; i < newCapacity; ++i) entries[i] = MapEntry{};
    count = 0;

    for (std::uint32_t i = 0; i < oldCap; ++i) {
        MapEntry& e = old[i];
        if (!e.used) continue;
        std::uint32_t idx = mapProbe(e.hash, newCapacity);
        while (entries[idx].used) idx = (idx + 1) & (newCapacity - 1u);
        entries[idx] = e;
        ++count;
    }
    if (old) gc.reallocate(old, oldCap * sizeof(MapEntry), 0);
}

bool ObjMap::set(GC& gc, Value key, Value value) {
    if (count + 1 > capacity * 3 / 4) {
        std::uint32_t newCap = capacity < 8 ? 8 : capacity * 2;
        resize(gc, newCap);
    }
    const std::uint64_t h = key.hash();
    std::uint32_t idx = mapProbe(h, capacity);
    bool isNew = true;
    for (;;) {
        MapEntry& e = entries[idx];
        if (!e.used) {
            e.used = true;
            e.hash = h & 0x7fffffffffffffffULL;
            e.key = key;
            e.value = value;
            ++count;
            break;
        }
        if (e.hash == (h & 0x7fffffffffffffffULL) && valuesEqual(e.key, key)) {
            e.value = value;
            isNew = false;
            break;
        }
        idx = (idx + 1) & (capacity - 1u);
    }
    gc.writeBarrier(this, key);
    gc.writeBarrier(this, value);
    return isNew;
}

bool ObjMap::remove(Value key) {
    if (capacity == 0) return false;
    const std::uint64_t h = key.hash();
    std::uint32_t idx = mapProbe(h, capacity);
    for (;;) {
        MapEntry& e = entries[idx];
        if (!e.used) return false;
        if (e.hash == (h & 0x7fffffffffffffffULL) && valuesEqual(e.key, key)) {
            // Удаляем с восстановлением кластера (backward-shift deletion).
            e.used = false;
            e.key = Value::nil();
            e.value = Value::nil();
            --count;
            std::uint32_t j = idx;
            for (;;) {
                j = (j + 1) & (capacity - 1u);
                if (!entries[j].used) break;
                const std::uint32_t ideal = mapProbe(entries[j].hash, capacity);
                // Если элемент j «не на своём месте» относительно дыры idx — сдвигаем.
                const bool wrap = (ideal <= idx) != (j <= idx);
                if (!wrap) { entries[idx] = entries[j]; entries[j] = MapEntry{}; idx = j; }
            }
            return true;
        }
        idx = (idx + 1) & (capacity - 1u);
    }
}

// ---------------------------------------------------------------------------
//  ObjFunction
// ---------------------------------------------------------------------------
std::uint32_t ObjFunction::lineAt(std::size_t ip) const noexcept {
    if (lineStarts.empty()) return 0;
    if (ip >= lineStarts.size()) return lineStarts.back();
    return lineStarts[ip];
}

// ---------------------------------------------------------------------------
//  Фабрики
// ---------------------------------------------------------------------------
ObjString* makeRawString(GC& gc, std::string_view sv, std::uint64_t hash) {
    const std::size_t size = sizeof(ObjString) + sv.size() + 1;
    ObjString* s = gc.allocate<ObjString>(size, ObjHeader(ObjHeader::Type::String, static_cast<std::uint32_t>(size)));
    s->hash = hash;
    s->length = static_cast<std::uint32_t>(sv.size());
    std::memcpy(s->data, sv.data(), sv.size());
    s->data[sv.size()] = '\0';
    return s;
}

ObjArray* makeArray(GC& gc, std::uint32_t capacity) {
    ObjArray* a = gc.allocate<ObjArray>(sizeof(ObjArray), ObjHeader(ObjHeader::Type::Array, sizeof(ObjArray)));
    a->count = 0;
    a->capacity = 0;
    a->items = nullptr;
    if (capacity) {
        a->items = static_cast<Value*>(gc.reallocate(nullptr, 0, capacity * sizeof(Value)));
        a->capacity = capacity;
    }
    return a;
}

ObjMap* makeMap(GC& gc) {
    ObjMap* m = gc.allocate<ObjMap>(sizeof(ObjMap), ObjHeader(ObjHeader::Type::Map, sizeof(ObjMap)));
    m->count = 0;
    m->capacity = 0;
    m->entries = nullptr;
    return m;
}

ObjClass* makeClass(GC& gc, ObjString* name) {
    ObjClass* c = gc.allocate<ObjClass>(sizeof(ObjClass), ObjHeader(ObjHeader::Type::Class, sizeof(ObjClass)));
    c->name = name;
    c->superclass = nullptr;
    c->methods = makeMap(gc);
    c->defaults = makeMap(gc);
    c->initializer = nullptr;
    static std::uint32_t sNextClassId = 1;
    c->classId = sNextClassId++;
    return c;
}

ObjInstance* makeInstance(GC& gc, ObjClass* klass) {
    ObjInstance* i = gc.allocate<ObjInstance>(sizeof(ObjInstance), ObjHeader(ObjHeader::Type::Instance, sizeof(ObjInstance)));
    i->klass = klass;
    i->fields = makeMap(gc);
    return i;
}

ObjResult* makeResult(GC& gc, bool ok, Value v, Value e) {
    ObjResult* r = gc.allocate<ObjResult>(sizeof(ObjResult), ObjHeader(ObjHeader::Type::Result, sizeof(ObjResult)));
    r->ok = ok;
    r->value = v;
    r->error = e;
    return r;
}

// ---------------------------------------------------------------------------
//  Сравнение и хэш
// ---------------------------------------------------------------------------
static std::uint64_t hashDouble(double d) noexcept {
    std::uint64_t b;
    std::memcpy(&b, &d, sizeof(d));
    return b;
}

std::uint64_t Value::hash() const noexcept {
    if (isNil())   return 0x9e3779b97f4a7c15ULL;
    if (isBool())  return asBool() ? 1 : 2;
    if (isNumber())return hashDouble(asNumber());
    ObjHeader* o = asObject();
    switch (o->type) {
        case ObjHeader::Type::String: return static_cast<ObjString*>(o)->hash;
        default: return hashDouble(static_cast<double>(reinterpret_cast<std::uintptr_t>(o)));
    }
}

bool valuesEqual(const Value& a, const Value& b) {
    if (a.bitwiseEquals(b)) return true;
    if (a.isNumber() && b.isNumber()) return a.asNumber() == b.asNumber();
    if (!a.isObject() || !b.isObject()) return false;
    ObjHeader* ao = a.asObject();
    ObjHeader* bo = b.asObject();
    if (ao->type != bo->type) return false;
    if (ao->type == ObjHeader::Type::String) {
        auto* sa = static_cast<ObjString*>(ao);
        auto* sb = static_cast<ObjString*>(bo);
        return sa->hash == sb->hash && sa->view() == sb->view();
    }
    if (ao->type == ObjHeader::Type::Array) {
        auto* aa = static_cast<ObjArray*>(ao);
        auto* ab = static_cast<ObjArray*>(bo);
        if (aa->count != ab->count) return false;
        for (std::uint32_t i = 0; i < aa->count; ++i)
            if (!valuesEqual(aa->items[i], ab->items[i])) return false;
        return true;
    }
    if (ao->type == ObjHeader::Type::Map) {
        auto* ma = static_cast<ObjMap*>(ao);
        auto* mb = static_cast<ObjMap*>(bo);
        if (ma->count != mb->count) return false;
        for (std::uint32_t i = 0; i < ma->capacity; ++i) {
            const MapEntry& e = ma->entries[i];
            if (!e.used) continue;
            Value* other = mb->find(e.key);
            if (!other || !valuesEqual(*other, e.value)) return false;
        }
        return true;
    }
    return false; // остальные объекты — по идентичности (уже обработано bitwiseEquals)
}

// ---------------------------------------------------------------------------
//  Печать
// ---------------------------------------------------------------------------
const char* Value::typeName() const noexcept {
    if (isNil())    return "Nil";
    if (isBool())   return "Bool";
    if (isNumber()) {
        double d = asNumber();
        return (d == std::floor(d) && std::fabs(d) < 9.0e15) ? "Int" : "Float";
    }
    ObjHeader* o = asObject();
    if (o->type == ObjHeader::Type::Instance)
        return "Instance";
    return o->typeName();
}

std::string Value::toString() const {
    if (isNil())    return "nil";
    if (isBool())   return asBool() ? "true" : "false";
    if (isNumber()) {
        double d = asNumber();
        if (d == std::floor(d) && std::fabs(d) < 9.0e15) return intToString(asInt());
        return numberToString(d);
    }
    ObjHeader* o = asObject();
    switch (o->type) {
        case ObjHeader::Type::String: return std::string(static_cast<ObjString*>(o)->view());
        case ObjHeader::Type::Class:  return std::string(static_cast<ObjClass*>(o)->name->view());
        case ObjHeader::Type::Instance: {
            auto* i = static_cast<ObjInstance*>(o);
            return format("<{} instance>", std::string(i->klass->name->view()));
        }
        case ObjHeader::Type::Function:
        case ObjHeader::Type::Closure: {
            auto* fn = o->type == ObjHeader::Type::Closure ? static_cast<ObjClosure*>(o)->function
                                                           : static_cast<ObjFunction*>(o);
            return format("<fn {}>", fn->name ? std::string(fn->name->view()) : "anonymous");
        }
        case ObjHeader::Type::NativeFn:
            return format("<native {}>", std::string(static_cast<ObjNativeFn*>(o)->name->view()));
        case ObjHeader::Type::Coroutine:
            return "<coroutine>";
        case ObjHeader::Type::Result: {
            auto* r = static_cast<ObjResult*>(o);
            return r->ok ? ("Ok(" + r->value.toString() + ")") : ("Err(" + r->error.toString() + ")");
        }
        case ObjHeader::Type::Array: {
            auto* a = static_cast<ObjArray*>(o);
            std::string s = "[";
            for (std::uint32_t i = 0; i < a->count; ++i) { if (i) s += ", "; s += a->items[i].inspect(); }
            return s + "]";
        }
        case ObjHeader::Type::Map: {
            auto* m = static_cast<ObjMap*>(o);
            std::string s = "{";
            bool first = true;
            for (std::uint32_t i = 0; i < m->capacity; ++i) {
                if (!m->entries[i].used) continue;
                if (!first) s += ", ";
                first = false;
                s += m->entries[i].key.inspect() + ": " + m->entries[i].value.inspect();
            }
            return s + "}";
        }
        default: break;
    }
    return format("<{}@{}>", o->typeName(), reinterpret_cast<std::uintptr_t>(o));
}

std::string Value::inspect() const {
    if (isObject() && asObject()->type == ObjHeader::Type::String)
        return "\"" + toString() + "\"";
    return toString();
}

} // namespace lv
