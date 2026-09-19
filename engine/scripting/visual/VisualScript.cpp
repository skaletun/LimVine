/**
 * @file    VisualScript.cpp
 * @brief   Транспилятор графа нод в LV Script + JSON-сериализация графа.
 */
#include "VisualScript.h"
#include "JsonMini.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace lv::vscript {

// ---------------------------------------------------------------------------
//  Базовые типы
// ---------------------------------------------------------------------------
const char* pinTypeName(PinType t) noexcept {
    switch (t) {
        case PinType::Exec:   return "exec";
        case PinType::Bool:   return "bool";
        case PinType::Int:    return "int";
        case PinType::Float:  return "float";
        case PinType::String: return "string";
        case PinType::Vec3:   return "vec3";
        case PinType::Entity: return "entity";
        default:              return "any";
    }
}

PinType pinTypeFromName(std::string_view n) noexcept {
    if (n == "exec")   return PinType::Exec;
    if (n == "bool")   return PinType::Bool;
    if (n == "int")    return PinType::Int;
    if (n == "float")  return PinType::Float;
    if (n == "string") return PinType::String;
    if (n == "vec3")   return PinType::Vec3;
    if (n == "entity") return PinType::Entity;
    return PinType::Any;
}

const Pin* Node::pin(std::string_view id) const {
    for (const Pin& p : pins) if (p.id == id) return &p;
    return nullptr;
}

std::string Node::prop(std::string_view key, std::string_view fallback) const {
    auto it = props.find(std::string(key));
    return it == props.end() ? std::string(fallback) : it->second;
}

