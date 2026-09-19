/**
 * @file    CommandBuffer.h
 * @brief   Отложенные структурные изменения ECS.
 * @ingroup ECS
 *
 * @details Системы и LV Script не имеют права менять состав архетипов во время
 *          итерации: миграция сущности инвалидирует указатели на компоненты.
 *          Вместо этого они пишут команды в @c CommandBuffer, который
 *          применяется между фазами (@c World::flushCommands).
 *
 *          Команды хранятся в типизированных очередях, а не в type-erased
 *          std::function: apply() идёт плотными циклами без косвенных вызовов,
 *          что важно при тысячах spawn'ов за кадр (волна противников, выстрелы).
 */
#pragma once

#include "Registry.h"

#include <functional>
#include <vector>

namespace lv::ecs {

/**
 * @brief Очередь отложенных структурных изменений.
 *
 * Потокобезопасность: один буфер на поток (Job System выдаёт каждому worker'у
 * свой), затем все буферы сливаются в детерминированном порядке.
 */
class CommandBuffer {
public:
    explicit CommandBuffer(Registry& reg) : reg_(reg) {}

    /// Отложенное создание. Возвращает «зарезервированный» handle: он станет
    /// валидным после flush(), но его можно использовать в командах сразу.
    [[nodiscard]] Entity create() {
        const std::uint32_t slot = static_cast<std::uint32_t>(pendingCreates_.size());
        pendingCreates_.push_back(kNullEntity);
        // Временный handle с поколением 0xFFFFFFF0+ — гарантированно не совпадёт
        // с реальными, и после flush заменяется на настоящий.
        return Entity{slot, 0xFFFFFFF0u};
    }

    void destroy(Entity e) { destroys_.push_back(e); }

    /// Добавить компонент (значение копируется в команду).
    ///
    /// Команда хранит типизированную копию, а не type-erased функтор:
    /// flush() конструирует компонент в пуле архетипа и затем выполняет
    /// присваивание через @c ComponentMeta::assign.
    template <Component T>
    void add(Entity e, const T& value) {
        ComponentMetaRegistry::instance().reg<T>();
        adds_.push_back(AddCmd{e, typeIdOf<T>(),
                               [value]() -> void* { return new T(value); },
                               [](void* p) { delete static_cast<T*>(p); }});
    }

    template <Component T>
    void remove(Entity e) { removes_.push_back({e, typeIdOf<T>()}); }

    /// Произвольная отложенная операция (например, вызов LV Script-колбэка).
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

    /// Применить все команды и очистить буфер.
    void flush();

    [[nodiscard]] std::size_t pendingCount() const noexcept {
        return pendingCreates_.size() + destroys_.size() + adds_.size() + removes_.size() + deferred_.size();
    }

    /// Заменить временный handle на настоящий (используется скриптами после flush).
    [[nodiscard]] Entity resolve(Entity placeholder) const {
        if (placeholder.generation == 0xFFFFFFF0u && placeholder.index < resolved_.size())
            return resolved_[placeholder.index];
        return placeholder;
    }

private:
    struct AddCmd {
        Entity entity;
        TypeId type;
        std::function<void*()> clone;
        std::function<void(void*)> dispose;
    };
    struct RemoveCmd { Entity entity; TypeId type; };

    Registry& reg_;
    std::vector<Entity>    pendingCreates_;
    std::vector<Entity>    resolved_;      ///< placeholder.index -> реальная сущность
    std::vector<Entity>    destroys_;
    std::vector<AddCmd>    adds_;
    std::vector<RemoveCmd> removes_;
    std::vector<std::function<void()>> deferred_;
};

inline void CommandBuffer::flush() {
    // Порядок важен: сначала создаём, затем добавляем компоненты, потом удаления.
    for (std::size_t i = 0; i < pendingCreates_.size(); ++i) {
        const Entity real = reg_.create();
        if (i >= resolved_.size()) resolved_.resize(i + 1, kNullEntity);
        resolved_[i] = real;
    }
    pendingCreates_.clear();

    auto fixup = [this](Entity e) { return resolve(e); };

    for (AddCmd& cmd : adds_) {
        const Entity e = fixup(cmd.entity);
        if (!reg_.alive(e)) continue;
        // Компонент уже существует — просто присваиваем новое значение.
        if (reg_.hasTypeId(e, cmd.type)) {
            void* dst = reg_.getTypeId(e, cmd.type);
            const ComponentMeta* meta = reg_.metaOf(cmd.type);
            void* boxed = cmd.clone();
            if (dst && meta && meta->assign) meta->assign(dst, boxed);
            if (cmd.dispose) cmd.dispose(boxed);
            continue;
        }
        reg_.addDynamic(e, cmd.type);           // конструирует значение по умолчанию
        void* dst = reg_.getTypeId(e, cmd.type);
        const ComponentMeta* meta = reg_.metaOf(cmd.type);
        void* boxed = cmd.clone();
        if (dst && meta && meta->assign) meta->assign(dst, boxed);
        if (cmd.dispose) cmd.dispose(boxed);
    }
    adds_.clear();

    for (const RemoveCmd& cmd : removes_) {
        const Entity e = fixup(cmd.entity);
        if (reg_.alive(e)) reg_.removeDynamic(e, cmd.type);
    }
    removes_.clear();

    for (Entity e : destroys_) reg_.destroy(fixup(e));
    destroys_.clear();

    for (auto& fn : deferred_) fn();
    deferred_.clear();
}

} // namespace lv::ecs
