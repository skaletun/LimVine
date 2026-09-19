/**
 * @file    Scheduler.h
 * @brief   Планировщик корутин LV Script.
 * @ingroup LVScript
 *
 * @details Корутины — основной способ писать «растянутую во времени» игровую
 *          логику без callback-ада:
 * @code
 * coroutine GrowCrop(crop) do
 *     for stage in 0..3 do
 *         wait(CropData.stages[stage].seconds)   # приостановка
 *         crop.SetStage(stage)
 *     end
 *     crop.Harvestable()
 * end
 * @endcode
 *
 *          Планировщик хранит «спящие» корутины в двух очередях:
 *          - временны́е (@c wait(sec)) — отсортированы по моменту пробуждения;
 *          - событийные (@c waitEvent("name")) — пробуждаются по @c fire().
 *
 *          Каждый кадр хост вызывает @c tick(dt); планировщик возобновляет
 *          готовые корутины и прогоняет VM до следующей приостановки.
 *          @c tickBudgetMs ограничивает время, которое скрипты могут съесть
 *          в кадре, — остаток переносится на следующий кадр (защита от фризов).
 */
#pragma once

#include "VM.h"

#include <deque>

namespace lv {

/**
 * @brief Планировщик корутин и отложенных вызовов.
 */
class Scheduler {
public:
    explicit Scheduler(VM& vm) : vm_(vm) { vm_.attachScheduler(this); }

    /// Продвинуть время и возобновить готовые корутины.
    /// @param dt Дельта времени кадра в секундах.
    /// @return Число возобновлённых корутин.
    std::size_t tick(double dt);

    /// Задать временной бюджет на скрипты в этом кадре (0 = без ограничения).
    void setFrameBudgetMs(double ms) noexcept { budgetMs_ = ms; }

    /// Зарегистрировать корутину (вызывается VM при `Op::MakeCoroutine`).
    void registerCoroutine(ObjCoroutine* co);

    /**
     * @brief «Подобрать» корутину: зарегистрировать и поставить в нужную очередь.
     *
     * Вызывается для каждой известной корутины в начале @c tick, поэтому
     * первичные корутины, запущенные обычным вызовом из скрипта
     * (`coroutine grow() ... grow()`), тоже корректно возобновляются.
     */
    void adopt(ObjCoroutine* co);

    /**
     * @brief Создать и немедленно запустить корутину.
     *
     * Это основной API для C++-стороны: `world.script().startCoroutine(fn, {args...})`.
     */
    ObjCoroutine* start(Value closure, std::vector<Value> args = {});

    /// Пробудить все корутины, ожидающие событие @c name.
    std::size_t fire(std::string_view name, Value payload = Value::nil());

    /// Остановить конкретную корутину.
    void stop(ObjCoroutine* co);

    /// Остановить все корутины (используется при выгрузке сцены).
    void stopAll();

    /// Удалить завершённые корутины из реестра (вызывается автоматически в tick).
    void reap();

    [[nodiscard]] double time() const noexcept { return time_; }
    [[nodiscard]] std::size_t activeCount() const noexcept { return coroutines_.size(); }
    [[nodiscard]] std::size_t waitingCount() const noexcept { return waiting_.size() + eventWaiting_.size(); }

    /// Пометить все корутины как корни GC (вызывается из VM::markRoots).
    void markRoots(GC& gc);

    /// Отложенный вызов функции через @c delay секунд (одноразовый таймер).
    void schedule(Value fn, double delay, std::vector<Value> args = {});

    /// Повторяющийся вызов каждые @c interval секунд. Возвращает id для отмены.
    std::uint32_t scheduleRepeating(Value fn, double interval, std::vector<Value> args = {});
    void cancelTimer(std::uint32_t id);

private:
    struct PendingCall {
        Value fn;
        std::vector<Value> args;
        double at = 0.0;
        double interval = 0.0;   // 0 = одноразовый
        std::uint32_t id = 0;
    };

    VM& vm_;
    double time_ = 0.0;
    double budgetMs_ = 0.0;
    std::uint32_t nextTimerId_ = 1;

    /// Все живые корутины (корень для GC).
    std::vector<ObjCoroutine*> coroutines_;
    /// Корутины, ожидающие времени.
    std::vector<ObjCoroutine*> waiting_;
    /// Корутины, ожидающие событие: event -> coroutines.
    std::unordered_map<std::string, std::vector<ObjCoroutine*>> eventWaiting_;
    std::vector<PendingCall> timers_;
};

} // namespace lv
