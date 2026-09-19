/**
 * @file    EngineBindings.cpp
 * @brief   Реализация биндингов LV Script <-> движок.
 */
#include "EngineBindings.h"
#include "../lvscript/Compiler.h"
#include "../lvscript/Parser.h"

#include <array>
#include <cstring>

namespace lv::scripting {

// ---------------------------------------------------------------------------
//  Marshaling математических типов
// ---------------------------------------------------------------------------
Value vec3ToValue(VM& vm, const Vec3& v) {
    Value m = vm.makeMap();
    auto* map = static_cast<ObjMap*>(m.asObject());
    map->set(vm.gc(), vm.internString("x"), Value::fromNumber(v.x));
    map->set(vm.gc(), vm.internString("y"), Value::fromNumber(v.y));
    map->set(vm.gc(), vm.internString("z"), Value::fromNumber(v.z));
    return m;
}

Vec3 vec3FromValue(const Value& v) {
    Vec3 out{0, 0, 0};
    if (!v.isObject() || v.asObject()->type != ObjHeader::Type::Map) return out;
    auto* m = static_cast<ObjMap*>(v.asObject());
    // Быстрый путь без интернирования: ищем по содержимому строк-ключей.
    for (std::uint32_t i = 0; i < m->capacity; ++i) {
        const MapEntry& e = m->entries[i];
        if (!e.used || !e.key.isObject() || e.key.asObject()->type != ObjHeader::Type::String) continue;
        const auto name = static_cast<ObjString*>(e.key.asObject())->view();
        const Real val = static_cast<Real>(e.value.asNumber());
        if (name == "x") out.x = val;
        else if (name == "y") out.y = val;
        else if (name == "z") out.z = val;
    }
    return out;
}

Value quatToValue(VM& vm, const Quat& q) {
    Value m = vm.makeMap();
    auto* map = static_cast<ObjMap*>(m.asObject());
    map->set(vm.gc(), vm.internString("x"), Value::fromNumber(q.x));
    map->set(vm.gc(), vm.internString("y"), Value::fromNumber(q.y));
    map->set(vm.gc(), vm.internString("z"), Value::fromNumber(q.z));
    map->set(vm.gc(), vm.internString("w"), Value::fromNumber(q.w));
    return m;
}

Quat quatFromValue(const Value& v) {
    Quat out;
    if (!v.isObject() || v.asObject()->type != ObjHeader::Type::Map) return out;
    auto* m = static_cast<ObjMap*>(v.asObject());
    for (std::uint32_t i = 0; i < m->capacity; ++i) {
        const MapEntry& e = m->entries[i];
        if (!e.used || !e.key.isObject() || e.key.asObject()->type != ObjHeader::Type::String) continue;
        const auto name = static_cast<ObjString*>(e.key.asObject())->view();
        const Real val = static_cast<Real>(e.value.asNumber());
        if (name == "x") out.x = val;
        else if (name == "y") out.y = val;
        else if (name == "z") out.z = val;
        else if (name == "w") out.w = val;
    }
    return out;
}

Value colorToValue(VM& vm, const Color& c) {
    Value m = vm.makeMap();
    auto* map = static_cast<ObjMap*>(m.asObject());
    map->set(vm.gc(), vm.internString("r"), Value::fromNumber(c.r));
    map->set(vm.gc(), vm.internString("g"), Value::fromNumber(c.g));
    map->set(vm.gc(), vm.internString("b"), Value::fromNumber(c.b));
    map->set(vm.gc(), vm.internString("a"), Value::fromNumber(c.a));
    return m;
}

Color colorFromValue(const Value& v) {
    Color out{1, 1, 1, 1};
    if (!v.isObject() || v.asObject()->type != ObjHeader::Type::Map) return out;
    auto* m = static_cast<ObjMap*>(v.asObject());
    for (std::uint32_t i = 0; i < m->capacity; ++i) {
        const MapEntry& e = m->entries[i];
        if (!e.used || !e.key.isObject() || e.key.asObject()->type != ObjHeader::Type::String) continue;
        const auto name = static_cast<ObjString*>(e.key.asObject())->view();
        const Real val = static_cast<Real>(e.value.asNumber());
        if (name == "r") out.r = val;
        else if (name == "g") out.g = val;
        else if (name == "b") out.b = val;
        else if (name == "a") out.a = val;
    }
    return out;
}

Value readField(VM& vm, const void* base, const FieldBinding& f) {
    const auto* p = static_cast<const std::byte*>(base) + f.offset;
    switch (f.kind) {
        case FieldBinding::Kind::Float: {
            Real v; std::memcpy(&v, p, sizeof(Real));
            return Value::fromNumber(static_cast<double>(v));
        }
        case FieldBinding::Kind::Int: {
            std::int64_t v; std::memcpy(&v, p, sizeof(std::int64_t));
            return Value::integer(v);
        }
        case FieldBinding::Kind::Bool: {
            bool v; std::memcpy(&v, p, sizeof(bool));
            return Value::boolean(v);
        }
        case FieldBinding::Kind::Vec3: {
            Vec3 v; std::memcpy(&v, p, sizeof(Vec3));
            return vec3ToValue(vm, v);
        }
        case FieldBinding::Kind::Vec2: {
            Vec2 v; std::memcpy(&v, p, sizeof(Vec2));
            Value m = vm.makeMap();
            auto* map = static_cast<ObjMap*>(m.asObject());
            map->set(vm.gc(), vm.internString("x"), Value::fromNumber(v.x));
            map->set(vm.gc(), vm.internString("y"), Value::fromNumber(v.y));
            return m;
        }
        case FieldBinding::Kind::Quat: {
            Quat v; std::memcpy(&v, p, sizeof(Quat));
            return quatToValue(vm, v);
        }
        case FieldBinding::Kind::Color: {
            Color v; std::memcpy(&v, p, sizeof(Color));
            return colorToValue(vm, v);
        }
        case FieldBinding::Kind::Entity: {
            ecs::Entity e; std::memcpy(&e, p, sizeof(ecs::Entity));
            Value m = vm.makeMap();
            auto* map = static_cast<ObjMap*>(m.asObject());
            map->set(vm.gc(), vm.internString("index"), Value::integer(e.index));
            map->set(vm.gc(), vm.internString("generation"), Value::integer(e.generation));
            return m;
        }
        case FieldBinding::Kind::String: {
            // Строковые поля компонент редки; поддерживаем std::string через
            // вызов копирующего конструктора (не memcpy: у SSO-строк это UB).
            const auto* sp = reinterpret_cast<const std::string*>(p);
            return vm.internString(*sp);
        }
    }
    return Value::nil();
}

bool writeField(const void* base, const FieldBinding& f, const Value& v) {
    if (f.readOnly) return false;
    auto* p = const_cast<std::byte*>(static_cast<const std::byte*>(base) + f.offset);
    switch (f.kind) {
        case FieldBinding::Kind::Float: {
            const Real val = static_cast<Real>(v.asNumber());
            std::memcpy(p, &val, sizeof(Real));
            return true;
        }
        case FieldBinding::Kind::Int: {
            const std::int64_t val = v.asInt();
            std::memcpy(p, &val, sizeof(std::int64_t));
            return true;
        }
        case FieldBinding::Kind::Bool: {
            const bool val = v.truthy();
            std::memcpy(p, &val, sizeof(bool));
            return true;
        }
        case FieldBinding::Kind::Vec3: {
            const Vec3 val = vec3FromValue(v);
            std::memcpy(p, &val, sizeof(Vec3));
            return true;
        }
        case FieldBinding::Kind::Quat: {
            const Quat val = quatFromValue(v);
            std::memcpy(p, &val, sizeof(Quat));
            return true;
        }
        case FieldBinding::Kind::Color: {
            const Color val = colorFromValue(v);
            std::memcpy(p, &val, sizeof(Color));
            return true;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------------------
//  BindingRegistry
// ---------------------------------------------------------------------------
BindingRegistry& BindingRegistry::instance() {
    static BindingRegistry inst;
    return inst;
}

// ---------------------------------------------------------------------------
//  ScriptWorld
// ---------------------------------------------------------------------------
ScriptWorld::ScriptWorld(SandboxConfig sandbox) : vm_(std::move(sandbox)), scheduler_(vm_) {
    installStdlib(vm_);
    if (logCb_) vm_.setLogSink(logCb_);
}

ScriptWorld::~ScriptWorld() { scheduler_.stopAll(); }

void ScriptWorld::attach(const EngineContext& ctx) {
    ctx_ = ctx;
    installEngineBindings(vm_, ctx_);
    vm_.setLogSink([this](std::string s) {
        if (logCb_) logCb_(s);
        else { std::fputs(s.c_str(), stdout); std::fputc('\n', stdout); }
    });
    // Рантайм-ошибки (включая нарушения песочницы) тоже уходят в лог консоли:
    // иначе мод может молча падать, а разработчик увидит только «ничего не произошло».
    vm_.setErrorHandler([this](const ScriptError& e, std::string_view tb) {
        if (logCb_) { logCb_(e.toString()); if (!tb.empty()) logCb_(std::string(tb)); }
        else std::fputs(e.toString().c_str(), stderr);
    });
}

void ScriptWorld::setLogCallback(LogCallback cb) {
    logCb_ = std::move(cb);
    vm_.setLogSink([this](std::string s) {
        if (logCb_) logCb_(s);
        else { std::fputs(s.c_str(), stdout); std::fputc('\n', stdout); }
    });
}

bool ScriptWorld::runFile(const std::string& path, const std::string& source) {
    DiagnosticList diags;
    Module m = parseSource(source, path, diags);
    for (const Diagnostic& d : diags)
        if (logCb_) logCb_(d.toString());
    bool failed = false;
    for (const Diagnostic& d : diags) failed = failed || (d.severity == DiagSeverity::Error);
    if (failed) return false;

    Compiler compiler(vm_);
    ObjFunction* fn = compiler.compile(m, diags);
    for (const Diagnostic& d : diags)
        if (logCb_) logCb_(d.toString());
    if (!fn) return false;

    protos_[path] = fn;
    loaded_.push_back(path);
    const RunStatus st = vm_.execute(fn);
    if (st != RunStatus::Ok && logCb_)
        logCb_(vm_.lastError().toString() + "\n" + vm_.traceback());
    return st == RunStatus::Ok;
}

bool ScriptWorld::hotReload(const std::string& path, const std::string& newSource) {
    DiagnosticList diags;
    Module m = parseSource(newSource, path, diags);
    for (const Diagnostic& d : diags) if (logCb_) logCb_(d.toString());
    for (const Diagnostic& d : diags)
        if (d.severity == DiagSeverity::Error) return false;

    Compiler compiler(vm_);
    ObjFunction* fresh = compiler.compile(m, diags);
    if (!fresh) return false;

    // Подменяем ТЕЛО прототипа, сохраняя сам объект ObjFunction: существующие
    // замыкания и инстансы продолжают ссылаться на него, поэтому состояние
    // (поля объектов, upvalue) переживает перезагрузку.
    auto it = protos_.find(path);
    if (it != protos_.end() && it->second) {
        ObjFunction* old = it->second;
        old->code = std::move(fresh->code);
        old->constants = std::move(fresh->constants);
        old->lineStarts = std::move(fresh->lineStarts);
        old->numLocals = fresh->numLocals;
        old->maxStack = fresh->maxStack;
        old->arity = fresh->arity;
        old->totalParams = fresh->totalParams;
        old->restParamSlot = fresh->restParamSlot;
        old->nested = std::move(fresh->nested);
    } else {
        protos_[path] = fresh;
        loaded_.push_back(path);
    }

    // Повторное исполнение верхнего уровня переустанавливает функции/классы.
    ObjFunction* entry = protos_[path];
    const RunStatus st = vm_.execute(entry);
    if (logCb_) logCb_("[hot-reload] " + path + (st == RunStatus::Ok ? " ok" : " FAILED"));
    return st == RunStatus::Ok;
}

void ScriptWorld::tick(float dt) {
    vm_.setGameTime(vm_.gameTime() + dt);
    scheduler_.tick(dt);

    // update(dt): аргументом передаётся deltaTime кадра.
    std::array<Value, 1> args{Value::fromNumber(dt)};
    dispatch("update", args);

    // Порция сборки мусора В КОНЦЕ кадра, когда игровая логика уже отработала.
    //
    // Смысл инкрементального режима в том, чтобы разложить сборку по кадрам:
    // шаг здесь ограничен бюджетом (см. GC::setStepBudget), поэтому вместо
    // одной заметной паузы в середине геймплея получается равномерная нагрузка.
    // В stop-the-world режиме вызов ничего не делает — сборка идёт из allocate().
    if (vm_.gc().incremental()) vm_.gc().step();
}

void ScriptWorld::dispatch(std::string_view method, std::span<const Value> args) {
    // Конвенция движка: скрипт может объявить глобальные функции-обработчики
    //   update(dt), fixedUpdate(dt), lateUpdate(), onCollision(a, b), onPause()
    // dispatch() вызывает их, если они определены. Это аналог «магических»
    // методов Unity, но явный: список имён виден здесь и в спецификации языка.
    Value* fn = vm_.findGlobal(method);
    if (!fn || !fn->isObject()) return;
    const ObjHeader::Type t = fn->asObject()->type;
    if (t != ObjHeader::Type::Closure && t != ObjHeader::Type::NativeFn) return;

    Value out;
    const RunStatus st = vm_.callValue(*fn, args, &out);
    if (st != RunStatus::Ok && logCb_)
        logCb_(vm_.lastError().toString() + "\n" + vm_.traceback());
}

Value ScriptWorld::wrapEntity(ecs::Entity e) {
    Value m = vm_.makeMap();
    auto* map = static_cast<ObjMap*>(m.asObject());
    map->set(vm_.gc(), vm_.internString("__entity"), Value::boolean(true));
    map->set(vm_.gc(), vm_.internString("index"), Value::integer(e.index));
    map->set(vm_.gc(), vm_.internString("generation"), Value::integer(e.generation));
    return m;
}

ecs::Entity ScriptWorld::unwrapEntity(const Value& v) {
    ecs::Entity e = ecs::kNullEntity;
    if (!v.isObject() || v.asObject()->type != ObjHeader::Type::Map) return e;
    auto* m = static_cast<ObjMap*>(v.asObject());
    if (Value* idx = m->find(Value())) { (void)idx; }
    for (std::uint32_t i = 0; i < m->capacity; ++i) {
        const MapEntry& en = m->entries[i];
        if (!en.used || !en.key.isObject() || en.key.asObject()->type != ObjHeader::Type::String) continue;
        const auto name = static_cast<ObjString*>(en.key.asObject())->view();
        if (name == "index") e.index = static_cast<std::uint32_t>(en.value.asInt());
        else if (name == "generation") e.generation = static_cast<std::uint32_t>(en.value.asInt());
    }
    return e;
}

// ---------------------------------------------------------------------------
//  Установка биндингов
// ---------------------------------------------------------------------------
namespace {

/// Ключевые natives получают доступ к контексту через замыкание на указатель.
struct Natives {
    EngineContext* ctx = nullptr;
};

ecs::Entity argEntity(const Value& v) { return ScriptWorld::unwrapEntity(v); }

} // namespace

void registerEngineComponents() {
    auto& reg = BindingRegistry::instance();
    using FK = FieldBinding::Kind;

    reg.registerComponent<ecs::Transform>("Transform");
    reg.field<ecs::Transform>("Transform", "position", &ecs::Transform::position, FK::Vec3);
    reg.field<ecs::Transform>("Transform", "rotation", &ecs::Transform::rotation, FK::Quat);
    reg.field<ecs::Transform>("Transform", "scale", &ecs::Transform::scale, FK::Vec3);

    reg.registerComponent<render::MeshRenderer>("MeshRenderer");
    reg.field<render::MeshRenderer>("MeshRenderer", "tint", &render::MeshRenderer::tint, FK::Color);

    reg.registerComponent<physics::RigidBody>("RigidBody");
    reg.registerComponent<audio::AudioSource>("AudioSource");
    reg.registerComponent<input::DeviceState>("InputState");   // редко используется, но доступен
}

void installEngineBindings(VM& vm, EngineContext& ctx) {
    registerEngineComponents();

    // Контекст захватывается по указателю: он живёт столько же, сколько VM.
    EngineContext* c = &ctx;

    // ------------------------------------------------------------------ Vec3
    auto vecNative = [&](const char* name, std::uint32_t arity, ObjNativeFn::Fn fn) {
        vm.registerNative(name, arity, std::move(fn), Cap_Math);
    };
    vecNative("vec3", 3, [](VM& v, std::span<const Value> a) {
        return vec3ToValue(v, Vec3{static_cast<Real>(a[0].asNumber()),
                                   static_cast<Real>(a[1].asNumber()),
                                   static_cast<Real>(a[2].asNumber())});
    });
    vecNative("vec3Add", 2, [](VM& v, std::span<const Value> a) {
        return vec3ToValue(v, vec3FromValue(a[0]) + vec3FromValue(a[1]));
    });
    vecNative("vec3Sub", 2, [](VM& v, std::span<const Value> a) {
        return vec3ToValue(v, vec3FromValue(a[0]) - vec3FromValue(a[1]));
    });
    vecNative("vec3Mul", 2, [](VM& v, std::span<const Value> a) {
        return vec3ToValue(v, vec3FromValue(a[0]) * static_cast<Real>(a[1].asNumber()));
    });
    vecNative("vec3Length", 1, [](VM&, std::span<const Value> a) {
        return Value::fromNumber(length(vec3FromValue(a[0])));
    });
    vecNative("vec3Distance", 2, [](VM&, std::span<const Value> a) {
        return Value::fromNumber(distance(vec3FromValue(a[0]), vec3FromValue(a[1])));
    });
    vecNative("vec3Normalize", 1, [](VM& v, std::span<const Value> a) {
        return vec3ToValue(v, normalize(vec3FromValue(a[0])));
    });
    vecNative("vec3Lerp", 3, [](VM& v, std::span<const Value> a) {
        const Real t = static_cast<Real>(a[2].asNumber());
        const Vec3 from = vec3FromValue(a[0]), to = vec3FromValue(a[1]);
        return vec3ToValue(v, Vec3{lerp(from.x, to.x, t), lerp(from.y, to.y, t), lerp(from.z, to.z, t)});
    });
    vecNative("color", 4, [](VM& v, std::span<const Value> a) {
        return colorToValue(v, Color{static_cast<Real>(a[0].asNumber()), static_cast<Real>(a[1].asNumber()),
                                     static_cast<Real>(a[2].asNumber()),
                                     a.size() > 3 ? static_cast<Real>(a[3].asNumber()) : 1.0f});
    });

    // ----------------------------------------------------------------- Entity
    vm.registerNative("spawn", 0, [c](VM& v, std::span<const Value>) -> Value {
        v.requireCapability(Cap_Spawn, "spawn()");
        if (!c->world) { v.runtimeErrorPublic("spawn(): no world attached"); return Value::nil(); }
        ecs::Entity e = c->world->registry().create();
        c->world->registry().add<ecs::Transform>(e);
        Value m = v.makeMap();
        auto* map = static_cast<ObjMap*>(m.asObject());
        map->set(v.gc(), v.internString("__entity"), Value::boolean(true));
        map->set(v.gc(), v.internString("index"), Value::integer(e.index));
        map->set(v.gc(), v.internString("generation"), Value::integer(e.generation));
        return m;
    }, Cap_Spawn);

    vm.registerNative("entity", 2, [](VM& v, std::span<const Value> a) -> Value {
        Value m = v.makeMap();
        auto* map = static_cast<ObjMap*>(m.asObject());
        map->set(v.gc(), v.internString("__entity"), Value::boolean(true));
        map->set(v.gc(), v.internString("index"), Value::integer(a[0].asInt()));
        map->set(v.gc(), v.internString("generation"), Value::integer(a[1].asInt()));
        return m;
    });

    vm.registerNative("alive", 1, [c](VM& v, std::span<const Value> a) -> Value {
        if (!c->world) return Value::boolean(false);
        return Value::boolean(c->world->registry().alive(argEntity(a[0])));
    }, Cap_Scene);

    vm.registerNative("destroy", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Spawn, "destroy()");
        if (!c->world) return Value::nil();
        c->world->commands().destroy(argEntity(a[0]));
        return Value::nil();
    }, Cap_Spawn);

    vm.registerNative("entityCount", 0, [c](VM&, std::span<const Value>) -> Value {
        return Value::integer(c->world ? static_cast<std::int64_t>(c->world->registry().entityCount()) : 0);
    }, Cap_Scene);

    // ------------------------------------------------------- Компоненты (get/set)
    vm.registerNative("getComponent", 2, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Scene, "getComponent()");
        if (!c->world) return Value::nil();
        const ecs::Entity e = argEntity(a[0]);
        const ComponentBinding* b = BindingRegistry::instance().byName(a[1].toString());
        if (!b) return Value::nil();
        void* comp = c->world->registry().getTypeId(e, b->typeId);
        if (!comp) return Value::nil();
        Value m = v.makeMap();
        auto* map = static_cast<ObjMap*>(m.asObject());
        map->set(v.gc(), v.internString("__component"), v.internString(b->name));
        for (const FieldBinding& f : b->fields)
            map->set(v.gc(), v.internString(f.name), readField(v, comp, f));
        return m;
    }, Cap_Scene);

