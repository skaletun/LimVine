/**
 * @file    Bytecode.cpp
 * @brief   (Де)сериализация байткода LV Script (.lvc) — кэш скомпилированных
 *          модулей на диске.
 *
 * Зачем
 * -----
 * Компиляция крупного проекта (десятки тысяч строк .lvs) при каждом запуске
 * редактора занимает заметное время. `.lvc` рядом с исходником позволяет
 * пропустить лексер/парсер/компилятор: файл читается, прототипы восстанавливаются
 * как есть, остаётся только исполнить верхний уровень модуля.
 *
 * Формат файла
 * ------------
 * @code
 *   "LVC1"                      4 байта, магическое число
 *   version                     uint32 LE  (kBytecodeVersion из Common.h)
 *   sourceHash                  uint64 LE  (FNV-1a от текста исходника)
 *   name                        varstring  (имя модуля)
 *   stringCount                 varuint
 *   strings[stringCount]        varstring  (таблица интернирования)
 *   protoCount                  varuint
 *   protos[protoCount]          Prototype  (индекс 0 — точка входа)
 * @endcode
 *
 * Prototype:
 * @code
 *   kind, arity, totalParams, restParamSlot,
 *   numUpvalues, numLocals, maxStack        varuint x7
 *   nameIndex                               varuint  (0xFFFFFFFF = без имени)
 *   codeLen + code[codeLen]                 varuint + сырые байты
 *   lineCount + lines[lineCount]            varuint x (1 + N)
 *   constCount + constants[constCount]      varuint + Constant
 *   nestedCount + nestedIdx[nestedCount]    varuint x (1 + N)
 * @endcode
 *
 * Constant (тег 1 байт):
 *   0 nil · 1 bool · 2 малое целое (varuint) · 3 double (8 байт LE)
 *   4 строка (индекс в таблице) · 5 прототип (индекс в таблице прототипов)
 *
 * Что НЕ сериализуется
 * --------------------
 * Классы, инстансы, замыкания, корутины, нативные функции — это рантайм-объекты.
 * В `.lvc` попадает байткод, который их СОЗДАЁТ; сами объекты появляются при
 * исполнении верхнего уровня модуля. Такая же схема работает при горячей
 * перезагрузке, поэтому поведение кэша и hot-reload совпадает.
 *
 * Числа пишутся varint'ом (LEB128), а `double` — сырыми 8 байтами в порядке
 * хоста. Кроссплатформенный обмен .lvc между разными endianness не
 * поддерживается намеренно: кэш всегда создаётся на машине, которая его читает,
 * а при несовпадении версии/хэша он просто перекомпилируется.
 */
#include "Bytecode.h"
#include "VM.h"
#include "Value.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace lv {

namespace {

constexpr char kMagic[4] = {'L', 'V', 'C', '1'};

/// Теги констант.
enum class ConstTag : Byte { Nil = 0, Bool = 1, SmallInt = 2, Double = 3, String = 4, Proto = 5 };

constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

// -- Запись -----------------------------------------------------------------
void putVarU32(std::vector<Byte>& out, std::uint32_t v) {
    while (v >= 0x80) { out.push_back(static_cast<Byte>(v | 0x80)); v >>= 7; }
    out.push_back(static_cast<Byte>(v));
}
void putRaw(std::vector<Byte>& out, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    out.insert(out.end(), p, p + len);
}
void putString(std::vector<Byte>& out, std::string_view s) {
    putVarU32(out, static_cast<std::uint32_t>(s.size()));
    putRaw(out, s.data(), s.size());
}

// -- Чтение -----------------------------------------------------------------
/// Курсор чтения с флагом ошибки: любое чтение за границей взводит @c bad.
struct Cursor {
    const Byte* p = nullptr;
    const Byte* end = nullptr;
    bool bad = false;

    [[nodiscard]] std::size_t remaining() const noexcept {
        return p < end ? static_cast<std::size_t>(end - p) : 0;
    }
    std::uint32_t varU32() {
        std::uint32_t v = 0; int shift = 0;
        for (;;) {
            if (p >= end || shift > 28) { bad = true; return 0; }
            const Byte b = *p++;
            v |= static_cast<std::uint32_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
            shift += 7;
        }
    }
    bool raw(void* dst, std::size_t len) {
        if (remaining() < len) { bad = true; return false; }
        std::memcpy(dst, p, len);
        p += len;
        return true;
    }
    std::string str() {
        const std::uint32_t len = varU32();
        if (bad || remaining() < len) { bad = true; return {}; }
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        return s;
    }
    Byte byte() {
        if (p >= end) { bad = true; return 0; }
        return *p++;
    }
};

/// FNV-1a 64 — тот же хэш, что использует AssetManager для контента.
std::uint64_t fnv1a(std::string_view data) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : data) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ULL;
    }
    return h;
}

