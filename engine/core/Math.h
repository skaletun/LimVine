/**
 * @file    Math.h
 * @brief   Математическое ядро LimVine: SIMD-дружелюбные vec/mat/quat.
 * @ingroup Core
 *
 * @details Принципы:
 *          - все типы тривиально копируемы и выровнены по 16 байт => их можно
 *            хранить в ECS-пулах сплошным массивом и грузить в SIMD-регистры;
 *          - левосторонняя система координат не используется: Y вверх, метры,
 *            кватернионы в порядке (x, y, z, w), матрицы column-major как в GL/Vulkan;
 *          - никакой виртуальности и аллокаций: только inline-функции.
 *
 * @note Полный SIMD-путь (SSE/NEON) включается макросом @c LV_USE_SIMD;
 *       скалярная реализация сохраняется как reference и для отладки.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <concepts>
#include <optional>
#include <limits>
#include <utility>

namespace lv {

/// Концепция «похоже на число» — защищает API от случайных приведений.
template <class T>
concept Numeric = std::is_arithmetic_v<T>;

/// Вещественный тип по умолчанию. Двиок собирается в float-точности:
/// для low-poly сцен этого достаточно, а кэш-пропускная способность вдвое лучше.
using Real = float;

constexpr Real kPi      = 3.14159265358979323846f;
constexpr Real kTwoPi   = 6.28318530717958647692f;
constexpr Real kHalfPi  = 1.57078632679489661923f;
constexpr Real kEpsilon = 1e-6f;
constexpr Real kDeg2Rad = kPi / 180.0f;
constexpr Real kRad2Deg = 180.0f / kPi;

[[nodiscard]] constexpr Real radians(Real deg) noexcept { return deg * kDeg2Rad; }
[[nodiscard]] constexpr Real degrees(Real rad) noexcept { return rad * kRad2Deg; }
[[nodiscard]] constexpr Real clampReal(Real v, Real lo, Real hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
[[nodiscard]] inline Real lerp(Real a, Real b, Real t) noexcept { return a + (b - a) * t; }

/// Кадронезависимое сглаживание: `damp(a, b, lambda, dt)`.
[[nodiscard]] inline Real damp(Real a, Real b, Real lambda, Real dt) noexcept {
    return lerp(a, b, 1.0f - std::exp(-lambda * dt));
}

// ---------------------------------------------------------------------------
//  Vec2 / Vec3 / Vec4
// ---------------------------------------------------------------------------

struct alignas(8) Vec2 {
    Real x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(Real xx, Real yy) noexcept : x(xx), y(yy) {}
    explicit constexpr Vec2(Real s) noexcept : x(s), y(s) {}
};

/**
 * @brief Трёхмерный вектор.
 *
 * Выровнен по 16 байт и дополнен четвёртым компонентом, чтобы один Vec3
 * занимал ровно одну SSE-строку: массивы Vec3 в ECS-пулах не «рвут» кэш-линии.
 */
struct alignas(16) Vec3 {
    Real x = 0, y = 0, z = 0, w = 0;

    constexpr Vec3() = default;
    constexpr Vec3(Real xx, Real yy, Real zz) noexcept : x(xx), y(yy), z(zz), w(0) {}
    explicit constexpr Vec3(Real s) noexcept : x(s), y(s), z(s), w(0) {}

    constexpr Vec3& operator+=(const Vec3& o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(Real s) noexcept { x *= s; y *= s; z *= s; return *this; }

    [[nodiscard]] static constexpr Vec3 zero()  noexcept { return {0, 0, 0}; }
    [[nodiscard]] static constexpr Vec3 one()   noexcept { return {1, 1, 1}; }
    [[nodiscard]] static constexpr Vec3 up()    noexcept { return {0, 1, 0}; }
    [[nodiscard]] static constexpr Vec3 right() noexcept { return {1, 0, 0}; }
    [[nodiscard]] static constexpr Vec3 forward() noexcept { return {0, 0, -1}; }
};

struct alignas(16) Vec4 {
    Real x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(Real xx, Real yy, Real zz, Real ww) noexcept : x(xx), y(yy), z(zz), w(ww) {}
    constexpr Vec4(const Vec3& v, Real ww) noexcept : x(v.x), y(v.y), z(v.z), w(ww) {}
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, const Vec3& b) noexcept { a += b; return a; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, const Vec3& b) noexcept { a -= b; return a; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, Real s) noexcept { a *= s; return a; }
[[nodiscard]] constexpr Vec3 operator*(Real s, Vec3 a) noexcept { a *= s; return a; }
[[nodiscard]] constexpr Vec3 operator-(const Vec3& a) noexcept { return {-a.x, -a.y, -a.z}; }
[[nodiscard]] constexpr Vec3 operator*(const Vec3& a, const Vec3& b) noexcept { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
[[nodiscard]] constexpr bool operator==(const Vec3& a, const Vec3& b) noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }

[[nodiscard]] constexpr Real dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] constexpr Real lengthSq(const Vec3& a) noexcept { return dot(a, a); }
[[nodiscard]] inline Real length(const Vec3& a) noexcept { return std::sqrt(lengthSq(a)); }
[[nodiscard]] inline Real distance(const Vec3& a, const Vec3& b) noexcept { return length(a - b); }

