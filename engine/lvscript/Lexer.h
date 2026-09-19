/**
 * @file    Lexer.h
 * @brief   Однопроходный лексер LV Script.
 * @ingroup LVScript
 *
 * Поддерживает:
 *  - идентификаторы/ключевые слова (UTF-8);
 *  - числа: `10`, `0xFF`, `0b1010`, `3.14`, `1e-3`, `1_000_000`;
 *  - строки `"..."` с экранированием и интерполяцией `"{expr}"` (токены InterpBegin/InterpEnd);
 *  - однострочные комментарии (`#`, двойной слэш) и вложенные блочные;
 *  - все операторы языка, включая опциональную цепочку, элвис, диапазоны и стрелки.
 */
#pragma once

#include "Common.h"

namespace lv {

/**
 * @brief Категории токенов.
 */
enum class Tok : std::uint16_t {
    EndOfFile,
    // Литералы
    Number, String, InterpBegin, InterpEnd, Identifier, Keyword,
    // Ключевые слова (отдельные коды для ускорения парсера)
    KwFunc, KwLet, KwVar, KwIf, KwElif, KwElse, KwWhile, KwFor, KwIn, KwReturn,
    KwBreak, KwContinue, KwClass, KwExtends, KwSuper, KwThis, KwNew, KwNil,
    KwTrue, KwFalse, KwAnd, KwOr, KwNot, KwMatch, KwWhen, KwCoroutine,
    KwYield, KwResume, KwImport, KwAs, KwIs, KwStatic, KwConst, KwTry, KwPass, KwGlobal,
    KwDo, KwEnd,
    // Разделители
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, DotDot, DotDotEq, DotDotDot, Colon, ColonColon, Semicolon, Arrow, FatArrow, Question,
    // Операторы
    Plus, Minus, Star, Slash, Percent, StarStar,
    Eq, PlusEq, MinusEq, StarEq, SlashEq, PercentEq, StarStarEq,
    EqEq, BangEq, Lt, Gt, LtEq, GtEq,
    AmpAmp, PipePipe, Bang,
    Amp, Pipe, Caret, Tilde, Shl, Shr,
    QuestionQuestion, QuestionColon, QuestionDot, At,
    // Ошибки
    Error
};

/// Строковое имя токена (для сообщений).
[[nodiscard]] const char* tokenName(Tok t) noexcept;

/**
 * @brief Токен с привязкой к позиции и лексеме.
 */
struct Token {
    Tok          kind = Tok::EndOfFile;
    SourceLoc    loc{};
    std::string_view text;    ///< Лексема (view в исходный буфер).
    double       number = 0;  ///< Для Number.
    std::string  str;         ///< Для String — уже декодированное значение.
    bool         interp = false; ///< True, если строка содержит интерполяцию.

    [[nodiscard]] bool is(Tok k) const noexcept { return kind == k; }
    [[nodiscard]] std::string describe() const;
};

/**
 * @brief Лексер: преобразует исходный текст в поток токенов.
 *
 * Не хранит владения исходником — @c source должен жить дольше лексера.
 */
class Lexer {
public:
    Lexer(std::string_view source, SourceName name);

    /// Прочитать все токены. Диапазон ошибок добавляется в @c diags.
    std::vector<Token> tokenize(DiagnosticList& diags);

    /// Ключевое слово? Возвращает код или Tok::Identifier.
    [[nodiscard]] static Tok keywordKind(std::string_view word) noexcept;

private:
    [[nodiscard]] char peek(std::size_t off = 0) const noexcept;
    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= src_.size(); }
    char advance() noexcept;
    bool match(char c) noexcept;
    void skipWhitespaceAndComments(DiagnosticList& diags);
    Token makeToken(Tok k, SourceLoc loc, std::string_view text) const noexcept;
    Token readNumber();
    Token readString(DiagnosticList& diags);
    Token readIdentifierOrKeyword();
    Token readInterpolated(DiagnosticList& diags, SourceLoc start);
    void error(DiagnosticList& diags, SourceLoc loc, std::string msg);

    std::string_view src_;
    SourceName       name_;
    std::size_t      pos_ = 0;
    std::uint32_t    line_ = 1;
    std::uint32_t    col_ = 1;
    /// Глубина вложенных `{}` внутри интерполированного выражения.
    /// Позволяет писать `"{ {"k": 1} }"` — `}` литерала map не закрывает интерполяцию.
    int              braceDepth_ = 0;
    /// True, пока не закрыта завершающая кавычка интерполированной строки.
    bool             insideString_ = false;
    std::string      scratch_;  ///< Буфер для строковых литералов.
};

} // namespace lv
