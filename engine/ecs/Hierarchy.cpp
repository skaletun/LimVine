/**
 * @file    Hierarchy.cpp
 * @brief   Реализация иерархии сущностей и world-transform.
 */
#include "Hierarchy.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace lv::ecs {

void registerHierarchyComponents() {
    registerComponent<Parent>("Parent");
    registerComponent<Children>("Children");
    registerComponent<WorldTransform>("WorldTransform");
}

namespace {

/// Быстрое обнаружение цикла: идём вверх от newParent к корню; если
/// встретили child — назначение создаст цикл.
bool isAncestorOf(const Registry& reg, Entity ancestor, Entity e) {
    std::unordered_set<std::uint32_t> seen;
    Entity cur = e;
    while (cur.index != kNullEntity.index) {
        if (cur.index == ancestor.index && cur.generation == ancestor.generation) return true;
        if (!seen.insert(cur.index).second) return false;   // уже есть цикл
        const Parent* p = reg.get<Parent>(cur);
        if (!p) return false;
        cur = p->entity;
    }
    return false;
}

} // namespace

void setParent(Registry& reg, Entity child, Entity newParent, bool keepWorldPosition) {
    if (child.index == kNullEntity.index) return;
    if (!reg.alive(child)) return;
    if (newParent.index != kNullEntity.index && !reg.alive(newParent)) newParent = kNullEntity;

    // Отсоединить от старого родителя.
    if (Parent* oldParent = reg.get<Parent>(child)) {
        if (oldParent->entity.index != kNullEntity.index) {
            if (Children* ch = reg.get<Children>(oldParent->entity)) {
                auto& v = ch->entities;
                v.erase(std::remove(v.begin(), v.end(), child), v.end());
            }
        }
        if (newParent.index == kNullEntity.index) {
            reg.remove<Parent>(child);
        } else {
            oldParent->entity = newParent;
        }
    } else if (newParent.index != kNullEntity.index) {
        reg.add<Parent>(child, Parent{newParent});
    }

    if (newParent.index == kNullEntity.index) {
        // Снятие родителя уже произошло выше.
        reg.getOrEmplace<WorldTransform>(child)->dirty = true;
        return;
    }

    // Проверка цикла.
    if (newParent.index == child.index)
        throw HierarchyCycleError("entity cannot be its own parent");
    if (isAncestorOf(reg, child, newParent))
        throw HierarchyCycleError("setting this parent would create a cycle");

    // Добавить в список детей нового родителя.
    Children* c = reg.get<Children>(newParent);
    if (!c) c = reg.add<Children>(newParent);
    if (std::find(c->entities.begin(), c->entities.end(), child) == c->entities.end())
        c->entities.push_back(child);

    if (keepWorldPosition) {
        const Transform* parentT = reg.get<Transform>(newParent);
        Transform* childT = reg.get<Transform>(child);
        const WorldTransform* parentWT = reg.get<WorldTransform>(newParent);
        if (parentT && childT) {
            const Mat4 parentW = parentWT ? parentWT->matrix : parentT->matrix();
            const Mat4 childW  = childT->matrix();
            // childLocal = inv(parentW) * childW
            const Mat4 local = inverse(parentW) * childW;
            childT->position = extractTranslation(local);
            childT->rotation = extractRotation(local);
            childT->scale    = extractScale(local);
        }
    }

    reg.getOrEmplace<WorldTransform>(child)->dirty = true;
}

Mat4 computeWorldMatrix(const Registry& reg, Entity e) {
    if (const WorldTransform* wt = reg.get<WorldTransform>(e)) return wt->matrix;
    if (const Transform* t = reg.get<Transform>(e)) return t->matrix();
    return Mat4::identity();
}

std::size_t updateWorldTransforms(Registry& reg) {
    std::size_t touched = 0;
    std::deque<Entity> queue;

    // Шаг 0: СНАЧАЛА собираем список корней, и только потом добавляем им
    // WorldTransform.
    //
    // Добавление компонента — это структурное изменение: сущность переезжает в
    // другой архетип, а пулы текущего архетипа перевыделяются. Делать это прямо
    // внутри reg.each<Transform>() нельзя — итератор указывал бы в освобождённую
    // память (падение проявлялось на 780-м кадре шаблона farming_iso, когда
    // корутина роста культуры добавляла новые сущности и архетип рос).
    std::vector<Entity> roots;
    roots.reserve(64);
    reg.each<Transform>([&](Entity e, const Transform&) {
        if (!reg.has<Parent>(e)) roots.push_back(e);
        return true;
    });

    // Шаг 1: корни — все сущности с Transform, но без Parent.
    for (Entity e : roots) {
        const Transform* t = reg.get<Transform>(e);
        if (!t) continue;
        const Vec3 pos = t->position;
        const Quat rot = t->rotation;
        const Vec3 scl = t->scale;
        const Mat4 mtx = t->matrix();
        // getOrEmplace может переселить сущность в новый архетип, поэтому
        // значения Transform скопированы ВЫШЕ, до вызова.
        WorldTransform* wt = reg.getOrEmplace<WorldTransform>(e);
        if (!wt) continue;
        wt->worldPosition = pos;
        wt->worldRotation = rot;
        wt->worldScale    = scl;
        wt->matrix        = mtx;
        wt->version++;
        wt->dirty = false;
        queue.push_back(e);
        ++touched;
    }

    // Шаг 2: BFS вниз по Children.
    while (!queue.empty()) {
        Entity parent = queue.front(); queue.pop_front();
        const Children* ch = reg.get<Children>(parent);
        if (!ch) continue;
        const WorldTransform* parentWTp = reg.get<WorldTransform>(parent);
        if (!parentWTp) continue;
        // Копии родительских величин: getOrEmplace ниже может переселить
        // РОДИТЕЛЯ (если он тоже получает WorldTransform в этом же проходе),
        // и указатель parentWTp станет висячим.
        const Mat4 parentM   = parentWTp->matrix;
        const Quat parentRot = parentWTp->worldRotation;
        const Vec3 parentScl = parentWTp->worldScale;
        // Список детей тоже копируем: добавление компонента ребёнку меняет
        // архетип родителя не может, но сам вектор Children живёт в пуле,
        // который перевыделяется при переезде родителя.
        const std::vector<Entity> childList = ch->entities;
        for (Entity child : childList) {
            const Transform* ct = reg.get<Transform>(child);
            if (!ct) continue;
            const Mat4 localM  = ct->matrix();
            const Quat childRot = ct->rotation;
            const Vec3 childScl = ct->scale;
            WorldTransform* cwt = reg.getOrEmplace<WorldTransform>(child);
            if (!cwt) continue;
            cwt->matrix = parentM * localM;
            cwt->worldPosition = extractTranslation(cwt->matrix);
            cwt->worldRotation = parentRot * childRot;
            cwt->worldScale = Vec3{parentScl.x * childScl.x, parentScl.y * childScl.y,
                                   parentScl.z * childScl.z};
            cwt->version++;
            cwt->dirty = false;
            queue.push_back(child);
            ++touched;
        }
    }

    // Шаг 3: orphans (есть Parent, но родитель мёртв или не найден) — открепить.
    std::vector<Entity> orphans;
    reg.each<Parent>([&](Entity e, Parent& p) {
        if (!reg.alive(p.entity) || !reg.get<Transform>(p.entity)) orphans.push_back(e);
        return true;
    });
    for (Entity e : orphans) {
        reg.remove<Parent>(e);
        reg.getOrEmplace<WorldTransform>(e)->dirty = true;
    }
    return touched;
}

} // namespace lv::ecs
