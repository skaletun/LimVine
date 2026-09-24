#include "scene/Scene.h"

#include "ecs/Hierarchy.h"
#include "scripting/EngineBindings.h"
#include "scripting/visual/JsonMini.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace lv::scene {
namespace {

using scripting::BindingRegistry;
using scripting::ComponentBinding;
using scripting::FieldBinding;

// ---------------------------------------------------------------------------
//  Чтение/запись полей компонента напрямую в JSON.
//
//  Мы НЕ переиспользуем readField/writeField из EngineBindings: те работают
//  через lv::Value и требуют живой VM. Сцена должна сохраняться в headless
//  инструментах (lvcook) и в редакторе без запущенных скриптов, поэтому здесь
//  отдельный маршалинг по тем же offset/kind.
// ---------------------------------------------------------------------------

template <class T>
[[nodiscard]] T readRaw(const void* base, std::size_t offset) {
    T v{};
    std::memcpy(&v, static_cast<const std::byte*>(base) + offset, sizeof(T));
    return v;
}

template <class T>
void writeRaw(void* base, std::size_t offset, const T& v) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &v, sizeof(T));
}

json::Value vec2Json(const Vec2& v) {
    json::Object o;
    o["x"] = json::Value(static_cast<double>(v.x));
    o["y"] = json::Value(static_cast<double>(v.y));
    return json::Value(std::move(o));
}
json::Value vec3Json(const Vec3& v) {
    json::Object o;
    o["x"] = json::Value(static_cast<double>(v.x));
    o["y"] = json::Value(static_cast<double>(v.y));
    o["z"] = json::Value(static_cast<double>(v.z));
    return json::Value(std::move(o));
}
json::Value quatJson(const Quat& q) {
    json::Object o;
    o["x"] = json::Value(static_cast<double>(q.x));
    o["y"] = json::Value(static_cast<double>(q.y));
    o["z"] = json::Value(static_cast<double>(q.z));
    o["w"] = json::Value(static_cast<double>(q.w));
    return json::Value(std::move(o));
}
json::Value colorJson(const Color& c) {
    json::Object o;
    o["r"] = json::Value(static_cast<double>(c.r));
    o["g"] = json::Value(static_cast<double>(c.g));
    o["b"] = json::Value(static_cast<double>(c.b));
    o["a"] = json::Value(static_cast<double>(c.a));
    return json::Value(std::move(o));
}

Real num(const json::Value& v, const char* key, Real def = 0) {
    const json::Value& f = v[key];
    return f.isNumber() ? static_cast<Real>(f.asNumber()) : def;
}

/// Ссылки на сущности откладываются: во время первого прохода целевой
/// сущности может ещё не существовать.
struct PendingEntityRef {
    ecs::Entity  owner;          ///< Кому принадлежит компонент.
    ecs::TypeId  type = 0;       ///< Тип компонента (адрес берём после всех миграций).
    std::size_t  offset = 0;     ///< Смещение поля.
    std::int64_t targetId = -1;  ///< id сущности в файле.
    std::string  where;          ///< Для диагностики.
};

} // namespace

// ---------------------------------------------------------------------------
//  Компонент Name
// ---------------------------------------------------------------------------
void registerSceneComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    ecs::registerComponent<Name>("Name");
    auto& reg = BindingRegistry::instance();
    reg.registerComponent<Name>("Name");
    reg.field<Name>("Name", "value", &Name::value, FieldBinding::Kind::String);
}

void setEntityName(ecs::Registry& reg, ecs::Entity e, std::string name) {
    registerSceneComponents();
    if (!reg.alive(e)) return;
    Name n;
    n.value = std::move(name);
    reg.add<Name>(e, n);
}

std::string entityName(const ecs::Registry& reg, ecs::Entity e) {
    const Name* n = reg.get<Name>(e);
    return n ? n->value : std::string{};
}