const Node* Graph::node(std::uint32_t id) const {
    for (const Node& n : nodes) if (n.id == id) return &n;
    return nullptr;
}
Node* Graph::node(std::uint32_t id) {
    for (Node& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

const Connection* Graph::inputOf(std::uint32_t nodeId, std::string_view pin) const {
    for (const Connection& c : connections)
        if (c.toNode == nodeId && c.toPin == pin) return &c;
    return nullptr;
}

std::vector<const Connection*> Graph::outputsOf(std::uint32_t nodeId, std::string_view pin) const {
    std::vector<const Connection*> out;
    for (const Connection& c : connections)
        if (c.fromNode == nodeId && c.fromPin == pin) out.push_back(&c);
    return out;
}

// ---------------------------------------------------------------------------
//  Контекст
// ---------------------------------------------------------------------------
std::string Transpiler::varNameFor(std::uint32_t nodeId, std::string_view pin) {
    std::ostringstream os;
    os << "n" << nodeId << "_" << pin;
    return os.str();
}

std::string TranspileContext::pad() const { return std::string(static_cast<std::size_t>(indent) * 4, ' '); }

void TranspileContext::emit(std::vector<std::string>& out, std::string line) {
    out.push_back(line.empty() ? std::string() : pad() + line);
}

std::string TranspileContext::exprFor(std::uint32_t node, std::string_view pin) {
    // Ключ кэша: «узел:пин». Чистые (pure) ноды вычисляются один раз и
    // сохраняются в локальную переменную — так же, как это делает программист.
    const std::string key = std::to_string(node) + ":" + std::string(pin);
    auto cached = valueCache.find(key);
    if (cached != valueCache.end()) return cached->second;

    const Node* n = graph->node(node);
    if (!n) { errors.push_back("connection refers to missing node " + std::to_string(node)); return "nil"; }

    // Если запрашивается вход пина, а не выход — спускаемся к источнику.
    if (const Pin* p = n->pin(pin); p && p->dir == PinDir::Input) {
        if (const Connection* c = graph->inputOf(node, pin)) {
            const std::string e = exprFor(c->fromNode, c->fromPin);
            valueCache[key] = e;
            return e;
        }
        // Не подключён — используем литерал пина.
        std::string lit = p->literal;
        if (lit.empty()) {
            switch (p->type) {
                case PinType::Bool:   lit = "false"; break;
                case PinType::Int:
                case PinType::Float:  lit = "0"; break;
                case PinType::String: lit = "\"\""; break;
                case PinType::Vec3:   lit = "vec3(0, 0, 0)"; break;
                default:              lit = "nil"; break;
            }
        }
        valueCache[key] = lit;
        return lit;
    }

    // Выходной пин цикла — это его переменная, а не вычисляемое выражение.
    if (const auto lv = loopVars.find(node); lv != loopVars.end()) {
        valueCache[key] = lv->second;
        return lv->second;
    }

    // Выходной пин: ищем эмиттер типа ноды.
    const std::string expr = transpiler->emitExpression(*n, *this);
    valueCache[key] = expr;
    return expr;
}

// ---------------------------------------------------------------------------
//  Транспилятор
// ---------------------------------------------------------------------------
namespace {
/// Экранировать строковый литерал для LV Script.
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    out += '"';
    return out;
}
} // namespace

std::string Transpiler::emitExpression(const Node& n, TranspileContext& ctx) const {
    auto it = data_.find(n.type);
    if (it != data_.end()) return it->second(n, ctx);

    // Неизвестная pure-нода трактуется как обращение к одноимённой функции движка:
    // `myMod.doSomething(a, b)`. Это позволяет проектам добавлять ноды без
    // изменения кода движка.
    std::ostringstream os;
    os << n.prop("call", n.type) << "(";
    bool first = true;
    for (const Pin& p : n.pins) {
        if (p.dir != PinDir::Input || p.type == PinType::Exec) continue;
        if (!first) os << ", ";
        first = false;
        os << ctx.exprFor(n.id, p.id);
    }
    os << ")";
    ctx.warnings.push_back("node type '" + n.type + "' has no registered emitter; emitted as a direct call");
    return os.str();
}

void Transpiler::registerExecNode(std::string_view type, ExecEmitter fn) { exec_[std::string(type)] = std::move(fn); }
void Transpiler::registerDataNode(std::string_view type, DataEmitter fn) { data_[std::string(type)] = std::move(fn); }

Transpiler::Transpiler() {
    // ---------------------------------------------------------------- данные
    registerDataNode("pure.literal", [](const Node& n, TranspileContext&) {
        const Pin* p = n.pin("value");
        if (!p) return std::string("nil");
        if (p->type == PinType::String) return quote(p->literal);
        return p->literal.empty() ? std::string("nil") : p->literal;
    });
    registerDataNode("pure.variable.get", [](const Node& n, TranspileContext&) { return n.prop("name", "nil"); });
    registerDataNode("pure.math.add", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " + " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.math.sub", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " - " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.math.mul", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " * " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.math.div", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " / " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.math.neg", [](const Node& n, TranspileContext& c) { return "(-" + c.exprFor(n.id, "a") + ")"; });
    registerDataNode("pure.compare.eq", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " == " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.compare.ne", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " != " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.compare.lt", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " < " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.compare.gt", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " > " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.compare.le", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " <= " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.compare.ge", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " >= " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.logic.and", [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " and " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.logic.or",  [](const Node& n, TranspileContext& c) { return "(" + c.exprFor(n.id, "a") + " or " + c.exprFor(n.id, "b") + ")"; });
    registerDataNode("pure.logic.not", [](const Node& n, TranspileContext& c) { return "(not " + c.exprFor(n.id, "a") + ")"; });
    registerDataNode("pure.string.join", [](const Node& n, TranspileContext& c) {
        return "(" + c.exprFor(n.id, "a") + " + " + c.exprFor(n.id, "b") + ")";
    });
    registerDataNode("pure.string.format", [](const Node& n, TranspileContext& c) {
        std::string fmt = n.prop("format", "{}");
        // "{}" -> интерполяция LV Script: заменяем плейсхолдеры на {expr}.
        std::string out = "\"";
        std::size_t argIndex = 0;
        for (std::size_t i = 0; i < fmt.size();) {
            if (i + 1 < fmt.size() && fmt[i] == '{' && fmt[i + 1] == '}') {
                const std::string pin = "arg" + std::to_string(argIndex++);
                out += "{" + c.exprFor(n.id, pin) + "}";
                i += 2;
            } else {
                if (fmt[i] == '"' || fmt[i] == '\\') out += '\\';
                out += fmt[i++];
            }
        }
        out += "\"";
        return out;
    });
    registerDataNode("pure.vec3.make", [](const Node& n, TranspileContext& c) {
        return "vec3(" + c.exprFor(n.id, "x") + ", " + c.exprFor(n.id, "y") + ", " + c.exprFor(n.id, "z") + ")";
    });
    registerDataNode("pure.vec3.component", [](const Node& n, TranspileContext& c) {
        return c.exprFor(n.id, "v") + "." + n.prop("component", "x");
    });
    registerDataNode("pure.engine.position", [](const Node& n, TranspileContext& c) {
        return "getPosition(" + c.exprFor(n.id, "entity") + ")";
    });
    registerDataNode("pure.engine.inputAxis", [](const Node& n, TranspileContext&) {
        return "inputAxis(" + quote(n.prop("action", "MoveX")) + ")";
    });
    registerDataNode("pure.engine.inputHeld", [](const Node& n, TranspileContext&) {
        return "inputHeld(" + quote(n.prop("action", "Fire")) + ")";
    });
    registerDataNode("pure.engine.deltaTime", [](const Node&, TranspileContext&) { return "deltaTime()"; });
    registerDataNode("pure.engine.time", [](const Node&, TranspileContext&) { return "time()"; });
    registerDataNode("pure.engine.entityCount", [](const Node&, TranspileContext&) { return "entityCount()"; });

    // ------------------------------------------------------- исполняемые узлы
    registerExecNode("event.entry", [](const Node&, TranspileContext&, std::vector<std::string>&) {
        // Служебный узел: сам код не генерирует, объявляется обёрткой функции.
    });
    registerExecNode("event.update", [](const Node&, TranspileContext&, std::vector<std::string>&) {});
    registerExecNode("event.start",  [](const Node&, TranspileContext&, std::vector<std::string>&) {});
    registerExecNode("event.custom", [](const Node&, TranspileContext&, std::vector<std::string>&) {});

    registerExecNode("action.print", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "print(" + c.exprFor(n.id, "value") + ")");
    });
    registerExecNode("action.setVariable", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, n.prop("name", "_") + " = " + c.exprFor(n.id, "value"));
    });
    registerExecNode("action.spawn", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        const std::string var = n.prop("result", "spawned");
        c.emit(out, "global " + var + " = spawn()");
        const std::string pos = c.exprFor(n.id, "position");
        if (!pos.empty() && pos != "nil") c.emit(out, "setPosition(" + var + ", " + pos + ")");
    });
    registerExecNode("action.destroy", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "destroy(" + c.exprFor(n.id, "entity") + ")");
    });
    registerExecNode("action.translate", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "translate(" + c.exprFor(n.id, "entity") + ", " + c.exprFor(n.id, "delta") + ")");
    });
    registerExecNode("action.setPosition", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "setPosition(" + c.exprFor(n.id, "entity") + ", " + c.exprFor(n.id, "position") + ")");
    });
    registerExecNode("action.callFunction", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        std::string call = n.prop("function", "nop") + "(";
        bool first = true;
        for (const Pin& p : n.pins) {
            if (p.dir != PinDir::Input || p.type == PinType::Exec) continue;
            if (!first) call += ", ";
            first = false;
            call += c.exprFor(n.id, p.id);
        }
        call += ")";
        const std::string result = n.prop("result", "");
        c.emit(out, result.empty() ? call : ("global " + result + " = " + call));
    });
    registerExecNode("action.coroutine", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, n.prop("function", "co") + "()");
    });
    registerExecNode("action.wait", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "wait(" + c.exprFor(n.id, "seconds") + ")");
    });

    registerExecNode("flow.branch", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "if " + c.exprFor(n.id, "condition") + " do");
        ++c.indent;
        const std::size_t before = out.size();
        c.transpiler->emitExecChain(n, "true", c, out);
        if (out.size() == before) c.emit(out, "pass");
        --c.indent;
        const bool hasFalse = !c.graph->outputsOf(n.id, "false").empty();
        if (hasFalse) {
            c.emit(out, "else do");
            ++c.indent;
            c.transpiler->emitExecChain(n, "false", c, out);
            --c.indent;
        }
        c.emit(out, "end");
    });

    registerExecNode("flow.sequence", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        // Sequence — просто несколько исходящих exec-портов; компилируем подряд.
        for (std::uint32_t i = 0;; ++i) {
            const std::string pin = "then" + std::to_string(i);
            if (c.graph->outputsOf(n.id, pin).empty()) break;
            c.transpiler->emitExecChain(n, pin, c, out);
        }
    });

    registerExecNode("flow.forRange", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        const std::string var = n.prop("index", "i");
        // Выходной пин цикла — это его индекс; data-ссылки на него становятся
        // обращением к локальной переменной цикла.
        c.loopVars[n.id] = var;
        const std::string from = c.exprFor(n.id, "from");
        const std::string to = c.exprFor(n.id, "to");
        const std::string inclusive = n.prop("inclusive", "0") == "1" ? "..=" : "..";
        c.emit(out, "for " + var + " in " + from + inclusive + to + " do");
        ++c.indent;
        c.transpiler->emitExecChain(n, "body", c, out);
        --c.indent;
        c.emit(out, "end");
        c.loopVars.erase(n.id);
        // После цикла продолжаем основную цепочку.
        c.transpiler->emitExecChain(n, "completed", c, out);
    });

    registerExecNode("flow.whileLoop", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "while " + c.exprFor(n.id, "condition") + " do");
        ++c.indent;
        c.transpiler->emitExecChain(n, "body", c, out);
        --c.indent;
        c.emit(out, "end");
        c.transpiler->emitExecChain(n, "completed", c, out);
    });

    registerExecNode("flow.break", [](const Node&, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "break");
    });
    registerExecNode("flow.continue", [](const Node&, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "continue");
    });
    registerExecNode("flow.comment", [](const Node& n, TranspileContext& c, std::vector<std::string>& out) {
        c.emit(out, "# " + n.prop("text", ""));
        c.transpiler->emitExecChain(n, "then", c, out);
    });
}

