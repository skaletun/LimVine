/**
 * @file    JobSystem.h
 * @brief   Пул потоков с рабочими очередями и параллельный for для ECS.
 * @ingroup Core
 *
 * @details Модель:
 *          - N worker-потоков (по умолчанию hardware_concurrency - 1);
 *          - у каждого worker'а своя дек-очередь (lock-free для своего потока,
 *            mutex для кражи), плюс глобальная очередь для внешних задач;
 *          - @c parallelFor делит диапазон на чанки по числу worker'ов и ждёт
 *            завершения через семафор-счётчик — это основной путь для систем ECS;
 *          - задачи НЕ аллоцируют в рантайме сверх необходимого: функтор
 *            хранится в @c std::function, но типичный job — это лямбда без
 *            захватов тяжёлых объектов.
 *
 *          Почему не std::async: нам нужен детерминированный порядок выполнения
 *          фаз и отсутствие неявных потоков, которые нельзя привязать к кадру.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lv {

using Job = std::function<void()>;

/**
 * @brief Пул потоков движка.
 *
 * Один экземпляр на процесс; создаётся в @c Engine::init и уничтожается в
 * @c Engine::shutdown. Все подсистемы (рендер-кадр, физика, скрипты, ассеты)
 * используют его через @c instance().
 */
class JobSystem {
public:
    /// Инициализировать пул. @p workerCount = 0 => hardware_concurrency - 1.
    static void init(std::uint32_t workerCount = 0);
    static void shutdown();
    [[nodiscard]] static JobSystem& instance();
    [[nodiscard]] static bool ready() noexcept;

    /// Поставить задачу в глобальную очередь.
    void submit(Job job);

    /**
     * @brief Параллельно выполнить @p body(i) для i из [0, count).
     *
     * Блокирует вызывающий поток до завершения всех чанков. Используется
     * системами ECS и culling'ом рендера.
     *
     * @param count     Число элементов.
     * @param chunkSize Минимальный размер чанка (кэш-линии дружелюбнее при 32-256).
     * @param body      Колбэк `void(std::size_t index)`.
     */
    void parallelFor(std::size_t count, std::size_t chunkSize, const std::function<void(std::size_t)>& body);

    /// Параллельно выполнить @p body(begin, end) по диапазонам.
    void parallelForRange(std::size_t count, std::size_t chunkSize,
                          const std::function<void(std::size_t, std::size_t)>& body);

    [[nodiscard]] std::uint32_t workerCount() const noexcept { return workerCount_; }
    [[nodiscard]] std::uint64_t jobsExecuted() const noexcept { return jobsExecuted_.load(std::memory_order_relaxed); }

    /// Индекс текущего worker'а (0..N-1); для главного потока возвращает N.
    [[nodiscard]] std::uint32_t currentWorker() const noexcept;

private:
    JobSystem() = default;
    ~JobSystem();
    void workerLoop(std::uint32_t index);
    bool tryPopJob(Job& out);

    std::uint32_t workerCount_ = 0;
    std::vector<std::thread> workers_;
    std::vector<std::uint32_t> workerIndexOfThread_;

    std::deque<Job> globalQueue_;
    std::mutex globalMutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> jobsExecuted_{0};
    std::atomic<std::uint32_t> activeJobs_{0};
    std::condition_variable idleCv_;
};

} // namespace lv
