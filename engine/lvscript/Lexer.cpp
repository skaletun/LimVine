/**
 * @file    Lexer.cpp
 * @brief   Реализация лексера LV Script.
 */
#include "Lexer.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace lv {

namespace {
const std::unordered_map<std::string_view, Tok>& keywordTable() {
    static const std::unordered_map<std::string_view, Tok> t = {
        {"func", Tok::KwFunc}, {"let", Tok::KwLet}, {"var", Tok::KwVar},
        {"if", Tok::KwIf}, {"elif", Tok::KwElif}, {"else", Tok::KwElse},
        {"while", Tok::KwWhile}, {"for", Tok::KwFor}, {"in", Tok::KwIn},
        {"return", Tok::KwReturn}, {"break", Tok::KwBreak}, {"continue", Tok::KwContinue},
        {"class", Tok::KwClass}, {"extends", Tok::KwExtends}, {"super", Tok::KwSuper},
        {"this", Tok::KwThis}, {"new", Tok::KwNew}, {"nil", Tok::KwNil},
        {"true", Tok::KwTrue}, {"false", Tok::KwFalse},
        {"and", Tok::KwAnd}, {"or", Tok::KwOr}, {"not", Tok::KwNot},
        {"match", Tok::KwMatch}, {"when", Tok::KwWhen},
        {"coroutine", Tok::KwCoroutine}, {"yield", Tok::KwYield}, {"resume", Tok::KwResume},
        {"import", Tok::KwImport}, {"as", Tok::KwAs}, {"is", Tok::KwIs},
        {"static", Tok::KwStatic}, {"const", Tok::KwConst},
        {"try", Tok::KwTry}, {"pass", Tok::KwPass}, {"global", Tok::KwGlobal},
        {"do", Tok::KwDo}, {"end", Tok::KwEnd},
    };
    return t;
}
} // namespace

Tok Lexer::keywordKind(std::string_view word) noexcept {
    const auto& t = keywordTable();
    auto it = t.find(word);
    return it == t.end() ? Tok::Identifier : it->second;
}

const char* tokenName(Tok t) noexcept {
    switch (t) {
        case Tok::EndOfFile: return "end of file";
        case Tok::Number: return "number";
        case Tok::String: return "string";
        case Tok::Identifier: return "identifier";
        case Tok::InterpBegin: return "'{' in string";
        case Tok::InterpEnd: return "'}' in string";
        case Tok::KwFunc: return "'func'";
        case Tok::KwLet: return "'let'";
        case Tok::KwVar: return "'var'";
        case Tok::KwIf: return "'if'";
        case Tok::KwElif: return "'elif'";
        case Tok::KwElse: return "'else'";
        case Tok::KwWhile: return "'while'";
        case Tok::KwFor: return "'for'";
        case Tok::KwIn: return "'in'";
        case Tok::KwReturn: return "'return'";
        case Tok::KwBreak: return "'break'";
        case Tok::KwContinue: return "'continue'";
        case Tok::KwClass: return "'class'";
        case Tok::KwExtends: return "'extends'";
        case Tok::KwSuper: return "'super'";
        case Tok::KwThis: return "'this'";
        case Tok::KwNew: return "'new'";
        case Tok::KwNil: return "'nil'";
        case Tok::KwTrue: return "'true'";
        case Tok::KwFalse: return "'false'";
        case Tok::KwAnd: return "'and'";
        case Tok::KwOr: return "'or'";
        case Tok::KwNot: return "'not'";
        case Tok::KwMatch: return "'match'";
        case Tok::KwWhen: return "'when'";
        case Tok::KwCoroutine: return "'coroutine'";
        case Tok::KwYield: return "'yield'";
        case Tok::KwResume: return "'resume'";
        case Tok::KwImport: return "'import'";
        case Tok::KwAs: return "'as'";
        case Tok::KwIs: return "'is'";
        case Tok::KwStatic: return "'static'";
        case Tok::KwConst: return "'const'";
        case Tok::KwTry: return "'try'";
        case Tok::KwPass: return "'pass'";
        case Tok::KwGlobal: return "'global'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Comma: return "','";
        case Tok::Dot: return "'.'";
        case Tok::DotDot: return "'..'";
        case Tok::DotDotEq: return "'..='";
        case Tok::DotDotDot: return "'...'";
        case Tok::Colon: return "':'";
        case Tok::ColonColon: return "'::'";
        case Tok::Semicolon: return "';'";
        case Tok::Arrow: return "'->'";
        case Tok::FatArrow: return "'=>'";
        case Tok::Question: return "'?'";
        case Tok::Plus: return "'+'";
        case Tok::Minus: return "'-'";
        case Tok::Star: return "'*'";
        case Tok::Slash: return "'/'";
        case Tok::Percent: return "'%'";
        case Tok::StarStar: return "'**'";
        case Tok::Eq: return "'='";
        case Tok::PlusEq: return "'+='";
        case Tok::MinusEq: return "'-='";
        case Tok::StarEq: return "'*='";
        case Tok::SlashEq: return "'/='";
        case Tok::PercentEq: return "'%='";
        case Tok::StarStarEq: return "'**='";
        case Tok::EqEq: return "'=='";
        case Tok::BangEq: return "'!='";
        case Tok::Lt: return "'<'";
        case Tok::Gt: return "'>'";
        case Tok::LtEq: return "'<='";
        case Tok::GtEq: return "'>='";
        case Tok::AmpAmp: return "'&&'";
        case Tok::PipePipe: return "'||'";
        case Tok::Bang: return "'!'";
        case Tok::Amp: return "'&'";
        case Tok::Pipe: return "'|'";
        case Tok::Caret: return "'^'";
        case Tok::Tilde: return "'~'";
        case Tok::Shl: return "'<<'";
        case Tok::Shr: return "'>>'";
        case Tok::QuestionQuestion: return "'??'";
        case Tok::QuestionColon: return "'?:'";
        case Tok::QuestionDot: return "'?.'";
        case Tok::At: return "'@'";
        case Tok::KwDo: return "'do'";
        case Tok::KwEnd: return "'end'";
        case Tok::Keyword: return "keyword";
        case Tok::Error: return "invalid token";
    }
    return "?";
}

