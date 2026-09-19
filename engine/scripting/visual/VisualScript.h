/**
 * @file    VisualScript.h
 * @brief   Visual Scripting (Blueprints) -> LV Script: граф нод в читаемый код.
 * @ingroup Scripting
 *
 * @details Главный принцип подсистемы: **визуальный скрипт не является отдельным
 *          языком исполнения**. Граф нод компилируется в обычный .lvs-текст,
 *          который затем проходит штатный путь (лексер -> парсер -> байткод).
 *
 *          Это даёт четыре свойства, которые невозможно получить при «своём
 *          интерпретаторе графов»:
 *          1. **Round-trip**: из кода можно вернуться в граф (метаданные
 *             расположения нод хранятся в комментариях `#@node`);
 *          2. **Отладка**: в консоли редактора виден обычный трейсбек по строкам
 *             сгенерированного файла, а не «узел 47»;
 *          3. **Горячая перезагрузка** работает как для любого .lvs;
 *          4. **Моды** могут читать и править логику текстом, даже если она
 *             была собрана визуально.
 *
 *          Формат графа — JSON (см. docs/03_lvscript_spec.md). Два класса рёбер:
 *          - exec-рёбра (белые) определяют порядок выполнения;
 *          - data-рёбра (цветные) определяют выражения.
 *
 * @note Будьте осторожны с символами звёздочка+слэш внутри блочных комментариев:
 *       они закрывают комментарий, и остаток текста попадает в код.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lv::vscript {

/// Тип данных пина (влияет на цвет в редакторе и на проверку совместимости).
enum class PinType : std::uint8_t {
    Exec, Bool, Int, Float, String, Vec3, Entity, Any
};

[[nodiscard]] const char* pinTypeName(PinType t) noexcept;
[[nodiscard]] PinType pinTypeFromName(std::string_view n) noexcept;

/// Направление пина.
enum class PinDir : std::uint8_t { Input, Output };

/**
 * @brief Пин узла.
 */
struct Pin {
    std::string id;              ///< Имя пина внутри узла ("exec", "value", "then").
    PinType     type = PinType::Any;
    PinDir      dir = PinDir::Input;
    std::string literal;         ///< Значение по умолчанию, если вход не подключён.
    std::string displayName;     ///< Подпись в редакторе.
};

/**
 * @brief Узел графа.
 */
struct Node {
    std::uint32_t id = 0;
    std::string   type;          ///< "event.update", "action.print", "pure.math.add", ...
    std::string   title;         ///< Подпись (может переопределяться пользователем).
    std::vector<Pin> pins;
    /// Произвольные свойства узла (имя переменной, текст строки, имя функции...).
    std::unordered_map<std::string, std::string> props;
    float x = 0, y = 0;          ///< Позиция на холсте (сохраняется для round-trip).
    std::string comment;

    [[nodiscard]] const Pin* pin(std::string_view id) const;
    [[nodiscard]] std::string prop(std::string_view key, std::string_view fallback = {}) const;
};

/**
 * @brief Соединение: выход одного пина -> вход другого.
 */
struct Connection {
    std::uint32_t fromNode = 0;
    std::string   fromPin;
    std::uint32_t toNode = 0;
    std::string   toPin;
};

/**
 * @brief Граф визуального скрипта.
 */
struct Graph {
    std::string name = "VisualScript";
    std::vector<Node> nodes;
    std::vector<Connection> connections;

    [[nodiscard]] const Node* node(std::uint32_t id) const;
    [[nodiscard]] Node* node(std::uint32_t id);
    /// Найти соединение, входящее в (node, pin).
    [[nodiscard]] const Connection* inputOf(std::uint32_t node, std::string_view pin) const;
    /// Все соединения, выходящие из (node, pin) — для exec-цепочек.
    [[nodiscard]] std::vector<const Connection*> outputsOf(std::uint32_t node, std::string_view pin) const;
};

/**
 * @brief Результат транспиляции.
 */
struct TranspileResult {
    bool        ok = false;
    std::string code;                  ///< Сгенерированный .lvs.
    std::vector<std::string> errors;   ///< Ошибки графа (циклы в exec, отсутствие входа...).
    std::vector<std::string> warnings;
    std::size_t nodeCount = 0;
    std::size_t lineCount = 0;
};