void Transpiler::emitExecChain(const Node& from, std::string_view outPin, TranspileContext& ctx,
                               std::vector<std::string>& out) const {
    for (const Connection* c : ctx.graph->outputsOf(from.id, outPin)) {
        // Из exec-порта могут выходить и DATA-рёбра (например, тело цикла питает
        // вход форматтера). Исполняемой цепочкой считается только соединение,
        // у которого целевой пин объявлен как exec-вход.
        const Node* target = ctx.graph->node(c->toNode);
        if (!target) continue;
        const Pin* tp = target->pin(c->toPin);
        if (!tp || tp->type != PinType::Exec || tp->dir != PinDir::Input) continue;
        emitExecNode(c->toNode, ctx, out);
    }
}

void Transpiler::emitExecNode(std::uint32_t nodeId, TranspileContext& ctx, std::vector<std::string>& out) const {
    const Node* n = ctx.graph->node(nodeId);
    if (!n) { ctx.errors.push_back("exec chain references missing node"); return; }

    // Защита от бесконечной рекурсии на циклических exec-графах: узел уже
    // раскрывается в ЭТОЙ цепочке. Проверка стоит здесь, а не в emitExecChain,
    // потому что продолжение по умолчанию вызывается после того, как узел
    // удалён из chain — иначе цикл 1->2->1 не обнаруживался.
    if (ctx.chain.count(nodeId)) {
        ctx.errors.push_back("execution cycle detected at node " + std::to_string(nodeId) +
                             " — LV Script генерирует циклы только через flow.forRange/flow.whileLoop");
        return;
    }
    ctx.chain.insert(nodeId);
    ctx.emitted.insert(nodeId);

    // Метаданные round-trip: по ним редактор восстановит раскладку холста.
    {
        std::ostringstream os;
        os << "#@node id=" << n->id << " type=" << n->type
           << " pos=" << static_cast<int>(n->x) << "," << static_cast<int>(n->y);
        if (!n->title.empty()) os << " title=" << n->title;
        ctx.emit(out, os.str());
    }

    auto it = exec_.find(n->type);
    if (it != exec_.end()) it->second(*n, ctx, out);
    else ctx.warnings.push_back("unknown exec node type '" + n->type + "' — skipped");

    // Продолжение по умолчанию: выход "then" (если узел сам его не обработал).
    static const std::vector<std::string> kSelfChaining = {
        "flow.branch", "flow.sequence", "flow.forRange", "flow.whileLoop", "flow.comment"
    };
    const bool selfChains = std::find(kSelfChaining.begin(), kSelfChaining.end(), n->type) != kSelfChaining.end();
    if (!selfChains) {
        for (const Connection* c : ctx.graph->outputsOf(nodeId, "then")) {
            const Node* target = ctx.graph->node(c->toNode);
            if (!target) continue;
            const Pin* tp = target->pin(c->toPin);
            if (!tp || tp->type != PinType::Exec || tp->dir != PinDir::Input) continue;
            if (ctx.emitted.count(c->toNode) || ctx.chain.count(c->toNode)) {
                // Узел уже скомпилирован ИЛИ является частью текущей цепочки —
                // второй случай и есть цикл в exec-графе.
                if (ctx.chain.count(c->toNode)) {
                    ctx.errors.push_back("execution cycle detected at node " + std::to_string(c->toNode) +
                                         " — LV Script генерирует циклы только через flow.forRange/flow.whileLoop");
                }
                continue;
            }
            emitExecNode(c->toNode, ctx, out);
        }
    }
    ctx.chain.erase(nodeId);
}