std::string Token::describe() const {
    switch (kind) {
        case Tok::Identifier: return format("identifier '{}'", std::string(text));
        case Tok::Number:     return format("number '{}'", std::string(text));
        case Tok::String:     return format("string {:?}", str);
        default:              return tokenName(kind);
    }
}

// ---------------------------------------------------------------------------
Lexer::Lexer(std::string_view source, SourceName name) : src_(source), name_(std::move(name)) {}

char Lexer::peek(std::size_t off) const noexcept {
    const std::size_t i = pos_ + off;
    return i < src_.size() ? src_[i] : '\0';
}

char Lexer::advance() noexcept {
    const char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
    return c;
}

bool Lexer::match(char c) noexcept {
    if (atEnd() || src_[pos_] != c) return false;
    advance();
    return true;
}

Token Lexer::makeToken(Tok k, SourceLoc loc, std::string_view text) const noexcept {
    Token t;
    t.kind = k;
    t.loc = loc;
    t.text = text;
    return t;
}

void Lexer::error(DiagnosticList& diags, SourceLoc loc, std::string msg) {
    diags.push_back(Diagnostic{DiagSeverity::Error, name_, loc, std::move(msg), {}});
}

void Lexer::skipWhitespaceAndComments(DiagnosticList& diags) {
    for (;;) {
        while (!atEnd()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(); continue; }
            break;
        }
        if (atEnd()) return;
        // Комментарии
        if (peek() == '#') { while (!atEnd() && peek() != '\n') advance(); continue; }
        if (peek() == '/' && peek(1) == '/') { while (!atEnd() && peek() != '\n') advance(); continue; }
        if (peek() == '/' && peek(1) == '*') {
            SourceLoc start{line_, col_, static_cast<std::uint32_t>(pos_)};
            advance(); advance();
            int depth = 1;
            while (!atEnd() && depth > 0) {
                if (peek() == '/' && peek(1) == '*') { advance(); advance(); ++depth; }
                else if (peek() == '*' && peek(1) == '/') { advance(); advance(); --depth; }
                else advance();
            }
            if (depth > 0) error(diags, start, "unterminated block comment");
            continue;
        }
        return;
    }
}

Token Lexer::readNumber() {
    SourceLoc loc{line_, col_, static_cast<std::uint32_t>(pos_)};
    const std::size_t start = pos_;
    std::int64_t intVal = 0;
    bool isFloat = false;
    int base = 10;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance(); advance(); base = 16;
        while (!atEnd() && (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
            if (peek() != '_') intVal = intVal * 16 + static_cast<std::int64_t>(std::stoi(std::string(1, peek()), nullptr, 16));
            advance();
        }
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        advance(); advance(); base = 2;
        while (!atEnd() && (peek() == '0' || peek() == '1' || peek() == '_')) {
            if (peek() != '_') intVal = intVal * 2 + (peek() - '0');
            advance();
        }
    } else {
        while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
            if (peek() != '_') intVal = intVal * 10 + (peek() - '0');
            advance();
        }
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            isFloat = true;
            advance();
            while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
        } else if (peek() == '.' && !std::isdigit(static_cast<unsigned char>(peek(1))) && peek(1) != '.') {
            // `1..5` — range, точка не часть числа. Оставляем целым.
        }
        if (peek() == 'e' || peek() == 'E') {
            const std::size_t save = pos_;
            const std::uint32_t sl = line_, sc = col_;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (std::isdigit(static_cast<unsigned char>(peek()))) {
                isFloat = true;
                while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
            } else {
                pos_ = save; line_ = sl; col_ = sc;
            }
        }
    }

    std::string_view text = src_.substr(start, pos_ - start);
    Token t = makeToken(Tok::Number, loc, text);
    if (!isFloat && base == 10) {
        t.number = static_cast<double>(intVal);
    } else if (!isFloat) {
        t.number = static_cast<double>(intVal);
    } else {
        double d = 0;
        std::string cleaned;
        cleaned.reserve(text.size());
        for (char c : text) if (c != '_') cleaned += c;
        std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), d);
        t.number = d;
    }
    return t;
}

