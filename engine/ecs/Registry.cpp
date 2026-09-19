/**
 * @file    Registry.cpp
 * @brief   Реализация архетипного реестра ECS.
 */
#include "Registry.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace lv::ecs {

// ---------------------------------------------------------------------------
//  ComponentPool
// ---------------------------------------------------------------------------
void ComponentPool::reserveRows(std::size_t newRows) {
    if (newRows <= capacity) return;
    std::size_t cap = capacity == 0 ? 32 : capacity;
    while (cap < newRows) cap *= 2;

    void* fresh = nullptr;
    if (alignment > alignof(std::max_align_t)) {
        if (posix_memalign(&fresh, alignment, cap * elemSize) != 0) throw std::bad_alloc();
    } else {
        fresh = std::malloc(cap * elemSize);
        if (!fresh) throw std::bad_alloc();
    }

    // Переносим живые строки move-конструктором и разрушаем старые.
    if (data) {
        for (std::size_t i = 0; i < rows; ++i) {
            if (moveElem) moveElem(static_cast<std::byte*>(fresh) + i * elemSize,
                                   static_cast<std::byte*>(data) + i * elemSize);
            else std::memcpy(static_cast<std::byte*>(fresh) + i * elemSize,
                             static_cast<std::byte*>(data) + i * elemSize, elemSize);
            if (dtor) dtor(static_cast<std::byte*>(data) + i * elemSize);
        }
        std::free(data);
    }
    data = fresh;
    capacity = cap;
}

void ComponentPool::clear() {
    if (!data) return;
    for (std::size_t i = 0; i < rows; ++i)
        if (dtor) dtor(static_cast<std::byte*>(data) + i * elemSize);
    std::free(data);
    data = nullptr;
    rows = 0;
    capacity = 0;
}

// ---------------------------------------------------------------------------
//  ComponentMetaRegistry
// ---------------------------------------------------------------------------
ComponentMetaRegistry& ComponentMetaRegistry::instance() {
    static ComponentMetaRegistry inst;
    return inst;
}

// ---------------------------------------------------------------------------
//  Registry: жизненный цикл
// ---------------------------------------------------------------------------
Registry::Registry() {
    // Архетип 0 — «пустой»: в нём живут только что созданные сущности.
    auto empty = std::make_unique<Archetype>();
    empty->id = 0;
    archetypes_.push_back(std::move(empty));
    archetypeIndex_[Signature{}] = 0;
}

Registry::~Registry() { clear(); }

void Registry::clear() {
    for (auto& a : archetypes_) {
        for (ComponentPool& p : a->pools) p.clear();
        a->entities.clear();
    }
    records_.clear();
    freeList_.clear();
    aliveCount_ = 0;
    // Архетипы сохраняем (их сигнатуры можно переиспользовать), но чистим пулы.
}

std::uint32_t Registry::allocSlot() {
    if (!freeList_.empty()) {
        const std::uint32_t idx = freeList_.back();
        freeList_.pop_back();
        return idx;
    }
    records_.push_back(EntityRecord{});
    return static_cast<std::uint32_t>(records_.size() - 1);
}

void Registry::freeSlot(std::uint32_t index) {
    records_[index].alive = false;
    records_[index].archetype = kInvalidArchetype;
    ++records_[index].generation;   // инвалидирует старые handle'ы
    freeList_.push_back(index);
}

Entity Registry::create() {
    const std::uint32_t idx = allocSlot();
    EntityRecord& rec = records_[idx];
    rec.alive = true;
    rec.archetype = 0;   // пустой архетип
    rec.row = static_cast<std::uint32_t>(archetypes_[0]->entities.size());
    archetypes_[0]->entities.push_back(Entity{idx, rec.generation});
    ++aliveCount_;
    return Entity{idx, rec.generation};
}

bool Registry::alive(Entity e) const noexcept {
    if (e.index >= records_.size()) return false;
    const EntityRecord& rec = records_[e.index];
    return rec.alive && rec.generation == e.generation;
}

void Registry::destroy(Entity e) {
    if (!alive(e)) return;
    EntityRecord& rec = records_[e.index];
    Archetype& arch = *archetypes_[rec.archetype];

    // Разрушаем компоненты сущности.
    for (ComponentPool& p : arch.pools)
        if (p.dtor) p.dtor(p.rowPtr(rec.row));

    removeRow(arch, rec.row, *this);
    freeSlot(e.index);
    --aliveCount_;
}

void Registry::removeRow(Archetype& arch, std::size_t row, Registry& reg) {
    const std::size_t last = arch.entities.size() - 1;
    if (row != last) {
        // Swap-remove: переносим последнюю строку на место удалённой.
        const Entity moved = arch.entities[last];
        arch.entities[row] = moved;
        for (ComponentPool& p : arch.pools) {
            if (p.moveElem) {
                p.moveElem(p.rowPtr(row), p.rowPtr(last));
            } else {
                std::memcpy(p.rowPtr(row), p.rowPtr(last), p.elemSize);
            }
            if (p.dtor) p.dtor(p.rowPtr(last));
            p.rows = row + 1 > p.rows ? p.rows : p.rows; // rows пересчитаем ниже
        }
        if (moved.valid() && moved.index < reg.records_.size())
            reg.records_[moved.index].row = static_cast<std::uint32_t>(row);
    }
    arch.entities.pop_back();
    for (ComponentPool& p : arch.pools) p.rows = arch.entities.size();
}

