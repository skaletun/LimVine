/**
 * @file    Scheduler.cpp
 * @brief   Реализация планировщика корутин.
 */
#include "Scheduler.h"

#include <algorithm>
#include <chrono>

namespace lv {

void Scheduler::markRoots(GC& gc) {
    for (ObjCoroutine* co : coroutines_) gc.markObject(co);
    for (ObjCoroutine* co : waiting_) gc.markObject(co);
    for (auto& [k, v] : eventWaiting_) for (ObjCoroutine* co : v) gc.markObject(co);
    for (const PendingCall& pc : timers_) {
        gc.mark(pc.fn);
        for (const Value& a : pc.args) gc.mark(a);
    }
}

void Scheduler::registerCoroutine(ObjCoroutine* co) {
    if (!co) return;
    if (std::find(coroutines_.begin(), coroutines_.end(), co) == coroutines_.end())
        coroutines_.push_back(co);
}

ObjCoroutine* Scheduler::start(Value closure, std::vector<Value> args) {
    if (!closure.isObject() || closure.asObject()->type != ObjHeader::Type::Closure) {
        vm_.runtimeErrorPublic("Scheduler::start expects a function value");
        return nullptr;
    }
    Value coVal = vm_.makeCoroutine(closure.as<ObjClosure>());
    auto* co = coVal.as<ObjCoroutine>();
    registerCoroutine(co);

    // Аргументы передаются первым resume'ом как массив.
    ObjArray* arr = lv::makeArray(vm_.gc(), static_cast<std::uint32_t>(args.size()));
    for (const Value& v : args) { arr->grow(vm_.gc(), arr->count + 1); arr->items[arr->count++] = v; }

    vm_.resumeCoroutine(co, Value::object(arr));
    vm_.tickRun();
    adopt(co);
    return co;
}

void Scheduler::adopt(ObjCoroutine* co) {
    if (!co) return;
    registerCoroutine(co);
    if (co->state != ObjCoroutine::State::Suspended) return;
    if (co->waitEvent) {
        auto& list = eventWaiting_[std::string(co->waitEvent->view())];
        if (std::find(list.begin(), list.end(), co) == list.end()) list.push_back(co);
        return;
    }
    if (std::find(waiting_.begin(), waiting_.end(), co) != waiting_.end()) return;
    waiting_.push_back(co);
    std::sort(waiting_.begin(), waiting_.end(),
              [](const ObjCoroutine* a, const ObjCoroutine* b) { return a->waitUntil < b->waitUntil; });
}

std::size_t Scheduler::tick(double dt) {
    time_ += dt;
    vm_.setGameTime(time_);
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t resumed = 0;

    // Подбираем корутины, приостановленные не через Scheduler::start
    // (например, первичная корутина, запущенная обычным вызовом из скрипта).
    for (ObjCoroutine* co : coroutines_) adopt(co);

    // 1) Временны́е корутины.
    while (!waiting_.empty() && waiting_.front()->waitUntil <= time_) {
        ObjCoroutine* co = waiting_.front();
        waiting_.erase(waiting_.begin());
        if (co->state == ObjCoroutine::State::Dead) continue;
        vm_.resumeCoroutine(co, Value::fromNumber(time_));
        // Статус не нужен: ошибка рантайма уже доставлена через errorHandler
        // корутины, а прерывание (suspend) обрабатывает сам планировщик ниже.
        (void)vm_.tickRun();
        ++resumed;
        adopt(co);   // повторно встаёт в очередь, если снова приостановлена
        if (budgetMs_ > 0.0) {
            const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (spent >= budgetMs_) break;   // остаток — на следующий кадр
        }
    }

    // 2) Таймеры.
    for (std::size_t i = 0; i < timers_.size();) {
        PendingCall& pc = timers_[i];
        if (pc.at <= time_) {
            vm_.callFunction(pc.fn, pc.args);
            if (pc.interval > 0.0) { pc.at = time_ + pc.interval; ++i; }
            else { timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(i)); }
        } else ++i;
    }

    reap();
    return resumed;
}

std::size_t Scheduler::fire(std::string_view name, Value payload) {
    auto it = eventWaiting_.find(std::string(name));
    if (it == eventWaiting_.end()) return 0;
    auto list = std::move(it->second);
    eventWaiting_.erase(it);
    std::size_t n = 0;
    for (ObjCoroutine* co : list) {
        if (co->state != ObjCoroutine::State::Suspended) continue;
        co->waitEvent = nullptr;
        vm_.resumeCoroutine(co, payload);
        vm_.tickRun();
        ++n;
        adopt(co);
    }
    reap();
    return n;
}

void Scheduler::stop(ObjCoroutine* co) {
    if (!co) return;
    co->state = ObjCoroutine::State::Dead;
    co->waitEvent = nullptr;
    waiting_.erase(std::remove(waiting_.begin(), waiting_.end(), co), waiting_.end());
    for (auto& [k, v] : eventWaiting_) v.erase(std::remove(v.begin(), v.end(), co), v.end());
}

void Scheduler::stopAll() {
    for (ObjCoroutine* co : coroutines_) { co->state = ObjCoroutine::State::Dead; co->waitEvent = nullptr; }
    coroutines_.clear();
    waiting_.clear();
    eventWaiting_.clear();
    timers_.clear();
}

void Scheduler::reap() {
    coroutines_.erase(std::remove_if(coroutines_.begin(), coroutines_.end(),
                                     [](ObjCoroutine* c) { return c->state == ObjCoroutine::State::Dead; }),
                      coroutines_.end());
    waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(),
                                  [](ObjCoroutine* c) { return c->state == ObjCoroutine::State::Dead; }),
                   waiting_.end());
}

void Scheduler::schedule(Value fn, double delay, std::vector<Value> args) {
    timers_.push_back(PendingCall{fn, std::move(args), time_ + delay, 0.0, 0});
}

std::uint32_t Scheduler::scheduleRepeating(Value fn, double interval, std::vector<Value> args) {
    const std::uint32_t id = nextTimerId_++;
    timers_.push_back(PendingCall{fn, std::move(args), time_ + interval, interval, id});
    return id;
}

void Scheduler::cancelTimer(std::uint32_t id) {
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [id](const PendingCall& p) { return p.id == id; }),
                  timers_.end());
}

} // namespace lv