/**
 * @brief Транспилятор графа в LV Script.
 *
 * Расширяемость: игровой проект может зарегистрировать СВОИ типы нод через
 * @c registerNodeKind — поэтому шаблоны (Farm/RPG/FPS) добавляют узлы вроде
 * «PlantCrop» или «PlayDialogue», не трогая код движка.
 */
class Transpiler {
public:
    /// Эмиттер exec-узла: получает узел, контекст и выдаёт строки кода.
    using ExecEmitter = std::function<void(const Node&, struct TranspileContext&, std::vector<std::string>& out)>;
    /// Эмиттер data-узла: возвращает LV Script выражение.
    using DataEmitter = std::function<std::string(const Node&, struct TranspileContext&)>;

    Transpiler();

    void registerExecNode(std::string_view type, ExecEmitter fn);
    void registerDataNode(std::string_view type, DataEmitter fn);

    /// Сгенерировать код. @p indent — базовый отступ (для вложенности в класс).
    [[nodiscard]] TranspileResult transpile(const Graph& g, int indent = 0) const;

    /// Обратное преобразование: из текста .lvs восстановить positions/comments.
    /// (Полный round-trip графа — в @c GraphParser; здесь извлекаются только
    /// метаданные `#@node`, чтобы редактор восстановил раскладку холста.)
    [[nodiscard]] static Graph parseMetadata(const std::string& lvsSource);

    /// Имя переменной, в которую сохраняется значение data-узла.
    [[nodiscard]] static std::string varNameFor(std::uint32_t nodeId, std::string_view pin);

    /// Скомпилировать выражение data-узла (через зарегистрированный эмиттер).
    [[nodiscard]] std::string emitExpression(const Node& n, TranspileContext& ctx) const;
    /// Скомпилировать exec-узел и всё, что идёт после него.
    void emitExecNode(std::uint32_t nodeId, TranspileContext& ctx, std::vector<std::string>& out) const;
    /// Пройти по exec-ребру из указанного выходного пина.
    void emitExecChain(const Node& from, std::string_view outPin, TranspileContext& ctx,
                       std::vector<std::string>& out) const;

private:
    std::unordered_map<std::string, ExecEmitter> exec_;
    std::unordered_map<std::string, DataEmitter> data_;
};

/**
 * @brief Контекст транспиляции: граф + состояние генерации.
 */
struct TranspileContext {
    const Graph* graph = nullptr;
    const Transpiler* transpiler = nullptr;
    int indent = 0;
    /// Кэш «узел.пин -> имя локальной переменной» (чтобы не вычислять дважды).
    std::unordered_map<std::string, std::string> valueCache;
    /// Счётчики для генерации уникальных имён.
    std::uint32_t tempCounter = 0;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    /// Рекурсивно вычислить выражение для входа (node, pin).
    [[nodiscard]] std::string exprFor(std::uint32_t node, std::string_view pin);
    /// Текущий отступ строками.
    [[nodiscard]] std::string pad() const;
    /// Выдать строку кода с текущим отступом.
    void emit(std::vector<std::string>& out, std::string line);

    /// Узлы текущей exec-цепочки (защита от циклов в графе).
    /// Отдельно от @c emitted: emitted отмечает «уже в файле», а chain —
    /// «раскрывается прямо сейчас», иначе цикл 1->2->1 не обнаруживается.
    std::unordered_set<std::uint32_t> chain;
    /// Узлы, уже скомпилированные в выходной файл.
    std::unordered_set<std::uint32_t> emitted;
    /**
     * @brief Переменные, введённые текущим exec-узлом (например, индекс цикла).
     *
     * Когда data-вход ссылается на выход flow.forRange, выражения не существует:
     * значение — это локальная переменная цикла. Карта «nodeId -> имя переменной»
     * позволяет корректно подставить её в тело.
     */
    std::unordered_map<std::uint32_t, std::string> loopVars;
};

// ---------------------------------------------------------------------------
//  JSON: минимальный парсер/сериализатор графа (без внешних зависимостей)
// ---------------------------------------------------------------------------

/// Разобрать JSON-описание графа. Возвращает false при синтаксической ошибке.
[[nodiscard]] bool parseGraphJson(const std::string& json, Graph& out, std::string& err);
/// Сериализовать граф в JSON (формат сохранения .lvgraph).
[[nodiscard]] std::string serializeGraphJson(const Graph& g);

} // namespace lv::vscript