/**
 * @brief Плоский индекс прототипов и строк модуля.
 *
 * Обход начинается с точки входа, поэтому она ВСЕГДА получает индекс 0 —
 * десериализатору не нужно хранить отдельное поле «какой прототип главный».
 */
struct ModuleIndex {
    std::vector<ObjFunction*> protos;
    std::unordered_map<const ObjFunction*, std::uint32_t> protoIds;
    std::vector<ObjString*> strings;
    std::unordered_map<const ObjString*, std::uint32_t> stringIds;

    std::uint32_t stringId(ObjString* s) {
        if (!s) return kNoIndex;
        auto [it, inserted] = stringIds.try_emplace(s, static_cast<std::uint32_t>(strings.size()));
        if (inserted) strings.push_back(s);
        return it->second;
    }
    std::uint32_t protoId(const ObjFunction* f) const {
        if (!f) return kNoIndex;
        auto it = protoIds.find(f);
        return it == protoIds.end() ? kNoIndex : it->second;
    }

    /// Обход в ширину: собирает все прототипы и все строки, на которые они ссылаются.
    void build(ObjFunction* entry) {
        if (!entry) return;
        std::vector<ObjFunction*> queue{entry};
        protoIds.emplace(entry, 0u);
        protos.push_back(entry);

        for (std::size_t i = 0; i < queue.size(); ++i) {
            ObjFunction* fn = queue[i];
            stringId(fn->name);                 // имя прототипа тоже попадает в таблицу

            auto visit = [&](ObjFunction* nested) {
                if (!nested) return;
                auto [it, inserted] = protoIds.try_emplace(nested,
                                                           static_cast<std::uint32_t>(protos.size()));
                if (inserted) { protos.push_back(nested); queue.push_back(nested); }
            };

            for (const Value& c : fn->constants) {
                if (!c.isObject()) continue;
                ObjHeader* h = c.asObject();
                if (h->type == ObjHeader::Type::String) stringId(static_cast<ObjString*>(h));
                else if (h->type == ObjHeader::Type::Function) visit(static_cast<ObjFunction*>(h));
            }
            for (ObjFunction* nested : fn->nested) visit(nested);
        }
    }
};

void writeConstant(std::vector<Byte>& out, const Value& v, ModuleIndex& idx) {
    if (v.isNil()) { out.push_back(static_cast<Byte>(ConstTag::Nil)); return; }
    if (v.isBool()) {
        out.push_back(static_cast<Byte>(ConstTag::Bool));
        out.push_back(v.asBool() ? 1 : 0);
        return;
    }
    if (v.isNumber()) {
        const double d = v.asNumber();
        const std::int64_t i = static_cast<std::int64_t>(d);
        // Компактный путь для неотрицательных целых, укладывающихся в 31 бит:
        // такие константы составляют подавляющее большинство в реальном коде.
        if (static_cast<double>(i) == d && i >= 0 && i < (1LL << 31)) {
            out.push_back(static_cast<Byte>(ConstTag::SmallInt));
            putVarU32(out, static_cast<std::uint32_t>(i));
        } else {
            out.push_back(static_cast<Byte>(ConstTag::Double));
            putRaw(out, &d, sizeof(double));
        }
        return;
    }
    if (v.isObject()) {
        ObjHeader* h = v.asObject();
        if (h->type == ObjHeader::Type::String) {
            out.push_back(static_cast<Byte>(ConstTag::String));
            putVarU32(out, idx.stringId(static_cast<ObjString*>(h)));
            return;
        }
        if (h->type == ObjHeader::Type::Function) {
            out.push_back(static_cast<Byte>(ConstTag::Proto));
            putVarU32(out, idx.protoId(static_cast<ObjFunction*>(h)));
            return;
        }
    }
    // Прочие рантайм-объекты в пуле констант не хранятся (их создаёт байткод).
    out.push_back(static_cast<Byte>(ConstTag::Nil));
}