// ---------------------------------------------------------------------------
//  Архетипы
// ---------------------------------------------------------------------------
ArchetypeId Registry::findOrCreateArchetype(const Signature& sig) {
    auto it = archetypeIndex_.find(sig);
    if (it != archetypeIndex_.end()) return it->second;

    auto arch = std::make_unique<Archetype>();
    arch->id = static_cast<ArchetypeId>(archetypes_.size());
    arch->signature = sig;
    arch->typeOrder = sig.toList();
    std::sort(arch->typeOrder.begin(), arch->typeOrder.end());

    for (TypeId t : arch->typeOrder) {
        const ComponentMeta* meta = ComponentMetaRegistry::instance().find(t);
        if (!meta) throw std::runtime_error("component TypeId=" + std::to_string(t) +
                                            " is not registered in ComponentMetaRegistry");
        arch->pools.emplace_back(t, meta->size, meta->alignment, meta->ctor, meta->dtor, meta->move);
    }
    const ArchetypeId id = arch->id;
    archetypes_.push_back(std::move(arch));
    archetypeIndex_[sig] = id;
    return id;
}

void Registry::moveEntityToArchetype(Entity e, ArchetypeId target) {
    EntityRecord& rec = records_[e.index];
    Archetype& src = *archetypes_[rec.archetype];
    Archetype& dst = *archetypes_[target];
    const std::size_t srcRow = rec.row;

    // 1) Резервируем строку в целевом архетипе и конструируем компоненты.
    dst.entities.push_back(e);
    const std::size_t dstRow = dst.entities.size() - 1;
    for (ComponentPool& p : dst.pools) {
        p.reserveRows(dst.entities.size());
        void* cell = p.rowPtr(dstRow);
        ComponentPool* srcPool = src.pool(p.typeId);
        if (srcPool) {
            if (p.moveElem) p.moveElem(cell, srcPool->rowPtr(srcRow));
            else std::memcpy(cell, srcPool->rowPtr(srcRow), p.elemSize);
        } else if (p.ctor) {
            p.ctor(cell);
        }
        p.rows = dst.entities.size();
    }

    // 2) Разрушаем компоненты, которых нет в целевом архетипе.
    for (ComponentPool& p : src.pools) {
        if (dst.signature.test(p.typeId)) continue;   // уже перемещены
        if (p.dtor) p.dtor(p.rowPtr(srcRow));
    }

    // 3) Удаляем строку из исходного архетипа (swap-remove).
    removeRow(src, srcRow, *this);

    // 4) Обновляем запись сущности.
    rec.archetype = target;
    rec.row = static_cast<std::uint32_t>(dstRow);
    ++migrations_;
}

// ---------------------------------------------------------------------------
//  Динамический доступ
// ---------------------------------------------------------------------------
bool Registry::hasTypeId(Entity e, TypeId t) const noexcept {
    if (!alive(e)) return false;
    return archetypes_[records_[e.index].archetype]->signature.test(t);
}

void* Registry::getTypeId(Entity e, TypeId t) noexcept {
    if (!alive(e)) return nullptr;
    EntityRecord& rec = records_[e.index];
    ComponentPool* p = archetypes_[rec.archetype]->pool(t);
    return p ? p->rowPtr(rec.row) : nullptr;
}

const ComponentMeta* Registry::metaOf(TypeId t) const noexcept {
    return ComponentMetaRegistry::instance().find(t);
}

void Registry::addDynamic(Entity e, TypeId t) {
    if (!alive(e)) return;
    EntityRecord& rec = records_[e.index];
    Archetype& src = *archetypes_[rec.archetype];
    if (src.signature.test(t)) return;
    Signature sig = src.signature;
    sig.set(t);
    moveEntityToArchetype(e, findOrCreateArchetype(sig));
}

void Registry::removeDynamic(Entity e, TypeId t) {
    if (!alive(e)) return;
    EntityRecord& rec = records_[e.index];
    Archetype& src = *archetypes_[rec.archetype];
    if (!src.signature.test(t)) return;
    Signature sig = src.signature;
    sig.clear(t);
    moveEntityToArchetype(e, findOrCreateArchetype(sig));
}

bool Registry::hasAll(Entity e, std::span<const TypeId> ids) const noexcept {
    if (!alive(e)) return false;
    const Signature& sig = archetypes_[records_[e.index].archetype]->signature;
    for (TypeId id : ids) if (!sig.test(id)) return false;
    return true;
}

std::vector<Entity> Registry::allEntities() const {
    std::vector<Entity> out;
    out.reserve(aliveCount_);
    for (const auto& archPtr : archetypes_)
        for (Entity e : archPtr->entities) out.push_back(e);
    return out;
}

Registry::Stats Registry::stats() const noexcept {
    Stats s{};
    s.entities = aliveCount_;
    s.archetypes = archetypes_.size();
    s.migrations = migrations_;
    for (const auto& a : archetypes_) {
        s.bytes += a->entities.capacity() * sizeof(Entity);
        for (const ComponentPool& p : a->pools) s.bytes += p.capacity * p.elemSize;
    }
    return s;
}

} // namespace lv::ecs
