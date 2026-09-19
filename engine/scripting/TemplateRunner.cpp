/**
 * @file    TemplateRunner.cpp
 * @brief   Реализация headless-запуска игровых шаблонов.
 */
#include "TemplateRunner.h"
#include "visual/JsonMini.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace lv::scripting {

std::unordered_map<std::string, std::string> valueToMap(const lv::Value& v) {
    std::unordered_map<std::string, std::string> out;
    if (!v.isObject() || v.asObject()->type != lv::ObjHeader::Type::Map) return out;
    const auto* m = static_cast<const lv::ObjMap*>(v.asObject());
    for (std::uint32_t i = 0; i < m->capacity; ++i) {
        const lv::MapEntry& e = m->entries[i];
        if (!e.used) continue;
        out.emplace(e.key.toString(), e.value.toString());
    }
    return out;
}

std::string readTextFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TemplateRunner::TemplateRunner() { buildContext(); }
TemplateRunner::~TemplateRunner() = default;

void TemplateRunner::buildContext() {
    // Компоненты регистрируются в BindingRegistry один раз на процесс; повторная
    // регистрация безопасна (перезаписывает запись тем же TypeId).
    registerEngineComponents();

    physics_.init(4096);
    physics::attachToWorld(physics_, world_);

    audio_.init(24, /*forceNull=*/true);   // headless: Null-бэкенд с полным пулом голосов
    audio::attachToWorld(audio_, world_);

    render::SurfaceDesc surface;
    renderer_.init(render::Backend::Null, surface);
    renderer_.attachToWorld(world_);

    ctx_.world    = &world_;
    ctx_.physics  = &physics_;
    ctx_.input    = &input_;
    ctx_.audio    = &audio_;
    ctx_.renderer = &renderer_;

    scripts_.attach(ctx_);
}

void TemplateRunner::setLogCallback(ScriptWorld::LogCallback cb) { scripts_.setLogCallback(std::move(cb)); }

TemplateLoadResult TemplateRunner::load(const std::filesystem::path& dir, std::vector<std::string>* diags) {
    loaded_ = {};
    auto report = [&](std::string msg) {
        loaded_.errors.push_back(msg);
        if (diags) diags->push_back(std::move(msg));
    };

    // ------------------------------------------------------------ Манифест
    const std::filesystem::path manifestPath = dir / "template.json";
    const std::string manifestText = readTextFile(manifestPath);
    if (manifestText.empty()) {
        report("cannot read " + manifestPath.string());
        return loaded_;
    }
    json::Value manifest;
    std::string jsonErr;
    if (!json::parse(manifestText, manifest, jsonErr)) {
        report("template.json parse error: " + jsonErr);
        return loaded_;
    }
    loaded_.id   = manifest["id"].asStringOr(dir.filename().string());
    loaded_.name = manifest["name"].asStringOr(loaded_.id);

    // ------------------------------------------------------- Input contexts
    // Шаблон объявляет нужные ему контексты ввода; создаём их здесь, чтобы
    // скрипты могли делать pushInputContext("Farm") без знания о раскладке.
    if (manifest.contains("input")) {
        const json::Value& in = manifest["input"];
        std::vector<std::string> contextNames;
        if (in.contains("contexts"))
            for (const json::Value& c : in["contexts"].asArray()) contextNames.push_back(c.asStringOr(""));
        if (in.contains("actions"))
            for (const json::Value& a : in["actions"].asArray()) {
                const std::string act = a.asStringOr("");
                if (!act.empty()) contextNames.push_back("action:" + act);
            }
        for (const std::string& cn : contextNames) {
            if (cn.rfind("action:", 0) == 0) continue;   // действия описывает сам шаблон
            input_.createContext(cn, 0);
            input_.pushContext(cn);
        }
    }

    // ------------------------------------------------------------- Сборка
    std::string src;
    auto appendFile = [&](const std::filesystem::path& file, const std::string& label) -> bool {
        const std::string text = readTextFile(file);
        if (text.empty()) { report("cannot read " + file.string()); return false; }
        // Маркер занимает РОВНО одну строку: номера строк в диагностике и
        // трейсбеках продолжают совпадать с исходным файлом.
        src += "# ---- file " + label + " ----\n";
        src += text;
        if (!text.empty() && text.back() != '\n') src += "\n";
        return true;
    };

    if (manifest.contains("stdlib"))
        for (const json::Value& m : manifest["stdlib"].asArray()) {
            const std::string mod = m.asStringOr("");
            if (mod.empty()) continue;
            if (!appendFile(dir / "../../" / "stdlib" / (mod + ".lvs"), "stdlib/" + mod + ".lvs")) continue;
            loaded_.stdlib.push_back(mod);
        }

    if (manifest.contains("scripts"))
        for (const json::Value& s : manifest["scripts"].asArray()) {
            const std::string rel = s.asStringOr("");
            if (rel.empty()) continue;
            if (!appendFile(dir / rel, rel)) continue;
            loaded_.scripts.push_back(rel);
        }

    if (!loaded_.errors.empty()) return loaded_;
    loaded_.source = src;

    // Исполняем. Диагностика уже выведена через logCb_ внутри ScriptWorld.
    const std::string moduleName = "<" + loaded_.id + ">";
    loaded_.ok = scripts_.runFile(moduleName, src);
    loaded_.moduleName = moduleName;
    if (!loaded_.ok) report("template script failed to run: " + loaded_.id);
    return loaded_;
}

void TemplateRunner::tick(float dt) {
    world_.tick(dt);
    scripts_.tick(dt);
}

void TemplateRunner::runFrames(int frames, float dt) {
    for (int i = 0; i < frames; ++i) tick(dt);
}

lv::ObjFunction* TemplateRunner::prototype() const noexcept {
    return scripts_.prototype(loaded_.moduleName);
}

bool TemplateRunner::callGlobalFn(const std::string& fnName, const std::vector<lv::Value>& args, lv::Value* out) {
    lv::Value* fn = scripts_.vm().findGlobal(fnName);
    if (!fn || !fn->isObject()) return false;
    return scripts_.vm().callValue(*fn, std::span<const lv::Value>(args.data(), args.size()), out)
           == lv::RunStatus::Ok;
}

bool TemplateRunner::callMethodOnGlobal(const std::string& globalInstance, const std::string& method,
                                        const std::vector<lv::Value>& args, lv::Value* out) {
    lv::Value* inst = scripts_.vm().findGlobal(globalInstance);
    if (!inst || !inst->isObject()) return false;
    // VM::callMethod сам раскладывает стек как `[receiver][args][callee]` и
    // устанавливает thisValue кадра, поэтому здесь не нужно ничего, кроме
    // передачи приёмника.
    return scripts_.vm().callMethod(*inst, method,
                                    std::span<const lv::Value>(args.data(), args.size()), out)
           == lv::RunStatus::Ok;
}

bool TemplateRunner::callGlobal(const std::string& fnName) {
    lv::Value* fn = scripts_.vm().findGlobal(fnName);
    if (!fn || !fn->isObject()) return false;
    return scripts_.vm().callValue(*fn, {}) == lv::RunStatus::Ok;
}

} // namespace lv::scripting