TranspileResult Transpiler::transpile(const Graph& g, int baseIndent) const {
    TranspileResult res;
    res.nodeCount = g.nodes.size();

    TranspileContext ctx;
    ctx.graph = &g;
    ctx.transpiler = this;
    ctx.indent = baseIndent;

    std::ostringstream header;
    header << "# ============================================================\n"
           << "# Сгенерировано Visual Scripting движка LimVine.\n"
           << "# Граф: " << g.name << " (" << g.nodes.size() << " узлов)\n"
           << "#\n"
           << "# Файл является ПОЛНОЦЕННЫМ исходником LV Script: его можно\n"
           << "# править руками, а строки '#@node' позволяют редактору\n"
           << "# восстановить раскладку холста (round-trip).\n"
           << "# ============================================================\n\n";

    std::vector<std::string> body;

    // 1) Узлы-события становятся функциями-обработчиками движка.
    bool anyEvent = false;
    for (const Node& n : g.nodes) {
        if (n.type.rfind("event.", 0) != 0) continue;
        anyEvent = true;
        std::string fnName;
        std::string params;
        if (n.type == "event.update")      { fnName = "update";     params = "dt"; }
        else if (n.type == "event.start")  { fnName = "start";      params = ""; }
        else if (n.type == "entry")        { fnName = "main";       params = ""; }
        else                               { fnName = n.prop("name", "onEvent"); params = n.prop("params", ""); }

        ctx.tempCounter = 0;
        ctx.valueCache.clear();
        ctx.chain.clear();
        ctx.emitted.clear();
        body.push_back("func " + fnName + "(" + params + ") do");
        ++ctx.indent;
        std::vector<std::string> inner;
        emitExecChain(n, "then", ctx, inner);
        for (auto& line : inner) body.push_back(line);
        if (inner.empty()) body.push_back(std::string(static_cast<std::size_t>(ctx.indent) * 4, ' ') + "pass");
        --ctx.indent;
        body.push_back("end");
        body.push_back("");
    }

    // 2) Узлы без событий (свободные цепочки) — исполняются при загрузке скрипта.
    // Узлы, не входящие ни в одну событийную цепочку (например, граф из одного
    // action.print без события) — исполняются при загрузке скрипта.
    for (const Node& n : g.nodes) {
        if (n.type.rfind("event.", 0) == 0) continue;
        if (ctx.emitted.count(n.id)) continue;      // уже скомпилирован в цепочке
        if (n.type.rfind("pure.", 0) == 0) continue; // чистые ноды кода не генерируют
        ctx.valueCache.clear();
        // chain живёт на протяжении ВСЕЙ цепочки от этого корня, поэтому
        // возврат в уже пройденный узел трактуется как цикл.
        ctx.chain.clear();
        std::vector<std::string> inner;
        emitExecNode(n.id, ctx, inner);
        for (auto& line : inner) body.push_back(line);
    }

    res.errors = ctx.errors;
    res.warnings = ctx.warnings;
    res.ok = ctx.errors.empty();

    std::ostringstream out;
    out << header.str();
    for (const std::string& line : body) out << line << "\n";
    res.code = out.str();
    res.lineCount = body.size() + 8;
    (void)anyEvent;
    return res;
}

