/**
 * @file    Parser.h
 * @brief   Рекурсивный парсер LV Script с precedence climbing.
 * @ingroup LVScript
 *
 * Грамматика (сокращённо, полная — в docs/03_lvscript_spec.md):
 * @code
 * module      := (statement)* EOF
 * statement   := letStmt | varStmt | funcDecl | classDecl | coroutineDecl
 *               | ifStmt | whileStmt | forStmt | returnStmt | breakStmt
 *               | continueStmt | importStmt | matchStmt | block | exprStmt
 * block       := 'do' statement* 'end'
 * expr        := assignment
 * assignment  := ternary ( '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '**=' ) assignment
 * ternary     := coalesce ( '?' expr ':' expr | '?:' expr )?
 * coalesce    := orExpr ( '??' orExpr )*
 * orExpr      := andExpr ( ('or' | '||') andExpr )*
 * andExpr     := equality ( ('and' | '&&') equality )*
 * equality    := comparison ( ('==' | '!=') comparison )*
 * comparison  := bitor ( ('<' | '>' | '<=' | '>=' | 'is') bitor )*
 * bitor       := bitxor ( ('|' | '^' | '&') bitxor )*
 * bitxor      := shift ( ('|' | '^' | '&') shift )*
 * shift       := range ( ('<<' | '>>') range )*
 * range       := addition ( ('..' | '..=') addition )?
 * addition    := multiplication ( ('+' | '-') multiplication )*
 * multiply    := pow ( ('*' | '/' | '%') pow )*
 * pow         := unary ( '**' pow )?                     // право-ассоц.
 * unary       := ('-' | 'not' | '!' | '~') unary | postfix
 * postfix     := primary ( call | index | member | optMember )*
 * primary     := NUMBER | STRING | 'nil' | 'true' | 'false' | IDENT
 *               | 'this' | 'super' '.' IDENT | '(' exprList ')'
 *               | '[' exprList ']' | '{' mapEntries '}'
 *               | lambda | 'new' typeExpr '(' args ')'
 *               | ifExpr | whileExpr | forExpr | matchExpr
 *               | 'yield' expr? | 'resume' expr | 'try' expr | expr '?'
 * lambda      := ('|' params '|' | params '->') (block | '=>' expr)
 * @endcode
 */
#pragma once

#include "AST.h"
#include "Lexer.h"

namespace lv {

/**
 * @brief Парсер: поток токенов -> AST.
 *
 * Восстанавливается после ошибок на границах инструкций, поэтому редактор
 * может показывать несколько диагностик за один проход.
 */
class Parser {
public:
    Parser(std::vector<Token> tokens, SourceName name);

    /// Разобрать модуль. Даже при ошибках возвращается частичный AST.
    [[nodiscard]] Module parseModule(DiagnosticList& diags);

    /// Разобрать одно выражение (используется REPL и Visual Scripting).
    [[nodiscard]] ExprPtr parseExpressionOnly(DiagnosticList& diags);

private:
    // -- Низкоуровневый доступ к токенам ------------------------------------
    [[nodiscard]] const Token& peek(std::size_t off = 0) const noexcept;
    [[nodiscard]] Tok peekKind(std::size_t off = 0) const noexcept;
    [[nodiscard]] bool check(Tok k) const noexcept;
    [[nodiscard]] bool match(Tok k) noexcept;
    Token consume() noexcept;
    bool expect(Tok k, DiagnosticList& diags, const char* what, const char* hint = nullptr);
    void error(const Token& t, DiagnosticList& diags, std::string msg, std::string hint = {});
    void synchronize(DiagnosticList& diags);

