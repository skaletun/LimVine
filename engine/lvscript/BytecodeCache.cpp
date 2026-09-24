/**
 * @file    BytecodeCache.cpp
 * @brief   Реализация дискового кэша байткода (.lvc).
 */
#include "BytecodeCache.h"
#include "VM.h"

#include <fstream>
#include <system_error>

namespace lv {

namespace {

/// Прочитать файл целиком в буфер байтов; пустой вектор при любой ошибке.
std::vector<Byte> readAllBytes(const std::filesystem::path& p) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec || size == 0) return {};

    std::ifstream in(p, std::ios::binary);
    if (!in) return {};

    std::vector<Byte> data(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!in) return {};
    return data;
}

/// Записать буфер атомарно: сначала во временный файл, затем rename.
///
/// Без атомарности параллельная сборка (или прерывание по Ctrl+C) могла бы
/// оставить обрезанный .lvc, который при следующем запуске читался бы как
/// «валидный, но повреждённый» — десериализатор это переживёт, но лишняя
/// перекомпиляция с диагностикой в логе пользователю не нужна.
bool writeAllBytesAtomic(const std::filesystem::path& p, const std::vector<Byte>& data) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) return false;

    std::filesystem::path tmp = p;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) { std::filesystem::remove(tmp, ec); return false; }
    }

    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        // Некоторые ФС не умеют rename поверх существующего файла.
        std::filesystem::remove(p, ec);
        std::filesystem::rename(tmp, p, ec);
        if (ec) { std::filesystem::remove(tmp, ec); return false; }
    }
    return true;
}

/// Превратить путь исходника в безопасное относительное имя внутри кэша.
///
/// Абсолютные пути и `..` не должны выводить кэш за пределы каталога, иначе
/// сборка чужого проекта могла бы писать куда угодно.
std::filesystem::path sanitize(const std::filesystem::path& sourcePath) {
    std::filesystem::path out;
    for (const auto& part : sourcePath.lexically_normal()) {
        const std::string s = part.string();
        if (s.empty() || s == "." || s == ".." || s == "/" || s == "\\") continue;
        if (s.size() == 2 && s[1] == ':') continue;   // диск Windows (C:)
        out /= s;
    }
    return out;
}

} // namespace

BytecodeCache::BytecodeCache(std::filesystem::path cacheDir) : cacheDir_(std::move(cacheDir)) {}

std::filesystem::path BytecodeCache::pathFor(const std::filesystem::path& sourcePath) const {
    if (cacheDir_.empty()) return {};
    std::filesystem::path rel = sanitize(sourcePath);
    rel.replace_extension(".lvc");
    return cacheDir_ / rel;
}

std::optional<BytecodeModule> BytecodeCache::load(VM& vm,
                                                  const std::filesystem::path& sourcePath,
                                                  std::string_view sourceText) {
    if (!enabled()) { ++stats_.misses; return std::nullopt; }

    const std::filesystem::path cachePath = pathFor(sourcePath);
    std::error_code ec;
    if (cachePath.empty() || !std::filesystem::exists(cachePath, ec) || ec) {
        ++stats_.misses;
        return std::nullopt;
    }

    const std::vector<Byte> blob = readAllBytes(cachePath);
    if (blob.empty()) { ++stats_.stale; return std::nullopt; }

    // deserialize сам отвергает чужую версию формата и повреждённые данные.
    auto module = BytecodeModule::deserialize(vm, blob);
    if (!module) { ++stats_.stale; return std::nullopt; }

    // Содержимое исходника изменилось -> кэш устарел.
    if (module->sourceHash != BytecodeModule::sourceHashOf(sourceText)) {
        ++stats_.stale;
        return std::nullopt;
    }

    ++stats_.hits;
    return module;
}

bool BytecodeCache::store(VM& vm, const std::filesystem::path& sourcePath,
                          std::string_view sourceText, const BytecodeModule& module) {
    if (!enabled() || !module.entry) return false;

    BytecodeModule copy = module;
    copy.sourceHash = BytecodeModule::sourceHashOf(sourceText);
    if (copy.name.empty()) copy.name = sourcePath.string();

    const std::vector<Byte> blob = copy.serialize(vm);
    if (blob.empty()) { ++stats_.writeErrors; return false; }

    const std::filesystem::path cachePath = pathFor(sourcePath);
    if (cachePath.empty() || !writeAllBytesAtomic(cachePath, blob)) {
        ++stats_.writeErrors;
        return false;
    }
    ++stats_.writes;
    return true;
}

std::size_t BytecodeCache::clear() {
    if (cacheDir_.empty()) return 0;
    std::error_code ec;
    if (!std::filesystem::exists(cacheDir_, ec)) return 0;

    std::size_t removed = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(cacheDir_, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec) && it->path().extension() == ".lvc") {
            std::error_code rmEc;
            if (std::filesystem::remove(it->path(), rmEc)) ++removed;
        }
    }
    return removed;
}

} // namespace lv