Token Lexer::readString(DiagnosticList& diags) {
    SourceLoc loc{line_, col_, static_cast<std::uint32_t>(pos_)};
    advance(); // открывающая кавычка
    scratch_.clear();
    bool interp = false;

    for (;;) {
        if (atEnd()) { error(diags, loc, "unterminated string literal"); break; }
        const char c = peek();
        if (c == '"') { advance(); break; }
        if (c == '{') {
            // Интерполяция: отдаём накопленный литерал и переходим в режим выражения.
            advance();
            interp = true;
            insideString_ = true;
            Token begin = makeToken(Tok::InterpBegin, loc, std::string_view{});
            begin.str = scratch_;   // часть строки до '{'
            begin.interp = true;
            scratch_.clear();
            return begin;
        }
        if (c == '\\') {
            advance();
            if (atEnd()) { error(diags, loc, "unterminated escape sequence"); break; }
            const char e = advance();
            switch (e) {
                case 'n': scratch_ += '\n'; break;
                case 't': scratch_ += '\t'; break;
                case 'r': scratch_ += '\r'; break;
                case '0': scratch_ += '\0'; break;
                case '\\': scratch_ += '\\'; break;
                case '"': scratch_ += '"'; break;
                case '{': scratch_ += '{'; break;
                case '}': scratch_ += '}'; break;
                case 'x': {
                    if (pos_ + 1 < src_.size()) {
                        const int v = std::stoi(std::string(src_.substr(pos_, 2)), nullptr, 16);
                        scratch_ += static_cast<char>(v);
                        advance(); advance();
                    }
                    break;
                }
                default:
                    error(diags, {line_, col_, static_cast<std::uint32_t>(pos_)},
                          format("unknown escape sequence '\\{}'", e));
                    scratch_ += e;
            }
            continue;
        }
        scratch_ += advance();
    }

    insideString_ = false;   // ordinary (non-interpolated) string is complete
    Token t = makeToken(Tok::String, loc, std::string_view{});
    t.str = scratch_;
    t.interp = interp;
    return t;
}

Token Lexer::readInterpolated(DiagnosticList& diags, SourceLoc start) {
    // Вызывается после InterpEnd: дочитываем остаток строки как новый литерал.
    scratch_.clear();
    for (;;) {
        if (atEnd()) { error(diags, start, "unterminated string literal"); break; }
        const char c = peek();
        if (c == '"') { advance(); insideString_ = false; break; }
        if (c == '{') {
            advance();
            Token begin = makeToken(Tok::InterpBegin, {line_, col_, static_cast<std::uint32_t>(pos_)}, {});
            begin.str = scratch_;
            begin.interp = true;
            scratch_.clear();
            return begin;
        }
        if (c == '\\') {
            advance();
            if (atEnd()) break;
            const char e = advance();
            switch (e) {
                case 'n': scratch_ += '\n'; break;
                case 't': scratch_ += '\t'; break;
                case '"': scratch_ += '"'; break;
                case '\\': scratch_ += '\\'; break;
                case '{': scratch_ += '{'; break;
                default: scratch_ += e;
            }
            continue;
        }
        scratch_ += advance();
    }
    Token t = makeToken(Tok::String, start, {});
    t.str = scratch_;
    t.interp = true;
    return t;
}

Token Lexer::readIdentifierOrKeyword() {
    SourceLoc loc{line_, col_, static_cast<std::uint32_t>(pos_)};
    const std::size_t start = pos_;
    auto isIdStart = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
               static_cast<unsigned char>(c) >= 0x80; // UTF-8
    };
    auto isIdCont = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
               static_cast<unsigned char>(c) >= 0x80;
    };
    while (!atEnd() && isIdCont(peek())) advance();
    std::string_view text = src_.substr(start, pos_ - start);
    Tok kw = keywordKind(text);
    return makeToken(kw == Tok::Identifier ? Tok::Identifier : kw, loc, text);
}