void writeProto(std::vector<Byte>& out, const ObjFunction& fn, ModuleIndex& idx) {
    putVarU32(out, static_cast<std::uint32_t>(fn.kind));
    putVarU32(out, fn.arity);
    putVarU32(out, fn.totalParams);
    putVarU32(out, static_cast<std::uint32_t>(fn.restParamSlot + 1));   // -1 -> 0, сдвиг для varint
    putVarU32(out, fn.numUpvalues);
    putVarU32(out, fn.numLocals);
    putVarU32(out, fn.maxStack);
    putVarU32(out, idx.stringId(fn.name));

    putVarU32(out, static_cast<std::uint32_t>(fn.code.size()));
    putRaw(out, fn.code.data(), fn.code.size());

    putVarU32(out, static_cast<std::uint32_t>(fn.lineStarts.size()));
    for (std::uint32_t line : fn.lineStarts) putVarU32(out, line);

    putVarU32(out, static_cast<std::uint32_t>(fn.constants.size()));
    for (const Value& c : fn.constants) writeConstant(out, c, idx);

    putVarU32(out, static_cast<std::uint32_t>(fn.nested.size()));
    for (const ObjFunction* nested : fn.nested) putVarU32(out, idx.protoId(nested));
}

/// Сырые данные прототипа, прочитанные до создания ссылок.
struct RawProto {
    std::vector<std::pair<ConstTag, std::uint64_t>> constants;   // тег + полезная нагрузка
    std::vector<double> doubles;                                  // значения для ConstTag::Double
    std::vector<std::uint32_t> nested;
};

// ---------------------------------------------------------------------------
//  Переотображение имён
// ---------------------------------------------------------------------------
/**
 * @brief Как инструкция кодирует индекс имени в своём 24-битном операнде.
 *
 * Индексы имён — это позиции в @c VM::names(), которые НЕ совпадают между
 * разными экземплярами VM: порядок интернирования зависит от того, какие
 * модули были скомпилированы раньше. Поэтому при сериализации операнды
 * переводятся в индексы локальной таблицы модуля, а при загрузке — обратно
 * в индексы таблицы целевой VM.
 */
enum class NameSlot : std::uint8_t {
    None,      ///< имя не используется
    Full24,    ///< весь операнд — индекс имени
    Low12,     ///< младшие 12 бит (старшие 12 — индекс константы класса)
    High16     ///< старшие 16 бит (младшие 8 — число аргументов)
};

[[nodiscard]] constexpr NameSlot nameSlotOf(Op op) noexcept {
    switch (op) {
        case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal:
        case Op::GetModule: case Op::LoadName:  case Op::GetClass:
        case Op::MemberGet: case Op::MemberSet: case Op::MemberGetOpt:
            return NameSlot::Full24;
        case Op::ClassMethod: case Op::ClassField:
        case Op::StaticMethod: case Op::ClassSuper:
            return NameSlot::Low12;
        case Op::CallMethod: case Op::InvokeSuper:
            return NameSlot::High16;
        default:
            return NameSlot::None;
    }
}

/**
 * @brief Пройти по байткоду и применить @p remap ко всем индексам имён.
 *
 * Учитывает переменную длину @c Op::Closure (хвост: numUpvalues + пары по 4 байта).
 * @return false, если поток инструкций повреждён.
 */
template <class Fn>
bool remapNameOperands(std::vector<Byte>& code, Fn&& remap) {
    std::size_t ip = 0;
    while (ip + 4 <= code.size()) {
        const Op op = static_cast<Op>(code[ip]);
        const std::uint32_t operand = readOperand24(code.data() + ip + 1);
        const NameSlot slot = nameSlotOf(op);

        if (slot != NameSlot::None) {
            std::uint32_t updated = operand;
            switch (slot) {
                case NameSlot::Full24:
                    updated = remap(operand) & 0xFFFFFF;
                    break;
                case NameSlot::Low12: {
                    const std::uint32_t high = (operand >> 12) & 0xFFF;
                    const std::uint32_t name = remap(operand & 0xFFF) & 0xFFF;
                    updated = (high << 12) | name;
                    break;
                }
                case NameSlot::High16: {
                    const std::uint32_t argc = operand & 0xFF;
                    const std::uint32_t name = remap(operand >> 8) & 0xFFFF;
                    updated = (name << 8) | argc;
                    break;
                }
                case NameSlot::None:
                    break;
            }
            code[ip + 1] = static_cast<Byte>((updated >> 16) & 0xFF);
            code[ip + 2] = static_cast<Byte>((updated >> 8) & 0xFF);
            code[ip + 3] = static_cast<Byte>(updated & 0xFF);
        }

        if (op == Op::Closure) {
            if (ip + 4 >= code.size()) return false;
            const std::size_t numUp = code[ip + 4];
            ip += 4 + 1 + numUp * 4;
        } else {
            ip += 4;
        }
    }
    return ip == code.size() || ip >= code.size();
}

} // namespace