// ---------------------------------------------------------------------------
//  Round-trip метаданных
// ---------------------------------------------------------------------------
Graph Transpiler::parseMetadata(const std::string& lvsSource) {
    Graph g;
    std::istringstream is(lvsSource);
    std::string line;
    while (std::getline(is, line)) {
        const auto at = line.find("#@node ");
        if (at == std::string::npos) continue;
        Node n;
        std::istringstream tokens(line.substr(at + 7));
        std::string tok;
        while (tokens >> tok) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = tok.substr(0, eq);
            const std::string val = tok.substr(eq + 1);
            if (key == "id") n.id = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
            else if (key == "type") n.type = val;
            else if (key == "title") n.title = val;
            else if (key == "pos") {
                const auto comma = val.find(',');
                if (comma != std::string::npos) {
                    n.x = static_cast<float>(std::atof(val.substr(0, comma).c_str()));
                    n.y = static_cast<float>(std::atof(val.substr(comma + 1).c_str()));
                }
            }
        }
        if (n.id != 0 || !n.type.empty()) g.nodes.push_back(std::move(n));
    }
    return g;
}

// ---------------------------------------------------------------------------
//  JSON <-> Graph
// ---------------------------------------------------------------------------
bool parseGraphJson(const std::string& text, Graph& out, std::string& err) {
    json::Value root;
    if (!json::parse(text, root, err)) return false;
    if (!root.isObject()) { err = "graph root must be an object"; return false; }

    out = Graph{};
    out.name = root["name"].asStringOr("VisualScript");

    for (const json::Value& jn : root["nodes"].asArray()) {
        Node n;
        n.id = static_cast<std::uint32_t>(jn["id"].asInt());
        n.type = jn["type"].asString();
        n.title = jn["title"].asString();
        n.x = static_cast<float>(jn["x"].asNumber());
        n.y = static_cast<float>(jn["y"].asNumber());
        n.comment = jn["comment"].asString();
        for (const auto& [k, v] : jn["props"].asObject()) n.props[k] = v.isString() ? v.asString() : v.dump();
        for (const json::Value& jp : jn["pins"].asArray()) {
            Pin p;
            p.id = jp["id"].asString();
            p.type = pinTypeFromName(jp["type"].asStringOr("any"));
            p.dir = jp["dir"].asStringOr("in") == "out" ? PinDir::Output : PinDir::Input;
            p.literal = jp["value"].isString() ? jp["value"].asString() : (jp["value"].isNull() ? std::string() : jp["value"].dump());
            p.displayName = jp["name"].asString();
            n.pins.push_back(std::move(p));
        }
        out.nodes.push_back(std::move(n));
    }

    for (const json::Value& jc : root["connections"].asArray()) {
        Connection c;
        c.fromNode = static_cast<std::uint32_t>(jc["from"].asInt());
        c.fromPin = jc["fromPin"].asString();
        c.toNode = static_cast<std::uint32_t>(jc["to"].asInt());
        c.toPin = jc["toPin"].asString();
        out.connections.push_back(c);
    }
    return true;
}

