/**
 * @file    lvcook.cpp
 * @brief   «Повар» контента LimVine: проверка и упаковка .lvs / шаблонов.
 *
 * Зачем: шаблон игры — это манифест + несколько .lvs + модули stdlib. Ошибка в
 * любом из файлов всплывает только в рантайме, поэтому в CI нужен инструмент,
 * который ПРОГОНЯЕТ ВЕСЬ КОНТЕНТ через парсер и компилятор и возвращает
 * ненулевой код при первой же диагностике уровня Error.
 *
 * Использование:
 *   lvcook check                       # проверить stdlib/ и templates/
 *   lvcook check templates/rpg_3p      # проверить один шаблон
 *   lvcook bundle templates/farming_iso -o /tmp/farming.lvs
 *                                      # собрать шаблон в один файл (дистрибуция)
 *   lvcook list                        # список шаблонов и их зависимостей
 *
 * Примечание о кэше байткода (.lvc): контейнер BytecodeModule объявлен в
 * engine/lvscript/Bytecode.h, но его serialize/deserialize ещё не реализованы,
 * поэтому lvcook пока НЕ пишет .lvc — он лишь проверяет, что модуль
 * компилируется, и (для bundle) склеивает исходники в порядке загрузки.
 */
#include "lvscript/Bytecode.h"
#include "lvscript/Compiler.h"
#include "lvscript/Parser.h"
#include "lvscript/Stdlib.h"
#include "lvscript/VM.h"
#include "scripting/visual/JsonMini.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_errors = 0;
int g_warnings = 0;
int g_files = 0;

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Скомпилировать один исходник; вернуть true при успехе.
bool compileOne(const std::string& label, const std::string& source, bool quiet) {
    lv::VM vm;                     // своя VM: интернированные имена не должны протекать между файлами
    lv::installStdlib(vm);

    lv::DiagnosticList diags;
    lv::Module mod = lv::parseSource(source, label, diags);
    lv::ObjFunction* fn = nullptr;
    if (!std::any_of(diags.begin(), diags.end(),
                     [](const lv::Diagnostic& d) { return d.severity == lv::DiagSeverity::Error; })) {
        lv::Compiler c(vm);
        fn = c.compile(mod, diags);
    }

    bool ok = fn != nullptr;
    for (const lv::Diagnostic& d : diags) {
        if (d.severity == lv::DiagSeverity::Error) { ++g_errors; ok = false; }
        else ++g_warnings;
        if (!quiet || d.severity == lv::DiagSeverity::Error)
            std::printf("  %s\n", d.toString().c_str());
    }
    ++g_files;
    return ok;
}

/// Собрать содержимое шаблона в один исходник (тот же порядок, что TemplateRunner).
bool buildBundle(const fs::path& dir, std::string& out, std::string& err) {
    const std::string manifest = readAll(dir / "template.json");
    if (manifest.empty()) { err = "cannot read " + (dir / "template.json").string(); return false; }

    lv::json::Value jv;
    std::string jerr;
    if (!lv::json::parse(manifest, jv, jerr)) { err = "template.json: " + jerr; return false; }

    out.clear();
    out += "# ==== LimVine bundle: " + jv["id"].asStringOr(dir.filename().string()) + " ====\n";
    out += "# Собран lvcook; порядок файлов соответствует template.json.\n\n";

    if (jv.contains("stdlib"))
        for (const lv::json::Value& m : jv["stdlib"].asArray()) {
            const std::string name = m.asStringOr("");
            if (name.empty()) continue;
            const fs::path p = fs::path("stdlib") / (name + ".lvs");
            const std::string text = readAll(p);
            if (text.empty()) { err = "cannot read " + p.string(); return false; }
            out += "# ---- file stdlib/" + name + ".lvs ----\n" + text;
            if (text.back() != '\n') out += "\n";
            out += "\n";
        }

    if (jv.contains("scripts"))
        for (const lv::json::Value& s : jv["scripts"].asArray()) {
            const std::string rel = s.asStringOr("");
            if (rel.empty()) continue;
            const fs::path p = dir / rel;
            const std::string text = readAll(p);
            if (text.empty()) { err = "cannot read " + p.string(); return false; }
            out += "# ---- file " + rel + " ----\n" + text;
            if (text.back() != '\n') out += "\n";
            out += "\n";
        }
    return true;
}

