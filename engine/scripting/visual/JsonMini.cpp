/**
 * @file    JsonMini.cpp
 * @brief   Реализация минимального JSON.
 */
#include "JsonMini.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lv::json {

namespace {
const Value kNull{};
const Array kEmptyArray{};
const Object kEmptyObject{};
} // namespace

const std::string& Value::asString() const {
    static const std::string empty;
    return isString() ? std::get<std::string>(data_) : empty;
}
const Array& Value::asArray() const { return isArray() ? std::get<Array>(data_) : kEmptyArray; }
const Object& Value::asObject() const { return isObject() ? std::get<Object>(data_) : kEmptyObject; }

const Value& Value::operator[](std::string_view key) const {
    if (!isObject()) return kNull;
    const auto& o = std::get<Object>(data_);
    auto it = o.find(std::string(key));
    return it == o.end() ? kNull : it->second;
}

const Value& Value::operator[](std::size_t index) const {
    if (!isArray()) return kNull;
    const auto& a = std::get<Array>(data_);
    return index < a.size() ? a[index] : kNull;
}

bool Value::contains(std::string_view key) const {
    if (!isObject()) return false;
    return std::get<Object>(data_).count(std::string(key)) != 0;
}

Value& Value::set(std::string key, Value v) {
    if (!isObject()) data_ = Object{};
    return std::get<Object>(data_)[std::move(key)] = std::move(v);
}

static void escapeInto(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    out += '"';
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    const std::string pad = indent >= 0 ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();
    const std::string padNext = indent >= 0 ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
    const char* nl = indent >= 0 ? "\n" : "";

    if (isNull()) { out += "null"; return; }
    if (isBool()) { out += std::get<bool>(data_) ? "true" : "false"; return; }
    if (isNumber()) {
        const double d = std::get<double>(data_);
        char buf[64];
        if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        else std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
        return;
    }
    if (isString()) { escapeInto(out, std::get<std::string>(data_)); return; }
    if (isArray()) {
        const auto& a = std::get<Array>(data_);
        if (a.empty()) { out += "[]"; return; }
        out += '['; out += nl;
        for (std::size_t i = 0; i < a.size(); ++i) {
            out += padNext;
            a[i].dumpTo(out, indent, depth + 1);
            if (i + 1 < a.size()) out += ',';
            out += nl;
        }
        out += pad; out += ']';
        return;
    }
    const auto& o = std::get<Object>(data_);
    if (o.empty()) { out += "{}"; return; }
    out += '{'; out += nl;
    std::size_t i = 0;
    for (const auto& [k, v] : o) {
        out += padNext;
        escapeInto(out, k);
        out += ':';
        if (indent >= 0) out += ' ';
        v.dumpTo(out, indent, depth + 1);
        if (++i < o.size()) out += ',';
        out += nl;
    }
    out += pad; out += '}';
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
//  Парсер
// ---------------------------------------------------------------------------
namespace {

struct Parser {
    std::string_view src;
    std::size_t pos = 0;
    std::string err;

    void skipWs() {
        while (pos < src.size()) {
            const char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos; continue; }
            if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {   // комментарии не по RFC, но удобны
                while (pos < src.size() && src[pos] != '\n') ++pos;
                continue;
            }
            break;
        }
    }
    bool fail(const char* what) {
        if (err.empty()) {
            std::size_t line = 1;
            for (std::size_t i = 0; i < pos && i < src.size(); ++i) if (src[i] == '\n') ++line;
            std::ostringstream os;
            os << what << " at offset " << pos << " (line " << line << ")";
            err = os.str();
        }
        return false;
    }
    bool parseValue(Value& out) {
        skipWs();
        if (pos >= src.size()) return fail("unexpected end of input");
        const char c = src[pos];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') { std::string s; if (!parseString(s)) return false; out = Value(std::move(s)); return true; }
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n') { if (src.compare(pos, 4, "null") != 0) return fail("invalid literal"); pos += 4; out = Value(nullptr); return true; }
        return parseNumber(out);
    }
    bool parseBool(Value& out) {
        if (src.compare(pos, 4, "true") == 0) { pos += 4; out = Value(true); return true; }
        if (src.compare(pos, 5, "false") == 0) { pos += 5; out = Value(false); return true; }
        return fail("invalid literal");
    }
    bool parseNumber(Value& out) {
        const std::size_t start = pos;
        if (pos < src.size() && (src[pos] == '-' || src[pos] == '+')) ++pos;
        while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) ||
                                    src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' ||
                                    ((src[pos] == '-' || src[pos] == '+') && pos > start))) ++pos;
        if (pos == start) return fail("expected a number");
        const std::string text(src.substr(start, pos - start));
        out = Value(std::strtod(text.c_str(), nullptr));
        return true;
    }
    bool parseString(std::string& out) {
        if (src[pos] != '"') return fail("expected '\"'");
        ++pos;
        out.clear();
        while (pos < src.size() && src[pos] != '"') {
            char c = src[pos++];
            if (c != '\\') { out += c; continue; }
            if (pos >= src.size()) return fail("unterminated escape");
            const char e = src[pos++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (pos + 4 > src.size()) return fail("bad \\u escape");
                    const unsigned cp = static_cast<unsigned>(std::strtoul(std::string(src.substr(pos, 4)).c_str(), nullptr, 16));
                    pos += 4;
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        if (pos >= src.size()) return fail("unterminated string");
        ++pos;   // закрывающая кавычка
        return true;
    }
    bool parseArray(Value& out) {
        ++pos;   // '['
        Array a;
        skipWs();
        if (pos < src.size() && src[pos] == ']') { ++pos; out = Value(std::move(a)); return true; }
        for (;;) {
            Value v;
            if (!parseValue(v)) return false;
            a.push_back(std::move(v));
            skipWs();
            if (pos < src.size() && src[pos] == ',') { ++pos; continue; }
            if (pos < src.size() && src[pos] == ']') { ++pos; break; }
            return fail("expected ',' or ']'");
        }
        out = Value(std::move(a));
        return true;
    }
    bool parseObject(Value& out) {
        ++pos;   // '{'
        Object o;
        skipWs();
        if (pos < src.size() && src[pos] == '}') { ++pos; out = Value(std::move(o)); return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos >= src.size() || src[pos] != ':') return fail("expected ':'");
            ++pos;
            Value v;
            if (!parseValue(v)) return false;
            o[std::move(key)] = std::move(v);
            skipWs();
            if (pos < src.size() && src[pos] == ',') { ++pos; continue; }
            if (pos < src.size() && src[pos] == '}') { ++pos; break; }
            return fail("expected ',' or '}'");
        }
        out = Value(std::move(o));
        return true;
    }
};

} // namespace

bool parse(std::string_view text, Value& out, std::string& err) {
    Parser p{text, 0, {}};
    if (!p.parseValue(out)) { err = p.err; return false; }
    p.skipWs();
    if (p.pos != text.size()) { err = "trailing characters after JSON value"; return false; }
    return true;
}

Value parseOr(std::string_view text) {
    Value v;
    std::string err;
    if (!parse(text, v, err)) return Value(nullptr);
    return v;
}

} // namespace lv::json