// ---------------------------------------------------------------------------
//  Сериализация
// ---------------------------------------------------------------------------
std::vector<Byte> BytecodeModule::serialize(VM& vm) const {
    std::vector<Byte> out;
    if (!entry) return out;
    out.reserve(4096);

    // Проход 1: индексируем прототипы и строки (entry гарантированно получает id 0).
    ModuleIndex idx;
    idx.build(entry);
    // Имя модуля тоже кладём в таблицу строк, чтобы не плодить форматы.
    idx.stringId(vm.internRaw(name.empty() ? entry->source : name));

    // Проход 2: собираем ТАБЛИЦУ ИМЁН МОДУЛЯ и переписываем операнды.
    //
    // В байткоде имена закодированы как индексы в VM::names() той VM, которая
    // компилировала модуль. В другой VM (или в этой же, но после загрузки
    // других модулей) те же индексы указывали бы на чужие имена — раньше это
    // приводило к обращению по неверному имени и падению сразу на первой
    // инструкции DefineGlobal. Поэтому в файл пишется собственная таблица
    // имён модуля, а операнды переводятся в её индексы.
    const std::vector<ObjString*>& vmNames = vm.names();
    std::vector<std::string> moduleNames;
    std::unordered_map<std::uint32_t, std::uint32_t> vmToModule;

    auto toModuleIndex = [&](std::uint32_t vmIndex) -> std::uint32_t {
        auto it = vmToModule.find(vmIndex);
        if (it != vmToModule.end()) return it->second;
        const auto moduleIndex = static_cast<std::uint32_t>(moduleNames.size());
        moduleNames.emplace_back(vmIndex < vmNames.size() && vmNames[vmIndex]
                                     ? std::string(vmNames[vmIndex]->view())
                                     : std::string());
        vmToModule.emplace(vmIndex, moduleIndex);
        return moduleIndex;
    };

    // Байткод переписываем в КОПИЯХ: исходные прототипы принадлежат живой VM
    // и продолжают исполняться.
    std::vector<std::vector<Byte>> rewritten;
    rewritten.reserve(idx.protos.size());
    for (ObjFunction* fn : idx.protos) {
        rewritten.push_back(fn->code);
        remapNameOperands(rewritten.back(), toModuleIndex);
    }

    // Проход 3: тела прототипов во временный буфер (таблицы идут в файле раньше,
    // но writeConstant может дописать в них новые строки).
    std::vector<Byte> body;
    body.reserve(4096);
    const std::size_t protoCount = idx.protos.size();
    for (std::size_t i = 0; i < protoCount; ++i) {
        ObjFunction& fn = *idx.protos[i];
        // Временно подменяем код на переотображённый — writeProto пишет fn.code.
        std::vector<Byte> original;
        original.swap(fn.code);
        fn.code = rewritten[i];
        writeProto(body, fn, idx);
        fn.code.swap(original);   // возвращаем прототипу его рабочий байткод
    }

    // Заголовок.
    putRaw(out, kMagic, 4);
    const std::uint32_t ver = kBytecodeVersion;
    putRaw(out, &ver, sizeof(ver));
    const std::uint64_t hash = sourceHash;
    putRaw(out, &hash, sizeof(hash));
    putString(out, name.empty() ? entry->source : name);

    // Таблица имён модуля.
    putVarU32(out, static_cast<std::uint32_t>(moduleNames.size()));
    for (const std::string& n : moduleNames) putString(out, n);

    // Таблица строковых констант.
    putVarU32(out, static_cast<std::uint32_t>(idx.strings.size()));
    for (ObjString* s : idx.strings) putString(out, s ? s->view() : std::string_view{});

    // Прототипы.
    putVarU32(out, static_cast<std::uint32_t>(protoCount));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// ---------------------------------------------------------------------------
//  Десериализация
// ---------------------------------------------------------------------------
std::optional<BytecodeModule> BytecodeModule::deserialize(VM& vm, std::span<const Byte> data) {
    constexpr std::size_t kHeaderMin = 4 + sizeof(std::uint32_t) + sizeof(std::uint64_t);
    if (data.size() < kHeaderMin) return std::nullopt;

    Cursor cur{data.data(), data.data() + data.size(), false};

    char magic[4];
    if (!cur.raw(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return std::nullopt;

    std::uint32_t version = 0;
    if (!cur.raw(&version, sizeof(version))) return std::nullopt;
    if (version != kBytecodeVersion) return std::nullopt;   // кэш устарел -> перекомпиляция

    std::uint64_t hash = 0;
    if (!cur.raw(&hash, sizeof(hash))) return std::nullopt;

    const std::string moduleName = cur.str();
    if (cur.bad) return std::nullopt;

    // Таблица имён модуля -> индексы в таблице имён ЭТОЙ VM.
    const std::uint32_t nameCount = cur.varU32();
    if (cur.bad || nameCount > (1u << 22)) return std::nullopt;
    std::vector<std::uint32_t> nameRemap;
    nameRemap.reserve(nameCount);
    for (std::uint32_t i = 0; i < nameCount; ++i) {
        const std::string n = cur.str();
        if (cur.bad) return std::nullopt;
        nameRemap.push_back(vm.internName(n));
    }

    // Таблица строк.
    const std::uint32_t stringCount = cur.varU32();
    if (cur.bad || stringCount > (1u << 22)) return std::nullopt;
    std::vector<ObjString*> strings;
    strings.reserve(stringCount);
    for (std::uint32_t i = 0; i < stringCount; ++i) {
        const std::string s = cur.str();
        if (cur.bad) return std::nullopt;
        ObjString* interned = vm.internRaw(s);
        // Пока строка лежит только в этом векторе, она недостижима для GC;
        // аллокация следующей строки могла бы её собрать.
        if (interned) GC::pin(interned);
        strings.push_back(interned);
    }

    // Прототипы: сначала создаём ВСЕ объекты (чтобы ссылки друг на друга
    // разрешались вперёд), затем заполняем константы и вложенные списки.
    const std::uint32_t protoCount = cur.varU32();
    if (cur.bad || protoCount == 0 || protoCount > (1u << 20)) return std::nullopt;

    // ВАЖНО: свежесозданные прототипы не достижимы ни из одного корня GC
    // (они ещё не записаны ни в глобалы, ни в стек VM), а каждая следующая
    // аллокация может запустить сборку. Поэтому закрепляем их сразу: без
    // этого загрузка модуля из .lvc падала, как только таблица прототипов
    // перешагивала порог сборки.
    std::vector<ObjFunction*> protos(protoCount, nullptr);
    for (std::uint32_t i = 0; i < protoCount; ++i) {
        protos[i] = vm.gc().allocate<ObjFunction>(
            sizeof(ObjFunction), ObjHeader(ObjHeader::Type::Function, sizeof(ObjFunction)));
        if (!protos[i]) return std::nullopt;
        GC::pin(protos[i]);
    }

    auto stringAt = [&](std::uint32_t i) -> ObjString* {
        return i < strings.size() ? strings[i] : nullptr;
    };

    std::vector<RawProto> raws(protoCount);

    for (std::uint32_t i = 0; i < protoCount; ++i) {
        ObjFunction& fn = *protos[i];
        fn.kind          = static_cast<ObjFunction::Kind>(cur.varU32());
        fn.arity         = cur.varU32();
        fn.totalParams   = cur.varU32();
        fn.restParamSlot = static_cast<std::int32_t>(cur.varU32()) - 1;
        fn.numUpvalues   = cur.varU32();
        fn.numLocals     = cur.varU32();
        fn.maxStack      = cur.varU32();
        fn.name          = stringAt(cur.varU32());
        fn.source        = moduleName;
        if (cur.bad) return std::nullopt;

        const std::uint32_t codeLen = cur.varU32();
        if (cur.bad || cur.remaining() < codeLen) return std::nullopt;
        fn.code.resize(codeLen);
        if (codeLen && !cur.raw(fn.code.data(), codeLen)) return std::nullopt;

        // Индексы имён из файла — локальные для модуля; переводим их в индексы
        // таблицы имён этой VM (см. комментарий в serialize()).
        if (!remapNameOperands(fn.code, [&](std::uint32_t moduleIndex) -> std::uint32_t {
                return moduleIndex < nameRemap.size() ? nameRemap[moduleIndex] : 0u;
            }))
            return std::nullopt;

        const std::uint32_t lineCount = cur.varU32();
        if (cur.bad || lineCount > codeLen + 64) return std::nullopt;   // sanity
        fn.lineStarts.resize(lineCount);
        for (std::uint32_t k = 0; k < lineCount; ++k) fn.lineStarts[k] = cur.varU32();
        if (cur.bad) return std::nullopt;

        const std::uint32_t constCount = cur.varU32();
        if (cur.bad || constCount > (1u << 20)) return std::nullopt;
        RawProto& raw = raws[i];
        raw.constants.reserve(constCount);
        for (std::uint32_t k = 0; k < constCount; ++k) {
            const auto tag = static_cast<ConstTag>(cur.byte());
            if (cur.bad) return std::nullopt;
            switch (tag) {
                case ConstTag::Nil:
                    raw.constants.emplace_back(tag, 0);
                    break;
                case ConstTag::Bool:
                    raw.constants.emplace_back(tag, cur.byte() != 0 ? 1u : 0u);
                    break;
                case ConstTag::SmallInt:
                    raw.constants.emplace_back(tag, cur.varU32());
                    break;
                case ConstTag::Double: {
                    double d = 0;
                    if (!cur.raw(&d, sizeof(double))) return std::nullopt;
                    raw.constants.emplace_back(tag, raw.doubles.size());
                    raw.doubles.push_back(d);
                    break;
                }
                case ConstTag::String:
                case ConstTag::Proto:
                    raw.constants.emplace_back(tag, cur.varU32());
                    break;
                default:
                    return std::nullopt;   // неизвестный тег — файл повреждён
            }
            if (cur.bad) return std::nullopt;
        }

        const std::uint32_t nestedCount = cur.varU32();
        if (cur.bad || nestedCount > protoCount) return std::nullopt;
        raw.nested.resize(nestedCount);
        for (std::uint32_t k = 0; k < nestedCount; ++k) raw.nested[k] = cur.varU32();
        if (cur.bad) return std::nullopt;
    }

    // Второй проход: разрешаем ссылки на строки и прототипы.
    for (std::uint32_t i = 0; i < protoCount; ++i) {
        ObjFunction& fn = *protos[i];
        const RawProto& raw = raws[i];

        fn.constants.clear();
        fn.constants.reserve(raw.constants.size());
        for (const auto& [tag, payload] : raw.constants) {
            switch (tag) {
                case ConstTag::Nil:
                    fn.constants.push_back(Value::nil());
                    break;
                case ConstTag::Bool:
                    fn.constants.push_back(Value::boolean(payload != 0));
                    break;
                case ConstTag::SmallInt:
                    fn.constants.push_back(Value::fromNumber(static_cast<double>(payload)));
                    break;
                case ConstTag::Double:
                    fn.constants.push_back(Value::fromNumber(raw.doubles[payload]));
                    break;
                case ConstTag::String: {
                    ObjString* s = stringAt(static_cast<std::uint32_t>(payload));
                    fn.constants.push_back(s ? Value::object(s) : Value::nil());
                    break;
                }
                case ConstTag::Proto: {
                    const auto id = static_cast<std::uint32_t>(payload);
                    fn.constants.push_back(id < protoCount ? Value::object(protos[id]) : Value::nil());
                    break;
                }
            }
        }

        fn.nested.clear();
        fn.nested.reserve(raw.nested.size());
        for (std::uint32_t id : raw.nested)
            if (id < protoCount) fn.nested.push_back(protos[id]);
    }

    BytecodeModule m;
    m.version    = version;
    m.sourceHash = hash;
    m.name       = moduleName;
    m.entry      = protos[0];
    m.protos     = std::move(protos);
    return m;
}

std::uint64_t BytecodeModule::sourceHashOf(std::string_view src) { return fnv1a(src); }

} // namespace lv
