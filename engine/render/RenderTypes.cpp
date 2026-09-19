/**
 * @file    RenderTypes.cpp
 * @brief   Извлечение frustum'а и проверка пересечений.
 */
#include "RenderTypes.h"

namespace lv::render {

Frustum Frustum::fromViewProjection(const Mat4& vp) noexcept {
    Frustum f;
    // Строки матрицы (column-major => строка i это m[i], m[4+i], m[8+i], m[12+i]).
    auto row = [&](int i, int c) { return vp.m[c * 4 + i]; };
    const Vec3 rows[4] = {
        {row(0, 0), row(0, 1), row(0, 2)},
        {row(1, 0), row(1, 1), row(1, 2)},
        {row(2, 0), row(2, 1), row(2, 2)},
        {row(3, 0), row(3, 1), row(3, 2)}
    };
    const Real offs[4] = {row(0, 3), row(1, 3), row(2, 3), row(3, 3)};

    const Vec3 n[6] = {
        rows[3] + rows[0], rows[3] - rows[0],
        rows[3] + rows[1], rows[3] - rows[1],
        rows[3] + rows[2], rows[3] - rows[2]
    };
    const Real d[6] = {
        offs[3] + offs[0], offs[3] - offs[0],
        offs[3] + offs[1], offs[3] - offs[1],
        offs[3] + offs[2], offs[3] - offs[2]
    };
    for (int i = 0; i < 6; ++i) {
        const Real len = std::max(kEpsilon, length(n[i]));
        f.planes[i].normal = n[i] * (1.0f / len);
        f.planes[i].offset = d[i] / len;
    }
    return f;
}

bool Frustum::intersects(const AABB& box) const noexcept {
    // Тест «положительной вершины»: для каждой плоскости берём угол AABB,
    // наиболее удалённый по направлению нормали.
    const Vec3 c = box.center();
    const Vec3 e = box.extents();
    for (const Plane& p : planes) {
        const Real r = e.x * std::fabs(p.normal.x) + e.y * std::fabs(p.normal.y) + e.z * std::fabs(p.normal.z);
        if (p.distanceTo(c) + r < 0) return false;
    }
    return true;
}

bool Frustum::contains(const Vec3& pt) const noexcept {
    for (const Plane& p : planes) if (p.distanceTo(pt) < 0) return false;
    return true;
}

} // namespace lv::render