    // -- Инструкции ---------------------------------------------------------
    StmtPtr parseStatement(DiagnosticList& diags);
    StmtPtr parseLet(DiagnosticList& diags, bool isVar, bool isGlobal);
    StmtPtr parseGlobalDecl(DiagnosticList& diags);
    StmtPtr parseFuncDecl(DiagnosticList& diags, bool isCoroutine, bool isStatic = false);
    StmtPtr parseClassDecl(DiagnosticList& diags);
    StmtPtr parseIfStmt(DiagnosticList& diags);
    StmtPtr parseWhileStmt(DiagnosticList& diags);
    StmtPtr parseForStmt(DiagnosticList& diags);
    StmtPtr parseMatchStmt(DiagnosticList& diags);
    StmtPtr parseImport(DiagnosticList& diags);
    StmtPtr parseBlock(DiagnosticList& diags);   ///< do ... end
    StmtPtr parseReturn(DiagnosticList& diags);
    StmtPtr parseExprStmt(DiagnosticList& diags);

    std::vector<Param> parseParams(DiagnosticList& diags);
    std::unique_ptr<MatchCase> parseMatchCase(DiagnosticList& diags);
    std::unique_ptr<Pattern> parsePattern(DiagnosticList& diags);

    // -- Выражения ----------------------------------------------------------
    ExprPtr parseExpr(DiagnosticList& diags);
    ExprPtr parseAssignment(DiagnosticList& diags);
    ExprPtr parseTernary(DiagnosticList& diags);
    ExprPtr parseBinary(DiagnosticList& diags, int minPrec);
    ExprPtr parseUnary(DiagnosticList& diags);
    ExprPtr parsePostfix(DiagnosticList& diags);
    ExprPtr parsePrimary(DiagnosticList& diags);
    ExprPtr parseLambda(DiagnosticList& diags);

    /**
     * @brief Превратить последний оператор-выражение блока в неявный `return`.
     *
     * Блочные тела лямбд (`|x| { a + b }`) и `do ... end`-блоки возвращают
     * значение последнего выражения — как в Rust/Kotlin. Без этой подстановки
     * компилятор генерировал `... ; Pop ; ReturnNil`, и колбэки (например,
     * setter'ы твинов) молча возвращали nil.
     */
    static void makeLastExprImplicitReturn(std::vector<StmtPtr>& body);
    ExprPtr parseIfExpr(DiagnosticList& diags);
    ExprPtr parseWhileExpr(DiagnosticList& diags);
    ExprPtr parseForExpr(DiagnosticList& diags, bool requireEnd = true);
    ExprPtr parseMatchExpr(DiagnosticList& diags);
    ExprPtr parseStringWithInterp(const Token& first, DiagnosticList& diags);

    // -- Типы ---------------------------------------------------------------
    TypeExprPtr parseType(DiagnosticList& diags);

    // -- Вспомогательное ----------------------------------------------------
    [[nodiscard]] static bool isStatementStart(Tok k) noexcept;
    [[nodiscard]] bool startsLambda() const noexcept;
    /**
     * @brief «Мягкие» ключевые слова: можно использовать как имена.
     *
     * `when`, `is`, `as`, `in`, `do`, `end`, `and`, `or`, `not`, `global`,
     * `static`, `pass`, `try` имеют смысл только в определённых позициях, поэтому
     * в роли имени параметра, поля или метода они не создают неоднозначности.
     * Жёсткие слова (`func`, `class`, `let`, `var`, `if`, `while`, `for`,
     * `return`, `break`, `continue`, `coroutine`, `yield`, `resume`, `match`,
     * `import`, `extends`, `super`, `this`, `new`, `nil`, `true`, `false`)
     * остаются зарезервированными.
     */
    [[nodiscard]] static bool isSoftKeyword(Tok k) noexcept;
    /// Съесть токен-имя (идентификатор или мягкое ключевое слово).
    bool consumeName(DiagnosticList& diags, std::string& out, const char* what);

    std::vector<Token> toks_;
    std::size_t        pos_ = 0;
    SourceName         name_;
    std::vector<std::string> lambdaNameHint_;
};

/// Удобная обёртка: исходник -> AST + диагностика.
[[nodiscard]] Module parseSource(std::string_view src, SourceName name, DiagnosticList& diags);

} // namespace lv