// ---------------------------------------------------------------------------
//  Сохранение
// ---------------------------------------------------------------------------
std::string saveSceneToString(const ecs::World& world, const SaveOptions& opts) {
    registerSceneComponents();
    const ecs::Registry& reg = world.registry();
    const BindingRegistry& bindings = BindingRegistry::instance();

    // Плотная нумерация: файл не должен зависеть от того, какие слоты
    // Entity были переиспользованы в этом запуске.
    std::vector<ecs::Entity> entities = reg.allEntities();
    std::sort(entities.begin(), entities.end(), [](ecs::Entity a, ecs::Entity b) {
        return a.index < b.index;
    });
    std::unordered_map<std::uint64_t, std::int64_t> idOf;
    idOf.reserve(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i)
        idOf[ecs::entityId(entities[i])] = static_cast<std::int64_t>(i);

    auto refId = [&](ecs::Entity e) -> json::Value {
        const auto it = idOf.find(ecs::entityId(e));
        // Ссылка на мёртвую/внешнюю сущность сохраняется как null, а не как
        // мусорный индекс: при загрузке она станет kNullEntity с предупреждением.
        if (it == idOf.end()) return json::Value(nullptr);
        return json::Value(it->second);
    };

    json::Array entArray;
    entArray.reserve(entities.size());

    const ecs::TypeId nameType     = ecs::typeIdOf<Name>();
    const ecs::TypeId parentType   = ecs::typeIdOf<ecs::Parent>();
    const ecs::TypeId childrenType = ecs::typeIdOf<ecs::Children>();
    const ecs::TypeId worldTfType  = ecs::typeIdOf<ecs::WorldTransform>();

    for (std::size_t i = 0; i < entities.size(); ++i) {
        const ecs::Entity e = entities[i];
        json::Object eo;
        eo["id"] = json::Value(static_cast<std::int64_t>(i));

        if (const Name* n = reg.get<Name>(e); n && !n->value.empty())
            eo["name"] = json::Value(n->value);

        if (const ecs::Parent* p = reg.get<ecs::Parent>(e); p && p->entity.valid())
            eo["parent"] = refId(p->entity);

        // Компоненты собираем в std::map (внутри json::Object) — порядок по
        // имени стабилен, значит сохранение идемпотентно.
        json::Object comps;
        for (const auto& [typeId, b] : bindings.all()) {
            // Name и Parent уже записаны выше как поля сущности; Children и
            // WorldTransform производны от Parent/Transform и пересчитываются
            // при загрузке, поэтому в файл не попадают.
            if (typeId == nameType || typeId == parentType) continue;
            if (typeId == childrenType || typeId == worldTfType) continue;

            const void* comp = const_cast<ecs::Registry&>(reg).getTypeId(e, typeId);
            if (!comp) continue;

            json::Object fields;
            for (const FieldBinding& f : b.fields) {
                switch (f.kind) {
                    case FieldBinding::Kind::Float:
                        fields[f.name] = json::Value(static_cast<double>(readRaw<Real>(comp, f.offset)));
                        break;
                    case FieldBinding::Kind::Int:
                        fields[f.name] = json::Value(readRaw<std::int64_t>(comp, f.offset));
                        break;
                    case FieldBinding::Kind::Bool:
                        fields[f.name] = json::Value(readRaw<bool>(comp, f.offset));
                        break;
                    case FieldBinding::Kind::Vec2:
                        fields[f.name] = vec2Json(readRaw<Vec2>(comp, f.offset));
                        break;
                    case FieldBinding::Kind::Vec3:
                        fields[f.name] = vec3Json(readRaw<Vec3>(comp, f.offset));
                        break;
                    case FieldBinding::Kind::Quat:
                        fields[f.name] = quatJson(readRaw<Quat>(comp, f.offset));
                        break;
                    case FieldBinding::Kind::Color:
                        fields[f.name] = colorJson(readRaw<Color>(comp, f.offset));
                        break;
                    case FieldBinding::Kind::Entity:
                        fields[f.name] = refId(readRaw<ecs::Entity>(comp, f.offset));
                        break;
                    case FieldBinding::Kind::String: {
                        // std::string нельзя читать через memcpy (SSO-указатель
                        // внутрь самого объекта) — только по ссылке.
                        const auto* sp = reinterpret_cast<const std::string*>(
                            static_cast<const std::byte*>(comp) + f.offset);
                        fields[f.name] = json::Value(*sp);
                        break;
                    }
                }
            }
            comps[b.name] = json::Value(std::move(fields));
        }
        eo["components"] = json::Value(std::move(comps));
        entArray.push_back(json::Value(std::move(eo)));
    }

    json::Object root;
    root["format"]   = json::Value("lvscene");
    root["version"]  = json::Value(kSceneFormatVersion);
    root["entities"] = json::Value(std::move(entArray));
    return json::Value(std::move(root)).dump(opts.indent);
}