    vm.registerNative("setField", 4, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Scene, "setField()");
        if (!c->world) return Value::boolean(false);
        const ecs::Entity e = argEntity(a[0]);
        const ComponentBinding* b = BindingRegistry::instance().byName(a[1].toString());
        if (!b) return Value::boolean(false);
        void* comp = c->world->registry().getTypeId(e, b->typeId);
        if (!comp) {
            // Компонента ещё нет — СОЗДАЁМ его (Unity-подобная семантика
            // `GetComponent<T>() ?? AddComponent<T>()`).
            //
            // Без этого `setField(e, "MeshRenderer", "tint", color(...))` молча
            // возвращал false, и все визуальные настройки в шаблонах (цвет
            // грядок, домов, актёров) не применялись: spawn() добавляет только
            // Transform. Ошибка здесь была бы не лучше — скрипты игровых
            // шаблонов не обязаны знать порядок добавления компонентов.
            c->world->registry().addDynamic(e, b->typeId);
            comp = c->world->registry().getTypeId(e, b->typeId);
            if (!comp) return Value::boolean(false);
        }
        const FieldBinding* f = b->field(a[2].toString());
        if (!f) return Value::boolean(false);
        return Value::boolean(writeField(comp, *f, a[3]));
    }, Cap_Scene);

    vm.registerNative("getField", 3, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Scene, "getField()");
        if (!c->world) return Value::nil();
        const ecs::Entity e = argEntity(a[0]);
        const ComponentBinding* b = BindingRegistry::instance().byName(a[1].toString());
        if (!b) return Value::nil();
        const void* comp = c->world->registry().getTypeId(e, b->typeId);
        if (!comp) return Value::nil();
        const FieldBinding* f = b->field(a[2].toString());
        if (!f) return Value::nil();
        return readField(v, comp, *f);
    }, Cap_Scene);

    vm.registerNative("hasComponent", 2, [c](VM& v, std::span<const Value> a) -> Value {
        if (!c->world) return Value::boolean(false);
        const ComponentBinding* b = BindingRegistry::instance().byName(a[1].toString());
        if (!b) return Value::boolean(false);
        return Value::boolean(c->world->registry().hasTypeId(argEntity(a[0]), b->typeId));
    }, Cap_Scene);

    // Быстрые шорткаты для самого горячего компонента — Transform.
    vm.registerNative("getPosition", 1, [c](VM& v, std::span<const Value> a) -> Value {
        if (!c->world) return Value::nil();
        const ecs::Transform* t = c->world->registry().get<ecs::Transform>(argEntity(a[0]));
        return t ? vec3ToValue(v, t->position) : Value::nil();
    }, Cap_Scene);

    // Запись позиции из скрипта ОБЯЗАНА двигать и физическое тело.
    //
    // Для динамических тел источником истины является тело: PhysicsWorld::
    // syncToECS() каждый кадр перезаписывает Transform позицией тела, поэтому
    // «голая» запись в компонент отменялась бы на следующем кадре (телепорт
    // игрока к медпакету в шаблоне fps_shooter молча не срабатывал).
    auto writePosition = [c](const ecs::Entity e, const Vec3& p) {
        if (!c->world) return;
        if (ecs::Transform* t = c->world->registry().get<ecs::Transform>(e)) t->position = p;
        if (c->physics)
            if (const physics::RigidBody* rb = c->world->registry().get<physics::RigidBody>(e)) {
                const ecs::Transform& t = c->world->registry().require<ecs::Transform>(e);
                c->physics->teleportBody(rb->body, p, t.rotation);
            }
    };

    vm.registerNative("setPosition", 2, [c, writePosition](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Scene, "setPosition()");
        if (!c->world) return Value::nil();
        writePosition(argEntity(a[0]), vec3FromValue(a[1]));
        return Value::nil();
    }, Cap_Scene);

    vm.registerNative("translate", 2, [c, writePosition](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Scene, "translate()");
        if (!c->world) return Value::nil();
        const ecs::Entity e = argEntity(a[0]);
        const ecs::Transform* t = c->world->registry().get<ecs::Transform>(e);
        if (!t) return Value::nil();
        writePosition(e, t->position + vec3FromValue(a[1]));
        return Value::nil();
    }, Cap_Scene);

    vm.registerNative("setRotationEuler", 2, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Scene, "setRotationEuler()");
        if (!c->world) return Value::nil();
        if (ecs::Transform* t = c->world->registry().get<ecs::Transform>(argEntity(a[0]))) {
            const Vec3 e = vec3FromValue(a[1]);
            t->rotation = Quat::fromEuler(radians(e.x), radians(e.y), radians(e.z));
        }
        return Value::nil();
    }, Cap_Scene);

    // ------------------------------------------------------------------ Input
    vm.registerNative("inputPressed", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.pressed()");
        return Value::boolean(c->input && c->input->pressed(a[0].toString()));
    }, Cap_Input);
    vm.registerNative("inputHeld", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.held()");
        return Value::boolean(c->input && c->input->held(a[0].toString()));
    }, Cap_Input);
    vm.registerNative("inputReleased", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.released()");
        return Value::boolean(c->input && c->input->released(a[0].toString()));
    }, Cap_Input);
    vm.registerNative("inputAxis", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.axis()");
        return Value::fromNumber(c->input ? c->input->axis(a[0].toString()) : 0.0);
    }, Cap_Input);
    vm.registerNative("inputAxis2D", 2, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.axis2D()");
        if (!c->input) return vec3ToValue(v, Vec3::zero());
        const Vec2 ax = c->input->axis2D(a[0].toString(), a[1].toString());
        return vec3ToValue(v, Vec3{ax.x, 0, ax.y});
    }, Cap_Input);
    vm.registerNative("mouseDelta", 0, [c](VM& v, std::span<const Value>) -> Value {
        v.requireCapability(Cap_Input, "mouse.delta()");
        if (!c->input) return vec3ToValue(v, Vec3::zero());
        const Vec2 d = c->input->mouseDelta();
        return vec3ToValue(v, Vec3{d.x, d.y, 0});
    }, Cap_Input);
    vm.registerNative("pushInputContext", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.pushContext()");
        if (c->input) c->input->pushContext(a[0].toString());
        return Value::nil();
    }, Cap_Input);
    vm.registerNative("popInputContext", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.popContext()");
        if (c->input) c->input->popContext(a[0].toString());
        return Value::nil();
    }, Cap_Input);

    // Контекст ввода создаётся ИЗ СКРИПТА: шаблоны и моды описывают свою
    // раскладку рядом со своей логикой, а не в C++-коде движка. Контекст
    // сразу помещается в стек (priority можно задать вторым аргументом).
    vm.registerNative("createInputContext", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.createContext()");
        if (!c->input) return Value::boolean(false);
        const std::string name = a[0].toString();
        const std::int32_t priority = (a.size() > 1) ? static_cast<std::int32_t>(a[1].asInt()) : 0;
        c->input->createContext(name, priority);
        c->input->pushContext(name);
        return Value::boolean(true);
    }, Cap_Input);

    // Действие: `mapKey("Farm", "Interact", 69)` — E.
    // Клавиша передаётся числовым кодом (см. docs/04_engine_systems.md), чтобы
    // не тянуть в скрипты таблицу имён клавиш.
    vm.registerNative("mapKey", 3, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.mapKey()");
        if (!c->input) return Value::boolean(false);
        auto* ctx = c->input->findContext(a[0].toString());
        if (!ctx) return Value::boolean(false);
        ctx->mapKey(a[1].toString(), static_cast<input::KeyCode>(a[2].asInt()));
        return Value::boolean(true);
    }, Cap_Input);

    // Ось: `mapKeyAxis("Farm", "MoveX", 68, 1.0)` — D даёт +1.
    vm.registerNative("mapKeyAxis", 4, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.mapKeyAxis()");
        if (!c->input) return Value::boolean(false);
        auto* ctx = c->input->findContext(a[0].toString());
        if (!ctx) return Value::boolean(false);
        ctx->mapKeyAxis(a[1].toString(), static_cast<input::KeyCode>(a[2].asInt()),
                        static_cast<Real>(a[3].asNumber()));
        return Value::boolean(true);
    }, Cap_Input);

    // Кнопка мыши: `mapMouseButton("FPS", "Fire", 0)` — ЛКМ.
    vm.registerNative("mapMouseButton", 3, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Input, "input.mapMouseButton()");
        if (!c->input) return Value::boolean(false);
        auto* ctx = c->input->findContext(a[0].toString());
        if (!ctx) return Value::boolean(false);
        ctx->mapMouseButton(a[1].toString(), static_cast<std::uint8_t>(a[2].asInt()));
        return Value::boolean(true);
    }, Cap_Input);

    // ---------------------------------------------------------------- Physics
    vm.registerNative("raycast", 3, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Physics, "raycast()");
        Value out = v.makeMap();
        auto* map = static_cast<ObjMap*>(out.asObject());
        map->set(v.gc(), v.internString("hit"), Value::boolean(false));
        if (!c->physics) return out;
        Ray r{vec3FromValue(a[0]), normalize(vec3FromValue(a[1]))};
        const physics::RaycastHit hit = c->physics->raycast(r, static_cast<Real>(a[2].asNumber()));
        map->set(v.gc(), v.internString("hit"), Value::boolean(hit.hit));
        if (hit.hit) {
            map->set(v.gc(), v.internString("position"), vec3ToValue(v, hit.position));
            map->set(v.gc(), v.internString("normal"), vec3ToValue(v, hit.normal));
            map->set(v.gc(), v.internString("distance"), Value::fromNumber(hit.distance));
            Value ent = v.makeMap();
            auto* em = static_cast<ObjMap*>(ent.asObject());
            em->set(v.gc(), v.internString("__entity"), Value::boolean(true));
            em->set(v.gc(), v.internString("index"), Value::integer(hit.entity.index));
            em->set(v.gc(), v.internString("generation"), Value::integer(hit.entity.generation));
            map->set(v.gc(), v.internString("entity"), ent);
        }
        return out;
    }, Cap_Physics);

    // addRigidBody(entity, halfExtents, kind)
    //   kind: 0 = Static, 1 = Kinematic, 2 = Dynamic (см. physics::BodyKind).
    //
    // ВАЖНО для шаблонов: у ДИНАМИЧЕСКОГО тела источником истины является само
    // тело — PhysicsWorld::syncToECS() каждый кадр перезаписывает Transform его
    // позицией, а syncFromECS() игнорирует Transform. Поэтому «телепорт»
    // динамического тела обязан идти через PhysicsWorld::teleportBody() (его и
    // вызывает биндинг setPosition). Персонажи, которыми управляет скрипт,
    // в шаблонах создаются КИНЕМАТИКАМИ: у них Transform — источник истины,
    // и setPosition() работает без оговорок.
    vm.registerNative("addRigidBody", 3, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Physics, "addRigidBody()");
        if (!c->physics || !c->world) return Value::nil();
        physics::BodyDesc d;
        d.kind = static_cast<physics::BodyKind>(static_cast<std::uint8_t>(a[2].asInt()));
        const ecs::Entity e = argEntity(a[0]);
        if (const ecs::Transform* t = c->world->registry().get<ecs::Transform>(e)) {
            d.position = t->position;
            d.rotation = t->rotation;
        }
        d.shape.kind = physics::ShapeKind::Box;
        d.shape.halfExtents = vec3FromValue(a[1]);
        d.entity = e;
        const physics::BodyId body = c->physics->createBody(d);
        c->world->registry().add<physics::RigidBody>(e, physics::RigidBody{body, true});
        return Value::integer(body);
    }, Cap_Physics);

    vm.registerNative("setVelocity", 2, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Physics, "setVelocity()");
        if (!c->physics || !c->world) return Value::nil();
        const ecs::Entity e = argEntity(a[0]);
        if (const physics::RigidBody* rb = c->world->registry().get<physics::RigidBody>(e))
            c->physics->setLinearVelocity(rb->body, vec3FromValue(a[1]));
        return Value::nil();
    }, Cap_Physics);

    vm.registerNative("applyImpulse", 2, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Physics, "applyImpulse()");
        if (!c->physics || !c->world) return Value::nil();
        const ecs::Entity e = argEntity(a[0]);
        if (const physics::RigidBody* rb = c->world->registry().get<physics::RigidBody>(e))
            c->physics->applyImpulse(rb->body, vec3FromValue(a[1]),
                                     c->world->registry().require<ecs::Transform>(e).position);
        return Value::nil();
    }, Cap_Physics);

    // ------------------------------------------------------------------ Audio
    vm.registerNative("playSound2D", 2, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Audio, "playSound2D()");
        if (!c->audio) return Value::integer(-1);
        audio::ClipHandle clip{static_cast<std::uint32_t>(a[0].asInt())};
        const audio::VoiceHandle voice = c->audio->play2D(clip, static_cast<Real>(a[1].asNumber()));
        return Value::integer(voice.index);
    }, Cap_Audio);
    vm.registerNative("playSound3D", 3, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Audio, "playSound3D()");
        if (!c->audio) return Value::integer(-1);
        audio::ClipHandle clip{static_cast<std::uint32_t>(a[0].asInt())};
        const audio::VoiceHandle voice = c->audio->play3D(clip, vec3FromValue(a[1]),
                                                          static_cast<Real>(a[2].asNumber()));
        return Value::integer(voice.index);
    }, Cap_Audio);
    vm.registerNative("stopSound", 1, [c](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Audio, "stopSound()");
        if (c->audio) c->audio->stop(audio::VoiceHandle{static_cast<std::uint32_t>(a[0].asInt())});
        return Value::nil();
    }, Cap_Audio);

    // ------------------------------------------------------------------ Время
    vm.registerNative("deltaTime", 0, [c](VM&, std::span<const Value>) -> Value {
        return Value::fromNumber(c->world ? c->world->frame().deltaTime : 0.0);
    }, Cap_Math);
    vm.registerNative("time", 0, [c](VM& v, std::span<const Value>) -> Value {
        if (!v.sandbox().deterministic && c->world) return Value::fromNumber(c->world->frame().unscaledTime);
        return Value::fromNumber(v.gameTime());
    }, Cap_Math);
    vm.registerNative("frameIndex", 0, [c](VM&, std::span<const Value>) -> Value {
        return Value::integer(c->world ? static_cast<std::int64_t>(c->world->frame().frameIndex) : 0);
    }, Cap_Math);

    // ------------------------------------------------------------ Сборщик мусора
    // Управление GC требует Cap_Debug: мод не должен провоцировать паузы в
    // чужой игре, а вот инструменты и сама игра — могут.

    /// `gcCollect()` — полная сборка немедленно. Уместно на загрузочном экране
    /// или после выгрузки уровня, когда пауза никому не мешает.
    vm.registerNative("gcCollect", 0, [](VM& v, std::span<const Value>) -> Value {
        v.requireCapability(Cap_Debug, "gcCollect()");
        return Value::integer(static_cast<std::int64_t>(v.gc().collect()));
    }, Cap_Debug);

    /// `gcStep()` — одна порция работы; true, если цикл завершился.
    vm.registerNative("gcStep", 0, [](VM& v, std::span<const Value>) -> Value {
        v.requireCapability(Cap_Debug, "gcStep()");
        return Value::boolean(v.gc().step());
    }, Cap_Debug);

    /// `gcSetIncremental(on)` — переключить режим сборки.
    vm.registerNative("gcSetIncremental", 1, [](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Debug, "gcSetIncremental()");
        v.gc().setIncremental(a[0].truthy());
        return Value::nil();
    }, Cap_Debug);

    /// `gcSetStepBudget(bytes)` — бюджет одного шага.
    vm.registerNative("gcSetStepBudget", 1, [](VM& v, std::span<const Value> a) -> Value {
        v.requireCapability(Cap_Debug, "gcSetStepBudget()");
        const std::int64_t bytes = a[0].asInt();
        v.gc().setStepBudget(bytes > 0 ? static_cast<std::size_t>(bytes) : 1);
        return Value::nil();
    }, Cap_Debug);

    /// `gcStats()` — карта со статистикой для HUD и отладочных оверлеев.
    vm.registerNative("gcStats", 0, [](VM& v, std::span<const Value>) -> Value {
        const GC::Stats s = v.gc().stats();
        Value m = v.makeMap();
        auto* map = static_cast<ObjMap*>(m.asObject());
        map->set(v.gc(), v.internString("objects"),     Value::integer(static_cast<std::int64_t>(s.objects)));
        map->set(v.gc(), v.internString("bytes"),       Value::integer(static_cast<std::int64_t>(s.bytes)));
        map->set(v.gc(), v.internString("collections"), Value::integer(static_cast<std::int64_t>(s.collections)));
        map->set(v.gc(), v.internString("freed"),       Value::integer(static_cast<std::int64_t>(s.totalFreed)));
        map->set(v.gc(), v.internString("steps"),       Value::integer(static_cast<std::int64_t>(s.steps)));
        map->set(v.gc(), v.internString("lastPauseMs"), Value::fromNumber(s.lastPauseMs));
        map->set(v.gc(), v.internString("maxPauseMs"),  Value::fromNumber(s.maxPauseMs));
        const char* phase = s.phase == GC::Phase::Mark  ? "mark"
                          : s.phase == GC::Phase::Sweep ? "sweep" : "idle";
        map->set(v.gc(), v.internString("phase"), v.internString(phase));
        return m;
    }, Cap_Debug);
}

} // namespace lv::scripting
