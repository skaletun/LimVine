/**
 * @file    lvrun.cpp
 * @brief   CLI-раннер шаблонов и отдельных .lvs-файлов LimVine.
 *
 * Использование:
 *   lvrun templates/farming_iso --frames 600          # прогнать шаблон headless
 *   lvrun templates/rpg_3p --frames 60 --disasm       # + дизассемблер байткода
 *   lvrun script.lvs --frames 0                       # просто выполнить файл
 *
 * Зачем: шаблоны должны запускаться и в CI, и вручную, без редактора и GPU.
 * Раннер собирает тот же «мини-движок», что и интеграционные тесты
 * (World + Physics + Input + Null-Audio + Null-Renderer), поэтому поведение
 * скриптов идентично игровому.
 */
#include "scripting/TemplateRunner.h"
#include "lvscript/Bytecode.h"
#include "lvscript/Compiler.h"
#include "lvscript/Parser.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string target;
    int frames = 120;
    float dt = 1.0f / 60.0f;
    bool disasm = false;
    bool quiet = false;
    bool withInputDemo = false;
};

void usage() {
    std::printf(
        "LimVine runner\n"
        "  lvrun <template-dir|file.lvs> [options]\n"
        "\n"
        "Options:\n"
        "  --frames N     число кадров симуляции (по умолчанию 120)\n"
        "  --dt F         шаг кадра в секундах (по умолчанию 1/60)\n"
        "  --disasm       напечатать дизассемблированный байткод модуля\n"
        "  --input        подать демонстрационный ввод (ось MoveX = +1 первые 30 кадров)\n"
        "  --quiet        не печатать вывод скриптов\n"
        "  -h, --help     эта справка\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return false; }
        else if (a == "--disasm") o.disasm = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--input") o.withInputDemo = true;
        else if (a == "--frames" && i + 1 < argc) o.frames = std::atoi(argv[++i]);
        else if (a == "--dt" && i + 1 < argc) o.dt = static_cast<float>(std::atof(argv[++i]));
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return false; }
        else if (o.target.empty()) o.target = a;
        else { std::fprintf(stderr, "unexpected argument: %s\n", a.c_str()); return false; }
    }
    if (o.target.empty()) { usage(); return false; }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;

    lv::JobSystem::init(0);
    lv::scripting::TemplateRunner runner;
    if (!opt.quiet)
        runner.setLogCallback([](const std::string& s) { std::printf("%s\n", s.c_str()); });

    const std::filesystem::path target(opt.target);
    const bool isDir = std::filesystem::exists(target) && std::filesystem::is_directory(target);

    if (isDir) {
        std::vector<std::string> diags;
        const auto res = runner.load(target, &diags);
        for (const auto& d : diags) std::fprintf(stderr, "%s\n", d.c_str());
        if (!res.ok) {
            std::fprintf(stderr, "failed to load template '%s'\n", opt.target.c_str());
            return 1;
        }
        if (!opt.quiet) {
            std::printf("# template %s (%s)\n", res.name.c_str(), res.id.c_str());
            std::printf("#   stdlib : ");
            for (const auto& m : res.stdlib) std::printf("%s ", m.c_str());
            std::printf("\n#   scripts: ");
            for (const auto& s : res.scripts) std::printf("%s ", s.c_str());
            std::printf("\n");
        }
        if (opt.disasm) {
            if (lv::ObjFunction* fn = runner.prototype())
                std::printf("%s", lv::Disassembler::disassemble(*fn).c_str());
        }
    } else {
        const std::string src = lv::scripting::readTextFile(target);
        if (src.empty()) { std::fprintf(stderr, "cannot read %s\n", opt.target.c_str()); return 1; }
        if (opt.disasm) {
            lv::DiagnosticList diags;
            lv::Module mod = lv::parseSource(src, opt.target, diags);
            lv::Compiler comp(runner.scripts().vm());
            if (lv::ObjFunction* fn = comp.compile(mod, diags))
                std::printf("%s", lv::Disassembler::disassemble(*fn).c_str());
        }
        if (!runner.scripts().runFile(opt.target, src)) {
            std::fprintf(stderr, "script failed\n");
            return 1;
        }
    }

    for (int i = 0; i < opt.frames; ++i) {
        // Демонстрационный ввод: удерживаем «вправо» первые полсекунды, чтобы
        // шаблон игрока реально сдвинулся — это видно по getPosition в логе.
        if (opt.withInputDemo) {
            runner.input().beginFrame();
            if (i < 30) runner.input().onKey(68, true);   // D
            else        runner.input().onKey(68, false);
        }
        runner.tick(opt.dt);
    }

    if (!opt.quiet)
        std::printf("# done: %d frames, entities=%zu\n", opt.frames, runner.world().entityCount());
    return 0;
}