bool saveSceneToFile(const ecs::World& world, const std::string& path,
                     const SaveOptions& opts, std::string* error) {
    const std::string text = saveSceneToString(world, opts);
    // Атомарная запись: частично записанный .lvscene не должен затирать
    // рабочую сцену, если процесс упал посередине.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot open for writing: " + tmp;
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) {
            if (error) *error = "write failed: " + tmp;
            return false;
        }
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (error) *error = "rename failed: " + tmp + " -> " + path;
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Загрузка
// ---------------------------------------------------------------------------
SceneLoadResult loadSceneFromString(ecs::World& world, std::string_view text) {
    registerSceneComponents();
    SceneLoadResult res;

    json::Value root;
    std::string err;
    if (!json::parse(text, root, err)) {
        res.error = "JSON parse error: " + err;
        return res;
    }
    if (!root.isObject()) {
        res.error = "root is not an object";
        return res;
    }
    if (root["format"].asStringOr("") != "lvscene") {
        res.error = "not a .lvscene file (missing \"format\": \"lvscene\")";
        return res;
    }
    const std::int64_t version = root["version"].asInt(0);
    if (version <= 0 || version > kSceneFormatVersion) {
        res.error = "unsupported .lvscene version " + std::to_string(version) +
                    " (engine supports up to " + std::to_string(kSceneFormatVersion) + ")";
        return res;
    }
    if (!root["entities"].isArray()) {
        res.error = "\"entities\" must be an array";
        return res;
    }

    ecs::Registry& reg = world.registry();
    const BindingRegistry& bindings = BindingRegistry::instance();
    const json::Array& arr = root["entities"].asArray();

    // Проход 1: создать все сущности, чтобы ссылки указывали на существующее.
    // Нумерация в файле обязана быть плотной 0..N-1; "id" проверяем, но
    // позицию в массиве считаем истиной — так сцена с дырами тоже грузится.
    res.entities.resize(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        res.entities[i] = reg.create();
        const std::int64_t declared = arr[i]["id"].asInt(static_cast<std::int64_t>(i));
        if (declared != static_cast<std::int64_t>(i))
            res.warnings.push_back("entity at index " + std::to_string(i) +
                                   " declares id " + std::to_string(declared) +
                                   "; using positional index instead");
    }

    auto resolve = [&](std::int64_t id) -> ecs::Entity {
        if (id < 0 || static_cast<std::size_t>(id) >= res.entities.size()) return ecs::kNullEntity;
        return res.entities[static_cast<std::size_t>(id)];
    };

    std::vector<PendingEntityRef> pending;
    std::vector<std::pair<ecs::Entity, std::int64_t>> parentLinks;

    // Проход 2: компоненты и поля.
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const json::Value& eo = arr[i];
        const ecs::Entity e = res.entities[i];
        const std::string where = "entity #" + std::to_string(i);

        if (eo["name"].isString()) setEntityName(reg, e, eo["name"].asString());
        if (eo["parent"].isNumber()) parentLinks.emplace_back(e, eo["parent"].asInt(-1));

        if (!eo["components"].isObject()) continue;
        for (const auto& [compName, compVal] : eo["components"].asObject()) {
            const ComponentBinding* b = bindings.byName(compName);
            if (!b) {
                res.warnings.push_back(where + ": unknown component \"" + compName + "\" (skipped)");
                continue;
            }
            if (!compVal.isObject()) {
                res.warnings.push_back(where + ": component \"" + compName + "\" is not an object (skipped)");
                continue;
            }
            reg.addDynamic(e, b->typeId);
            void* comp = reg.getTypeId(e, b->typeId);
            if (!comp) {
                res.warnings.push_back(where + ": failed to create component \"" + compName + "\"");
                continue;
            }

            for (const auto& [fieldName, fieldVal] : compVal.asObject()) {
                const FieldBinding* f = b->field(fieldName);
                if (!f) {
                    res.warnings.push_back(where + ": unknown field \"" + compName + "." +
                                           fieldName + "\" (skipped)");
                    continue;
                }
                switch (f->kind) {
                    case FieldBinding::Kind::Float:
                        writeRaw<Real>(comp, f->offset, static_cast<Real>(fieldVal.asNumber()));
                        break;
                    case FieldBinding::Kind::Int:
                        writeRaw<std::int64_t>(comp, f->offset, fieldVal.asInt());
                        break;
                    case FieldBinding::Kind::Bool:
                        writeRaw<bool>(comp, f->offset, fieldVal.asBool());
                        break;
                    case FieldBinding::Kind::Vec2:
                        writeRaw<Vec2>(comp, f->offset, Vec2{num(fieldVal, "x"), num(fieldVal, "y")});
                        break;
                    case FieldBinding::Kind::Vec3:
                        writeRaw<Vec3>(comp, f->offset,
                                       Vec3{num(fieldVal, "x"), num(fieldVal, "y"), num(fieldVal, "z")});
                        break;
                    case FieldBinding::Kind::Quat:
                        writeRaw<Quat>(comp, f->offset,
                                       Quat{num(fieldVal, "x"), num(fieldVal, "y"),
                                            num(fieldVal, "z"), num(fieldVal, "w", 1)});
                        break;
                    case FieldBinding::Kind::Color:
                        writeRaw<Color>(comp, f->offset,
                                        Color{num(fieldVal, "r", 1), num(fieldVal, "g", 1),
                                              num(fieldVal, "b", 1), num(fieldVal, "a", 1)});
                        break;
                    case FieldBinding::Kind::Entity: {
                        // Отложено: addDynamic ниже по циклу мигрирует сущность
                        // между архетипами и инвалидирует `comp`.
                        PendingEntityRef p;
                        p.owner    = e;
                        p.type     = b->typeId;
                        p.offset   = f->offset;
                        p.targetId = fieldVal.isNumber() ? fieldVal.asInt(-1) : -1;
                        p.where    = where + ": " + compName + "." + fieldName;
                        pending.push_back(std::move(p));
                        break;
                    }
                    case FieldBinding::Kind::String: {
                        auto* sp = reinterpret_cast<std::string*>(
                            static_cast<std::byte*>(comp) + f->offset);
                        *sp = fieldVal.asStringOr("");
                        break;
                    }
                }
            }
        }
    }

    // Проход 3: иерархия. setParent сам поддержит Children и отловит циклы.
    for (const auto& [child, parentId] : parentLinks) {
        const ecs::Entity parent = resolve(parentId);
        if (!parent.valid()) {
            res.warnings.push_back("dangling parent reference " + std::to_string(parentId) +
                                   " (entity left at root)");
            continue;
        }
        try {
            ecs::setParent(reg, child, parent, /*keepWorldPosition=*/false);
        } catch (const ecs::HierarchyCycleError& ex) {
            res.warnings.push_back(std::string("hierarchy cycle in scene file: ") + ex.what());
        }
    }

    // Проход 4: ссылки на сущности — уже после всех структурных изменений.
    for (const PendingEntityRef& p : pending) {
        void* comp = reg.getTypeId(p.owner, p.type);
        if (!comp) continue;
        const ecs::Entity target = resolve(p.targetId);
        if (p.targetId >= 0 && !target.valid())
            res.warnings.push_back(p.where + ": dangling entity reference " +
                                   std::to_string(p.targetId) + " (set to null)");
        writeRaw<ecs::Entity>(comp, p.offset, target);
    }

    // World-матрицы производны от Transform+Parent — пересчитываем, а не читаем.
    ecs::updateWorldTransforms(reg);

    res.ok = true;
    return res;
}

SceneLoadResult loadSceneFromFile(ecs::World& world, const std::string& path) {
    SceneLoadResult res;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        res.error = "cannot open scene file: " + path;
        return res;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadSceneFromString(world, ss.str());
}

} // namespace lv::scene
