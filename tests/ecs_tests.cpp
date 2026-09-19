#include "ecs/Registry.h"
#include "core/Math.h"
#include <cstdio>
#include <chrono>
using namespace lv;
using namespace lv::ecs;

struct TransformC { Vec3 position; Quat rotation; Vec3 scale{1,1,1}; static constexpr std::string_view lv_component_name = "Transform"; };
struct Health { float current = 100, max = 100; static constexpr std::string_view lv_component_name = "Health"; };
struct Velocity { Vec3 linear; static constexpr std::string_view lv_component_name = "Velocity"; static constexpr bool lv_volatile = true; };
struct IsEnemy { static constexpr std::string_view lv_component_name = "IsEnemy"; };

namespace {
int g_passed = 0, g_failed = 0;
void check(bool cond, const char* what) {
  if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
  else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
} // namespace

int main(){
  Registry r;
  registerComponent<TransformC>(); registerComponent<Health>(); registerComponent<Velocity>(); registerComponent<IsEnemy>();

  for (int i=0;i<1000;++i) {
    Entity e = r.create();
    r.add<TransformC>(e, TransformC{{(Real)i,0,0}});
    if (i % 2 == 0) r.add<Health>(e, Health{50.f, 100.f});
    if (i % 3 == 0) r.add<Velocity>(e, Velocity{{0,(Real)i,0}});
    if (i % 7 == 0) r.add<IsEnemy>(e);
  }
  std::printf("\n== ECS: archetype storage ==\n");
  check(r.entityCount() == 1000, "1000 entities created");
  check(r.archetypeCount() == 9, "9 distinct archetypes (2^3 component combinations + empty)");

  std::size_t n=0; Real sum=0;
  r.each<TransformC, Health>([&](Entity e, TransformC& t, Health& h){ ++n; sum += t.position.x + h.current; (void)e; return true; });
  check(n == 500, "query <Transform, Health> matches exactly 500 entities");
  check(sum > 0, "component data is readable through the query");
  (void)sum;

  // structural change: remove component
  Entity victim = r.collect<TransformC, Health, Velocity>()[0];
  std::printf("\n== ECS: structural changes ==\n");
  check(r.alive(victim) && r.has<Health>(victim), "victim has Health before removal");
  r.remove<Health>(victim);
  check(!r.has<Health>(victim) && r.alive(victim) && r.has<TransformC>(victim),
        "component removal migrates the archetype and keeps other components");

  // destroy + handle invalidation
  Entity e2 = r.create();
  r.add<TransformC>(e2);
  r.destroy(e2);
  check(!r.alive(e2), "destroyed entity is not alive");
  Entity recycled = r.create();
  check(recycled.index == e2.index && recycled.generation != e2.generation,
        "slot is recycled with a bumped generation");
  check(!r.alive(e2), "stale handle stays invalid after recycling");
  r.destroy(recycled);

  // tag component query
  std::size_t enemies=0; r.each<IsEnemy, TransformC>([&](Entity, IsEnemy&, TransformC&){ ++enemies; return true; });
  std::printf("\n== ECS: tag components & stats ==\n");
  check(enemies == 143, "zero-sized tag component filters correctly (1000/7)");

  auto st = r.stats();
  check(st.entities == r.entityCount(), "stats agree with entityCount");
  check(st.bytes < 8u * 1024 * 1024, "1000 entities fit in well under 8 MB");
  std::printf("  bytes=%zu migrations=%zu\n", st.bytes, st.migrations);

  // --- Производительность: линейное сканирование архетипа ---
  {
    Registry big;
    for (int i = 0; i < 50000; ++i) {
      Entity e = big.create();
      big.add<TransformC>(e, TransformC{{(Real)i, 0, 0}});
      big.add<Velocity>(e, Velocity{{0, 1, 0}});
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 20; ++pass) {
      big.each<TransformC, Velocity>([](Entity, TransformC& t, Velocity& v) {
        t.position += v.linear * 0.016f;
        return true;
      });
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("  50k entities x 20 passes of movement: %.2f ms (%.0f M entity-updates/s)\n",
                ms, 50000.0 * 20.0 / (ms * 1000.0));
    check(ms < 400.0, "50k entities move 20x faster than 400 ms (cache-friendly SoA)");
  }

  std::printf("\n----------------------------------------\n");
  std::printf("ECS self-test: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