[[nodiscard]] inline Vec3 normalize(const Vec3& a) noexcept {
    const Real len = length(a);
    return len > kEpsilon ? a * (1.0f / len) : Vec3::zero();
}

/// Расстояние без корня — для сравнений «кто ближе».
[[nodiscard]] constexpr Real distanceSq(const Vec3& a, const Vec3& b) noexcept { return lengthSq(a - b); }

/// Покомпонентный min/max/abs — используются в AABB и frustum culling.
[[nodiscard]] constexpr Vec3 min(const Vec3& a, const Vec3& b) noexcept {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
[[nodiscard]] constexpr Vec3 max(const Vec3& a, const Vec3& b) noexcept {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
[[nodiscard]] constexpr Vec3 abs(const Vec3& a) noexcept {
    return {a.x < 0 ? -a.x : a.x, a.y < 0 ? -a.y : a.y, a.z < 0 ? -a.z : a.z};
}

// ---------------------------------------------------------------------------
//  Quat
// ---------------------------------------------------------------------------

/**
 * @brief Кватернион (x, y, z, w), хранение — как у Vec4 для SIMD.
 *
 * Кватернионы вместо углов Эйлера выбраны потому, что:
 *  - нет gimbal lock (критично для third-person и flight-камер);
 *  - slerp даёт равномерное вращение «из коробки» для анимаций и турелей;
 *  - композиция поворотов дешевле матричной.
 */
struct alignas(16) Quat {
    Real x = 0, y = 0, z = 0, w = 1;

    constexpr Quat() = default;
    constexpr Quat(Real xx, Real yy, Real zz, Real ww) noexcept : x(xx), y(yy), z(zz), w(ww) {}

    [[nodiscard]] static constexpr Quat identity() noexcept { return {0, 0, 0, 1}; }

    /// Поворот вокруг произвольной оси (ось должна быть нормирована).
    [[nodiscard]] static inline Quat fromAxisAngle(const Vec3& axis, Real angleRad) noexcept {
        const Real h = angleRad * 0.5f;
        const Real s = std::sin(h);
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
    }
    /// Из углов Эйлера (yaw вокруг Y, pitch вокруг X, roll вокруг Z), порядок YXZ.
    [[nodiscard]] static inline Quat fromEuler(Real pitch, Real yaw, Real roll) noexcept {
        const Real cy = std::cos(yaw * 0.5f),   sy = std::sin(yaw * 0.5f);
        const Real cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
        const Real cr = std::cos(roll * 0.5f),  sr = std::sin(roll * 0.5f);
        return {
            cy * sp * cr + sy * cp * sr,
            sy * cp * cr - cy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr
        };
    }
    /// «Смотреть на» цель: базис строится из направления и мирового up.
    [[nodiscard]] static inline Quat lookRotation(const Vec3& forward, const Vec3& up = Vec3::up()) noexcept;
};

[[nodiscard]] inline Real dot(const Quat& a, const Quat& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] inline Quat normalize(const Quat& q) noexcept {
    const Real len = std::sqrt(dot(q, q));
    if (len <= kEpsilon) return Quat::identity();
    const Real inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

[[nodiscard]] inline Quat conjugate(const Quat& q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }

[[nodiscard]] inline Quat operator*(const Quat& a, const Quat& b) noexcept {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

/// Поворот вектора кватернионом: q * v * q⁻¹ (в развёрнутой форме — 18 FLOP).
[[nodiscard]] inline Vec3 rotate(const Quat& q, const Vec3& v) noexcept {
    const Vec3 t = 2.0f * cross({q.x, q.y, q.z}, v);
    return v + q.w * t + cross({q.x, q.y, q.z}, t);
}

/// Сферическая интерполяция с защитой от «длинного пути» и вырожденного случая.
[[nodiscard]] inline Quat slerp(Quat a, Quat b, Real t) noexcept {
    Real d = dot(a, b);
    if (d < 0.0f) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    const Real theta = std::acos(clampReal(d, -1.0f, 1.0f));
    const Real sinTheta = std::sin(theta);
    const Real wa = std::sin((1.0f - t) * theta) / sinTheta;
    const Real wb = std::sin(t * theta) / sinTheta;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

inline Quat Quat::lookRotation(const Vec3& fwd, const Vec3& up) noexcept {
    const Vec3 f = normalize(fwd);
    const Vec3 r = normalize(cross(up, f));
    const Vec3 u = cross(f, r);
    // Матрица -> кватернион (ветвление по максимальному диагональному элементу).
    const Real trace = r.x + u.y + f.z;
    if (trace > 0.0f) {
        const Real s = std::sqrt(trace + 1.0f) * 2.0f;
        return normalize({(u.z - f.y) / s, (f.x - r.z) / s, (r.y - u.x) / s, 0.25f * s});
    }
    if (r.x > u.y && r.x > f.z) {
        const Real s = std::sqrt(1.0f + r.x - u.y - f.z) * 2.0f;
        return normalize({0.25f * s, (r.y + u.x) / s, (f.x + r.z) / s, (u.z - f.y) / s});
    }
    if (u.y > f.z) {
        const Real s = std::sqrt(1.0f + u.y - r.x - f.z) * 2.0f;
        return normalize({(r.y + u.x) / s, 0.25f * s, (u.z + f.y) / s, (f.x - r.z) / s});
    }
    const Real s = std::sqrt(1.0f + f.z - r.x - u.y) * 2.0f;
    return normalize({(f.x + r.z) / s, (u.z + f.y) / s, 0.25f * s, (r.y - u.x) / s});
}

// ---------------------------------------------------------------------------
//  Mat4 (column-major, как в OpenGL/Vulkan)
// ---------------------------------------------------------------------------

/// Матрица 4x4 в column-major порядке: @c m[col][row].
struct alignas(16) Mat4 {
    Real m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    [[nodiscard]] static constexpr Mat4 identity() noexcept { return Mat4{}; }

    [[nodiscard]] static inline Mat4 translation(const Vec3& t) noexcept {
        Mat4 r; r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r;
    }
    [[nodiscard]] static inline Mat4 scale(const Vec3& s) noexcept {
        Mat4 r; r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z; return r;
    }
    [[nodiscard]] static Mat4 rotation(const Quat& q) noexcept;
    [[nodiscard]] static Mat4 trs(const Vec3& t, const Quat& r, const Vec3& s) noexcept;

    /// Перспективная проекция (правосторонняя, depth [0,1] как в Vulkan).
    [[nodiscard]] static Mat4 perspective(Real fovYRad, Real aspect, Real zNear, Real zFar) noexcept;
    /// Ортографическая проекция — основной режим изометрического шаблона.
    [[nodiscard]] static Mat4 ortho(Real l, Real r, Real b, Real t, Real n, Real f) noexcept;
    /// Матрица вида «смотрит на».
    [[nodiscard]] static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept;
};

[[nodiscard]] Mat4 operator*(const Mat4& a, const Mat4& b) noexcept;
[[nodiscard]] Vec4 operator*(const Mat4& m, const Vec4& v) noexcept;

/**
 * @brief Матрица 3x3 в column-major порядке: элемент (строка r, столбец c) лежит
 *        в @c m[c * 3 + r] — та же раскладка, что у Mat4.
 */
struct alignas(16) Mat3x3 {
    Real m[9] = {1,0,0,0,1,0,0,0,1};
    /// Доступ по (строка, столбец) — избавляет вызывающий код от арифметики индексов.
    [[nodiscard]] constexpr Real at(int row, int col) const noexcept { return m[col * 3 + row]; }
    constexpr void set(int row, int col, Real v) noexcept { m[col * 3 + row] = v; }
};

/**
 * @brief Кватернион из 3x3 матрицы вращения (метод Shepperd: ветвление по
 *        наибольшему диагональному элементу — численно устойчиво во всех случаях).
 *
 * @param m Ортонормированная матрица вращения (масштаб должен быть снят заранее).
 */
[[nodiscard]] inline Quat quatFromMat3(const Mat3x3& m) noexcept {
    const Real r00 = m.at(0,0), r01 = m.at(0,1), r02 = m.at(0,2);
    const Real r10 = m.at(1,0), r11 = m.at(1,1), r12 = m.at(1,2);
    const Real r20 = m.at(2,0), r21 = m.at(2,1), r22 = m.at(2,2);
    const Real trace = r00 + r11 + r22;
    Quat q;
    if (trace > 0.0f) {
        const Real s = std::sqrt(trace + 1.0f) * 2.0f;   // s = 4w
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const Real s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;   // s = 4x
        q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const Real s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;   // s = 4y
        q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const Real s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;   // s = 4z
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    return normalize(q);
}

/// Обратная матрица (для вычисления локальных координат из world-).
[[nodiscard]] inline Mat4 inverse(const Mat4& m) noexcept {
    const Real* o = m.m;
    Real inv[16];
    inv[0]  =  o[5]*o[10]*o[15] - o[5]*o[11]*o[14] - o[9]*o[6]*o[15] + o[9]*o[7]*o[14] + o[13]*o[6]*o[11] - o[13]*o[7]*o[10];
    inv[4]  = -o[4]*o[10]*o[15] + o[4]*o[11]*o[14] + o[8]*o[6]*o[15] - o[8]*o[7]*o[14] - o[12]*o[6]*o[11] + o[12]*o[7]*o[10];
    inv[8]  =  o[4]*o[9]*o[15]  - o[4]*o[11]*o[13] - o[8]*o[5]*o[15] + o[8]*o[7]*o[13] + o[12]*o[5]*o[11] - o[12]*o[7]*o[9];
    inv[12] = -o[4]*o[9]*o[14]  + o[4]*o[10]*o[13] + o[8]*o[5]*o[14] - o[8]*o[6]*o[13] - o[12]*o[5]*o[10] + o[12]*o[6]*o[9];
    inv[1]  = -o[1]*o[10]*o[15] + o[1]*o[11]*o[14] + o[9]*o[2]*o[15] - o[9]*o[3]*o[14] - o[13]*o[2]*o[11] + o[13]*o[3]*o[10];
    inv[5]  =  o[0]*o[10]*o[15] - o[0]*o[11]*o[14] - o[8]*o[2]*o[15] + o[8]*o[3]*o[14] + o[12]*o[2]*o[11] - o[12]*o[3]*o[10];
    inv[9]  = -o[0]*o[9]*o[15]  + o[0]*o[11]*o[13] + o[8]*o[1]*o[15] - o[8]*o[3]*o[13] - o[12]*o[1]*o[11] + o[12]*o[3]*o[9];
    inv[13] =  o[0]*o[9]*o[14]  - o[0]*o[10]*o[13] - o[8]*o[1]*o[14] + o[8]*o[2]*o[13] + o[12]*o[1]*o[10] - o[12]*o[2]*o[9];
    inv[2]  =  o[1]*o[6]*o[15]  - o[1]*o[7]*o[14]  - o[5]*o[2]*o[15] + o[5]*o[3]*o[14] + o[13]*o[2]*o[7]  - o[13]*o[3]*o[6];
    inv[6]  = -o[0]*o[6]*o[15]  + o[0]*o[7]*o[14]  + o[4]*o[2]*o[15] - o[4]*o[3]*o[14] - o[12]*o[2]*o[7]  + o[12]*o[3]*o[6];
    inv[10] =  o[0]*o[5]*o[15]  - o[0]*o[7]*o[13]  - o[4]*o[1]*o[15] + o[4]*o[3]*o[13] + o[12]*o[1]*o[7]  - o[12]*o[3]*o[5];
    inv[14] = -o[0]*o[5]*o[14]  + o[0]*o[6]*o[13]  + o[4]*o[1]*o[14] - o[4]*o[2]*o[13] - o[12]*o[1]*o[6]  + o[12]*o[2]*o[5];
    inv[3]  = -o[1]*o[6]*o[11]  + o[1]*o[7]*o[10]  + o[5]*o[2]*o[11] - o[5]*o[3]*o[10] - o[9]*o[2]*o[7]   + o[9]*o[3]*o[6];
    inv[7]  =  o[0]*o[6]*o[11]  - o[0]*o[7]*o[10]  - o[4]*o[2]*o[11] + o[4]*o[3]*o[10] + o[8]*o[2]*o[7]   - o[8]*o[3]*o[6];
    inv[11] = -o[0]*o[5]*o[11]  + o[0]*o[7]*o[9]   + o[4]*o[1]*o[11] - o[4]*o[3]*o[9]  - o[8]*o[1]*o[7]   + o[8]*o[3]*o[5];
    inv[15] =  o[0]*o[5]*o[10]  - o[0]*o[6]*o[9]   - o[4]*o[1]*o[10] + o[4]*o[2]*o[9]  + o[8]*o[1]*o[6]   - o[8]*o[2]*o[5];
    Real det = o[0]*inv[0] + o[1]*inv[4] + o[2]*inv[8] + o[3]*inv[12];
    if (std::fabs(det) < kEpsilon) return Mat4::identity();
    det = 1.0f / det;
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
    return r;
}

/// Извлечение смещения из column-major матрицы TRS.
[[nodiscard]] inline Vec3 extractTranslation(const Mat4& m) noexcept { return {m.m[12], m.m[13], m.m[14]}; }

/// Извлечение масштаба (предполагается равномерный или ортогональный базис).
[[nodiscard]] inline Vec3 extractScale(const Mat4& m) noexcept {
    auto col = [&](int c) { return Vec3{m.m[c*4], m.m[c*4+1], m.m[c*4+2]}; };
    return {length(col(0)), length(col(1)), length(col(2))};
}

/**
 * @brief Извлечение вращения из TRS-матрицы.
 *
 * Каждый столбец базиса делится на СВОЙ масштаб (столбец c соответствует
 * масштабу по оси c — именно так его закладывает @c Mat4::trs), после чего
 * ортонормированный базис переводится в кватернион.
 */
[[nodiscard]] inline Quat extractRotation(const Mat4& m) noexcept {
    Vec3 s = extractScale(m);
    const Real inv[3] = {
        s.x > kEpsilon ? 1.0f / s.x : 1.0f,
        s.y > kEpsilon ? 1.0f / s.y : 1.0f,
        s.z > kEpsilon ? 1.0f / s.z : 1.0f
    };
    Mat3x3 m3;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m3.m[c * 3 + r] = m.m[c * 4 + r] * inv[c];
    return quatFromMat3(m3);
}

inline Mat4 Mat4::rotation(const Quat& q) noexcept {
    const Real xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const Real xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const Real wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 r;
    r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + wz);       r.m[2] = 2 * (xz - wy);
    r.m[4] = 2 * (xy - wz);     r.m[5] = 1 - 2 * (xx + zz);   r.m[6] = 2 * (yz + wx);
    r.m[8] = 2 * (xz + wy);     r.m[9] = 2 * (yz - wx);       r.m[10] = 1 - 2 * (xx + yy);
    return r;
}

inline Mat4 Mat4::trs(const Vec3& t, const Quat& r, const Vec3& s) noexcept {
    Mat4 out = rotation(r);
    for (int i = 0; i < 3; ++i) { out.m[i] *= s.x; out.m[4 + i] *= s.y; out.m[8 + i] *= s.z; }
    out.m[12] = t.x; out.m[13] = t.y; out.m[14] = t.z;
    return out;
}

inline Mat4 Mat4::perspective(const Real fovY, const Real aspect, const Real n, const Real f) noexcept {
    const Real t = std::tan(fovY * 0.5f);
    Mat4 r{};
    for (int i = 0; i < 16; ++i) r.m[i] = 0;
    r.m[0] = 1.0f / (aspect * t);
    r.m[5] = 1.0f / t;
    r.m[10] = f / (n - f);
    r.m[11] = -1.0f;
    r.m[14] = (n * f) / (n - f);
    return r;
}

inline Mat4 Mat4::ortho(const Real l, const Real rr, const Real b, const Real t, const Real n, const Real f) noexcept {
    Mat4 r{};
    for (int i = 0; i < 16; ++i) r.m[i] = 0;
    r.m[0] = 2.0f / (rr - l);
    r.m[5] = 2.0f / (t - b);
    r.m[10] = 1.0f / (n - f);
    r.m[12] = (rr + l) / (l - rr);
    r.m[13] = (t + b) / (b - t);
    r.m[14] = n / (n - f);
    r.m[15] = 1.0f;
    return r;
}

inline Mat4 Mat4::lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept {
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r{};
    r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z; r.m[12] = -dot(s, eye);
    r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z; r.m[13] = -dot(u, eye);
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = dot(f, eye);
    r.m[3] = 0; r.m[7] = 0; r.m[11] = 0; r.m[15] = 1;
    return r;
}

inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            Real sum = 0;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}

inline Vec4 operator*(const Mat4& m, const Vec4& v) noexcept {
    return {
        m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
        m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w
    };
}

// ---------------------------------------------------------------------------
//  Геометрия
// ---------------------------------------------------------------------------

/// Выровненная по осям ограничивающая коробка.
struct AABB {
    Vec3 minPt{0, 0, 0};
    Vec3 maxPt{0, 0, 0};

    [[nodiscard]] constexpr Vec3 center() const noexcept { return (minPt + maxPt) * 0.5f; }
    [[nodiscard]] constexpr Vec3 extents() const noexcept { return (maxPt - minPt) * 0.5f; }
    constexpr void expand(const Vec3& p) noexcept { minPt = min(minPt, p); maxPt = max(maxPt, p); }
    constexpr void expand(const AABB& o) noexcept { expand(o.minPt); expand(o.maxPt); }
    [[nodiscard]] constexpr bool intersects(const AABB& o) const noexcept {
        return minPt.x <= o.maxPt.x && maxPt.x >= o.minPt.x &&
               minPt.y <= o.maxPt.y && maxPt.y >= o.minPt.y &&
               minPt.z <= o.maxPt.z && maxPt.z >= o.minPt.z;
    }
    [[nodiscard]] constexpr bool contains(const Vec3& p) const noexcept {
        return p.x >= minPt.x && p.x <= maxPt.x && p.y >= minPt.y && p.y <= maxPt.y &&
               p.z >= minPt.z && p.z <= maxPt.z;
    }
};

/// Луч для raycast'ов (hitscan-оружие, выбор объекта в редакторе).
struct Ray {
    Vec3 origin;
    Vec3 direction{0, 0, -1};
    [[nodiscard]] constexpr Vec3 at(Real t) const noexcept { return origin + direction * t; }
};

/// Цвет в линейном пространстве, 8 бит на канал при сериализации.
struct alignas(16) Color {
    Real r = 1, g = 1, b = 1, a = 1;
    constexpr Color() = default;
    constexpr Color(Real rr, Real gg, Real bb, Real aa = 1.0f) noexcept : r(rr), g(gg), b(bb), a(aa) {}
    [[nodiscard]] constexpr std::uint32_t toRGBA8() const noexcept {
        auto q = [](Real v) { return static_cast<std::uint32_t>(clampReal(v, 0.f, 1.f) * 255.0f + 0.5f); };
        return (q(r) << 24) | (q(g) << 16) | (q(b) << 8) | q(a);
    }
};

/// Преобразование экземпляра (позиция/поворот/масштаб) объявлено как ECS-компонент
/// @c lv::ecs::Transform — единый канонический тип движка. Дублировать его в
/// @c lv:: нельзя: неоднозначность имён ломает каждый `using namespace`.
/// Вспомогательные функции живут свободно:
struct TransformOps {
    [[nodiscard]] static Mat4 matrix(const Vec3& p, const Quat& r, const Vec3& s) noexcept {
        return Mat4::trs(p, r, s);
    }
};

/// Пересечение луча и AABB (slab method). Возвращает дистанцию или nullopt.
[[nodiscard]] inline std::optional<Real> intersectRayAABB(const Ray& r, const AABB& box) noexcept {
    Real tmin = 0.0f, tmax = std::numeric_limits<Real>::infinity();
    const Real o[3] = {r.origin.x, r.origin.y, r.origin.z};
    const Real d[3] = {r.direction.x, r.direction.y, r.direction.z};
    const Real lo[3] = {box.minPt.x, box.minPt.y, box.minPt.z};
    const Real hi[3] = {box.maxPt.x, box.maxPt.y, box.maxPt.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < kEpsilon) {
            if (o[i] < lo[i] || o[i] > hi[i]) return std::nullopt;
        } else {
            const Real inv = 1.0f / d[i];
            Real t1 = (lo[i] - o[i]) * inv;
            Real t2 = (hi[i] - o[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return std::nullopt;
        }
    }
    return tmin;
}

} // namespace lv