bool checkTemplate(const fs::path& dir, bool quiet) {
    std::string bundle, err;
    if (!buildBundle(dir, bundle, err)) {
        std::printf("[FAIL] %s: %s\n", dir.string().c_str(), err.c_str());
        ++g_errors;
        return false;
    }
    const bool ok = compileOne("<" + dir.filename().string() + ">", bundle, quiet);
    std::printf("[%s] %s (%zu bytes)\n", ok ? " ok " : "FAIL", dir.string().c_str(), bundle.size());
    return ok;
}

int cmdCheck(const std::vector<std::string>& args, bool quiet) {
    // Явная цель (шаблон или файл) или всё содержимое репозитория.
    if (!args.empty()) {
        for (const std::string& a : args) {
            const fs::path p(a);
            if (!fs::exists(p)) { std::printf("[FAIL] %s: not found\n", a.c_str()); ++g_errors; continue; }
            if (fs::is_directory(p)) checkTemplate(p, quiet);
            else compileOne(a, readAll(p), quiet);
        }
    } else {
        for (const auto& e : fs::directory_iterator("stdlib"))
            if (e.path().extension() == ".lvs") {
                std::printf("--- %s\n", e.path().string().c_str());
                compileOne(e.path().string(), readAll(e.path()), quiet);
            }
        if (fs::exists("templates"))
            for (const auto& e : fs::directory_iterator("templates"))
                if (e.is_directory()) checkTemplate(e.path(), quiet);
    }
    std::printf("\nlvcook: %d file(s), %d error(s), %d warning(s)\n", g_files, g_errors, g_warnings);
    return g_errors == 0 ? 0 : 1;
}

int cmdBundle(const std::vector<std::string>& args) {
    if (args.empty()) { std::fprintf(stderr, "usage: lvcook bundle <template-dir> [-o out.lvs]\n"); return 2; }
    const fs::path dir(args[0]);
    std::string outPath = (dir.filename().string() + ".bundle.lvs");
    for (std::size_t i = 1; i < args.size(); ++i)
        if (args[i] == "-o" && i + 1 < args.size()) outPath = args[++i];

    std::string bundle, err;
    if (!buildBundle(dir, bundle, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (!compileOne("<" + dir.filename().string() + ">", bundle, false)) {
        std::fprintf(stderr, "bundle does not compile; nothing written\n");
        return 1;
    }
    std::ofstream f(outPath, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    f << bundle;
    std::printf("written %s (%zu bytes)\n", outPath.c_str(), bundle.size());
    return 0;
}

int cmdList() {
    if (!fs::exists("templates")) { std::printf("no templates/\n"); return 0; }
    for (const auto& e : fs::directory_iterator("templates")) {
        if (!e.is_directory()) continue;
        const std::string manifest = readAll(e.path() / "template.json");
        if (manifest.empty()) { std::printf("%-16s (no template.json)\n", e.path().filename().string().c_str()); continue; }
        lv::json::Value jv; std::string jerr;
        if (!lv::json::parse(manifest, jv, jerr)) { std::printf("%-16s (bad manifest: %s)\n", e.path().filename().string().c_str(), jerr.c_str()); continue; }
        std::printf("%-16s %s\n", jv["id"].asStringOr(e.path().filename().string()).c_str(),
                    jv["name"].asStringOr("").c_str());
        std::printf("%-16s   summary: %s\n", "", jv["summary"].asStringOr("").c_str());
        std::printf("%-16s   stdlib :", "");
        if (jv.contains("stdlib")) for (const lv::json::Value& m : jv["stdlib"].asArray()) std::printf(" %s", m.asStringOr("").c_str());
        std::printf("\n%-16s   scripts:", "");
        if (jv.contains("scripts")) for (const lv::json::Value& s : jv["scripts"].asArray()) std::printf(" %s", fs::path(s.asStringOr("")).filename().string().c_str());
        std::printf("\n");
    }
    return 0;
}

void usage() {
    std::printf(
        "LimVine content cooker\n"
        "  lvcook check  [path...]              проверить .lvs / шаблоны (по умолчанию всё)\n"
        "  lvcook bundle <template-dir> [-o f]  собрать шаблон в один .lvs\n"
        "  lvcook list                          список шаблонов и их зависимостей\n"
        "\n"
        "  --quiet    печатать только ошибки\n"
        "Код возврата: 0 — ошибок нет, 1 — есть ошибки компиляции/чтения.\n");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    std::string cmd;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--quiet") quiet = true;
        else if (cmd.empty()) cmd = a;
        else args.push_back(a);
    }
    if (cmd.empty()) { usage(); return 2; }
    if (cmd == "check")  return cmdCheck(args, quiet);
    if (cmd == "bundle") return cmdBundle(args);
    if (cmd == "list")   return cmdList();
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    usage();
    return 2;
}
