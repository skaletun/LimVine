/**
 * @file    TemplateRunner.h
 * @brief   Запуск игровых шаблонов LimVine (templates/<имя>) в headless-режиме.
 * @ingroup Scripting
 *
 * @details Зачем нужен отдельный раннер
 *          ----------------------------
 *          Шаблон — это НЕ один файл: манифест `template.json` объявляет,
 *          какие модули стандартной библиотеки нужны игре и в каком порядке
 *          грузятся её скрипты. Раннер инкапсулирует эту рутину, чтобы один и
 *          тот же код загрузки использовался:
 *            * редактором (кнопка Play в viewport'е),
 *            * CLI-утилитой `tools/lvrun` (`lvrun templates/rpg_3p --frames 600`),
 *            * интеграционными тестами шаблонов (tests/template_tests.cpp).
 *
 *          Раннер собирает «мини-движок» (World + Physics + Input + Audio +
 *          Renderer c Null-бэкендом), поэтому шаблоны исполняются ровно в тех
 *          же биндингах, что и в настоящей игре, но без окна и GPU. Это
 *          принципиально для CI: логика шаблона проверяется на каждой сборке.
 *
 *          Порядок загрузки
 *          ----------------
 *          1. stdlib-модули из поля `stdlib` манифеста (tween/fsm/inventory/...);
 *          2. скрипты из поля `scripts` — строго в перечисленном порядке,
 *             потому что объявления верхнего уровня модуля являются глобалами,
 *             и `config.lvs` обязан отгрузиться раньше остальных.
 *
 *          Все файлы конкатенируются в один виртуальный модуль с маркерами
 *          `# ---- file <path> ----`: так диагностика компилятора и трейсбеки
 *          остаются читаемыми, а номера строк совпадают с исходником
 *          (маркер занимает ровно одну строку).
 */
#pragma once

#include "EngineBindings.h"
#include "../audio/Audio.h"
#include "../ecs/World.h"
#include "../input/Input.h"
#include "../physics/Physics.h"
#include "../render/Renderer.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lv::scripting {

/**
 * @brief Результат загрузки шаблона.
 */
struct TemplateLoadResult {
    bool ok = false;
    std::string id;                       ///< `template.json` -> "id".
    std::string name;                     ///< Человекочитаемое имя шаблона.
    std::vector<std::string> stdlib;      ///< Загруженные модули stdlib.
    std::vector<std::string> scripts;     ///< Загруженные скрипты шаблона.
    std::vector<std::string> errors;      ///< Диагностика компилятора и ошибки чтения файлов.
    std::string moduleName;               ///< Имя виртуального модуля (для поиска прототипа).
    std::string source;                   ///< Итоговый собранный исходник (для :disasm в консоли).
};

/**
 * @brief Headless-окружение для запуска игрового шаблона.
 *
 * Объект владеет всеми подсистемами; порядок объявления важен — `ctx_`
 * хранит указатели на них, поэтому подсистемы объявлены ДО `scripts_`.
 */
class TemplateRunner {
public:
    TemplateRunner();
    ~TemplateRunner();

    TemplateRunner(const TemplateRunner&) = delete;
    TemplateRunner& operator=(const TemplateRunner&) = delete;

    /// Подписаться на строки, которые скрипт печатает через `print`.
    void setLogCallback(ScriptWorld::LogCallback cb);

    /**
     * @brief Загрузить шаблон из каталога (читает `template.json`).
     * @param dir Каталог шаблона, например `templates/farming_iso`.
     * @param diags Сюда складываются ошибки чтения/компиляции.
     */
    TemplateLoadResult load(const std::filesystem::path& dir, std::vector<std::string>* diags = nullptr);

    /// Один кадр игры: tick мира (физика/системы) + tick скриптового планировщика.
    void tick(float dt);

    /// Прогнать @p frames кадров с фиксированным шагом @p dt.
    void runFrames(int frames, float dt = 1.0f / 60.0f);

    /// Вызвать глобальную скриптовую функцию без аргументов (например, `update`).
    bool callGlobal(const std::string& fnName);

    /**
     * @brief Вызвать глобальную функцию с аргументами и вернуть её результат.
     *
     * Используется тестами шаблонов: проверить игровую логику проще вызовом
     * `templateState()`, чем разбором текстового лога.
     */
    bool callGlobalFn(const std::string& fnName, const std::vector<lv::Value>& args, lv::Value* out);

    /**
     * @brief Вызвать метод глобального объекта-инстанса: `callMethodOnGlobal("farmer", "interact", {})`.
     *
     * Даёт тестам доступ к игровым действиям без необходимости эмулировать
     * ввод (который требует beginFrame/onKey/endFrame на каждый кадр).
     */
    bool callMethodOnGlobal(const std::string& globalInstance, const std::string& method,
                            const std::vector<lv::Value>& args, lv::Value* out);

    // -- Доступ к подсистемам (нужен тестам и редактору) --------------------
    [[nodiscard]] ScriptWorld& scripts() noexcept { return scripts_; }
    [[nodiscard]] ecs::World& world() noexcept { return world_; }
    [[nodiscard]] input::InputSystem& input() noexcept { return input_; }
    [[nodiscard]] physics::PhysicsWorld& physics() noexcept { return physics_; }
    [[nodiscard]] audio::AudioSystem& audio() noexcept { return audio_; }
    [[nodiscard]] render::Renderer& renderer() noexcept { return renderer_; }
    [[nodiscard]] const TemplateLoadResult& loaded() const noexcept { return loaded_; }

    /**
     * @brief Скомпилированный прототип загруженного шаблона (для дизассемблера).
     *
     * ScriptWorld сохраняет прототип каждого выполненного модуля, чтобы горячая
     * перезагрузка могла подменить тело, не трогая существующие замыкания.
     */
    [[nodiscard]] lv::ObjFunction* prototype() const noexcept;

private:
    void buildContext();

    ecs::World            world_;
    physics::PhysicsWorld physics_;
    input::InputSystem    input_;
    audio::AudioSystem    audio_;
    render::Renderer      renderer_;
    EngineContext         ctx_{};
    ScriptWorld           scripts_;
    TemplateLoadResult    loaded_;
};

/**
 * @brief Развернуть скриптовый map в удобную для тестов форму «ключ -> строка».
 *
 * `templateState()` шаблонов возвращает map со смешанными значениями
 * (числа, строки, bool). Тестам проще сравнивать строки, чем возиться с
 * NaN-boxing'ом, поэтому значение приводится к своему текстовому
 * представлению (`Value::toString()`), а числа дополнительно доступны как
 * double через `std::stod` на стороне вызывающего.
 */
[[nodiscard]] std::unordered_map<std::string, std::string> valueToMap(const lv::Value& v);

/// Прочитать текстовый файл целиком; при ошибке возвращает пустую строку.
[[nodiscard]] std::string readTextFile(const std::filesystem::path& p);

} // namespace lv::scripting