std::vector<Token> Lexer::tokenize(DiagnosticList& diags) {
    std::vector<Token> out;
    out.reserve(src_.size() / 3 + 16);

    for (;;) {
        skipWhitespaceAndComments(diags);
        if (atEnd()) {
            out.push_back(makeToken(Tok::EndOfFile, {line_, col_, static_cast<std::uint32_t>(pos_)}, {}));
            break;
        }

        const SourceLoc loc{line_, col_, static_cast<std::uint32_t>(pos_)};
        const char c = peek();

        // Закрывающая скобка интерполяции — только на нулевой глубине вложенных `{}`,
        // иначе `}` литерала map внутри `"{ ... }"` ошибочно завершил бы интерполяцию.
        if (insideString_ && braceDepth_ == 0 && c == '}') {
            advance();
            const SourceLoc iloc{line_, col_ - 1, static_cast<std::uint32_t>(pos_ - 1)};
            out.push_back(makeToken(Tok::InterpEnd, iloc, {}));
            out.push_back(readInterpolated(diags, iloc));
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) { out.push_back(readNumber()); continue; }
        if (c == '"') { out.push_back(readString(diags)); continue; }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
            static_cast<unsigned char>(c) >= 0x80) {
            out.push_back(readIdentifierOrKeyword());
            continue;
        }

        // Операторы
        advance();
        Tok k = Tok::Error;
        std::size_t len = 1;
        switch (c) {
            case '(': k = Tok::LParen; break;
            case ')': k = Tok::RParen; break;
            case '{': k = Tok::LBrace; if (insideString_) ++braceDepth_; break;
            case '}': k = Tok::RBrace; if (insideString_ && braceDepth_ > 0) --braceDepth_; break;
            case '[': k = Tok::LBracket; break;
            case ']': k = Tok::RBracket; break;
            case ',': k = Tok::Comma; break;
            case ';': k = Tok::Semicolon; break;
            case '@': k = Tok::At; break;
            case '~': k = Tok::Tilde; break;
            case ':':
                if (match(':')) { k = Tok::ColonColon; len = 2; } else k = Tok::Colon;
                break;
            case '.':
                if (match('.')) {
                    if (match('.'))      { k = Tok::DotDotDot; len = 3; }
                    else if (match('=')) { k = Tok::DotDotEq;  len = 3; }
                    else                 { k = Tok::DotDot;     len = 2; }
                } else k = Tok::Dot;
                break;
            case '+': if (match('=')) { k = Tok::PlusEq; len = 2; } else k = Tok::Plus; break;
            case '-':
                if (match('=')) { k = Tok::MinusEq; len = 2; }
                else if (match('>')) { k = Tok::Arrow; len = 2; }
                else k = Tok::Minus;
                break;
            case '*':
                if (match('*')) { if (match('=')) { k = Tok::StarStarEq; len = 3; } else { k = Tok::StarStar; len = 2; } }
                else if (match('=')) { k = Tok::StarEq; len = 2; }
                else k = Tok::Star;
                break;
            case '/': if (match('=')) { k = Tok::SlashEq; len = 2; } else k = Tok::Slash; break;
            case '%': if (match('=')) { k = Tok::PercentEq; len = 2; } else k = Tok::Percent; break;
            case '=':
                if (match('=')) { k = Tok::EqEq; len = 2; }
                else if (match('>')) { k = Tok::FatArrow; len = 2; }
                else k = Tok::Eq;
                break;
            case '!': if (match('=')) { k = Tok::BangEq; len = 2; } else k = Tok::Bang; break;
            case '<':
                if (match('=')) { k = Tok::LtEq; len = 2; }
                else if (match('<')) { k = Tok::Shl; len = 2; }
                else k = Tok::Lt;
                break;
            case '>':
                if (match('=')) { k = Tok::GtEq; len = 2; }
                else if (match('>')) { k = Tok::Shr; len = 2; }
                else k = Tok::Gt;
                break;
            case '&': if (match('&')) { k = Tok::AmpAmp; len = 2; } else k = Tok::Amp; break;
            case '|': if (match('|')) { k = Tok::PipePipe; len = 2; } else k = Tok::Pipe; break;
            case '^': k = Tok::Caret; break;
            case '?':
                if (match('?')) { k = Tok::QuestionQuestion; len = 2; }
                else if (match('.')) { k = Tok::QuestionDot; len = 2; }
                else if (match(':')) { k = Tok::QuestionColon; len = 2; }
                else k = Tok::Question;
                break;
            default:
                error(diags, loc, format("unexpected character '{}'", c));
                continue;
        }
        out.push_back(makeToken(k, loc, src_.substr(loc.offset, len)));
    }
    return out;
}

} // namespace lv