std::string serializeGraphJson(const Graph& g) {
    json::Object root;
    root["name"] = json::Value(g.name);

    json::Array nodes;
    for (const Node& n : g.nodes) {
        json::Object o;
        o["id"] = json::Value(static_cast<double>(n.id));
        o["type"] = json::Value(n.type);
        if (!n.title.empty()) o["title"] = json::Value(n.title);
        o["x"] = json::Value(static_cast<double>(n.x));
        o["y"] = json::Value(static_cast<double>(n.y));
        if (!n.comment.empty()) o["comment"] = json::Value(n.comment);
        json::Object props;
        for (const auto& [k, v] : n.props) props[k] = json::Value(v);
        if (!props.empty()) o["props"] = json::Value(std::move(props));
        json::Array pins;
        for (const Pin& p : n.pins) {
            json::Object po;
            po["id"] = json::Value(p.id);
            po["type"] = json::Value(std::string(pinTypeName(p.type)));
            po["dir"] = json::Value(std::string(p.dir == PinDir::Output ? "out" : "in"));
            if (!p.literal.empty()) po["value"] = json::Value(p.literal);
            if (!p.displayName.empty()) po["name"] = json::Value(p.displayName);
            pins.push_back(json::Value(std::move(po)));
        }
        if (!pins.empty()) o["pins"] = json::Value(std::move(pins));
        nodes.push_back(json::Value(std::move(o)));
    }
    root["nodes"] = json::Value(std::move(nodes));

    json::Array conns;
    for (const Connection& c : g.connections) {
        json::Object o;
        o["from"] = json::Value(static_cast<double>(c.fromNode));
        o["fromPin"] = json::Value(c.fromPin);
        o["to"] = json::Value(static_cast<double>(c.toNode));
        o["toPin"] = json::Value(c.toPin);
        conns.push_back(json::Value(std::move(o)));
    }
    root["connections"] = json::Value(std::move(conns));

    return json::Value(std::move(root)).dump(2);
}

} // namespace lv::vscript
