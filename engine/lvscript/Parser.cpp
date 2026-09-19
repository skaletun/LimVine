/**
 * @file    Parser.cpp
 * @brief   Реализация рекурсивного парсера LV Script.
 */
#include "Parser.h"

#include <set>

namespace lv {

// ---------------------------------------------------------------------------
//  Вспомогательные структуры
// ---------------------------------------------------------------------------
namespace {

/// Приоритет бинарного оператора; 0 — не бинарный.
struct BinInfo { int prec; BinOp op; };

std::optional<BinInfo> binInfoFor(Tok k) {
    switch (k) {
        case Tok::EqEq:    return BinInfo{8, BinOp::Eq};
        case Tok::BangEq:  return BinInfo{8, BinOp::Ne};
        case Tok::Lt:      return BinInfo{9, BinOp::Lt};
        case Tok::Gt:      return BinInfo{9, BinOp::Gt};
        case Tok::LtEq:    return BinInfo{9, BinOp::Le};
        case Tok::GtEq:    return BinInfo{9, BinOp::Ge};
        case Tok::Pipe:    return BinInfo{10, BinOp::BitOr};
        case Tok::Caret:   return BinInfo{11, BinOp::BitXor};
        case Tok::Amp:     return BinInfo{12, BinOp::BitAnd};
        case Tok::Shl:     return BinInfo{13, BinOp::Shl};
        case Tok::Shr:     return BinInfo{13, BinOp::Shr};
        case Tok::Plus:    return BinInfo{15, BinOp::Add};
        case Tok::Minus:   return BinInfo{15, BinOp::Sub};
        case Tok::Star:    return BinInfo{16, BinOp::Mul};
        case Tok::Slash:   return BinInfo{16, BinOp::Div};
        case Tok::Percent: return BinInfo{16, BinOp::Mod};
        case Tok::StarStar:return BinInfo{18, BinOp::Pow};
        default: return std::nullopt;
    }
}

std::optional<CompoundOp> compoundFor(Tok k) {
    switch (k) {
        case Tok::PlusEq:      return CompoundOp::Add;
        case Tok::MinusEq:     return CompoundOp::Sub;
        case Tok::StarEq:      return CompoundOp::Mul;
        case Tok::SlashEq:     return CompoundOp::Div;
        case Tok::PercentEq:   return CompoundOp::Mod;
        case Tok::StarStarEq:  return CompoundOp::Pow;
        default: return std::nullopt;
    }
}

bool isKeywordish(Tok k) {
    return k == Tok::KwIf || k == Tok::KwWhile || k == Tok::KwFor || k == Tok::KwMatch ||
           k == Tok::KwReturn || k == Tok::KwBreak || k == Tok::KwContinue ||
           k == Tok::KwLet || k == Tok::KwVar || k == Tok::KwFunc || k == Tok::KwClass ||
           k == Tok::KwImport || k == Tok::KwYield || k == Tok::KwResume ||
           k == Tok::KwCoroutine || k == Tok::KwTry;
}

} // namespace

// ---------------------------------------------------------------------------
Parser::Parser(std::vector<Token> tokens, SourceName name)
    : toks_(std::move(tokens)), name_(std::move(name)) {}

const Token& Parser::peek(std::size_t off) const noexcept {
    const std::size_t i = pos_ + off;
    return i < toks_.size() ? toks_[i] : toks_.back();
}

Tok Parser::peekKind(std::size_t off) const noexcept { return peek(off).kind; }

bool Parser::check(Tok k) const noexcept { return peekKind() == k; }

bool Parser::match(Tok k) noexcept {
    if (check(k)) { ++pos_; return true; }
    return false;
}

Token Parser::consume() noexcept { return toks_[pos_++]; }

bool Parser::expect(Tok k, DiagnosticList& diags, const char* what, const char* hint) {
    if (match(k)) return true;
    error(peek(), diags, format("expected {}, found {}", what, peek().describe()));
    return false;
}

void Parser::error(const Token& t, DiagnosticList& diags, std::string msg, std::string hint) {
    if (!diags.empty() && diags.back().loc.line == t.loc.line &&
        diags.back().message == msg)
        return; // не дублируем одну и ту же ошибку в пределах строки
    diags.push_back(Diagnostic{DiagSeverity::Error, name_, t.loc, std::move(msg), std::move(hint)});
}

void Parser::synchronize(DiagnosticList& diags) {
    (void)diags;
    while (!check(Tok::EndOfFile)) {
        if (isStatementStart(peekKind())) return;
        ++pos_;
    }
}

bool Parser::isSoftKeyword(Tok k) noexcept {
    switch (k) {
        case Tok::KwWhen: case Tok::KwIs: case Tok::KwAs: case Tok::KwIn:
        case Tok::KwDo: case Tok::KwEnd: case Tok::KwAnd: case Tok::KwOr:
        case Tok::KwNot: case Tok::KwGlobal: case Tok::KwStatic: case Tok::KwPass:
        case Tok::KwTry:
            return true;
        default:
            return false;
    }
}

bool Parser::consumeName(DiagnosticList& diags, std::string& out, const char* what) {
    if (check(Tok::Identifier) || isSoftKeyword(peekKind())) {
        out = std::string(consume().text);
        return true;
    }
    error(peek(), diags, format("expected {}, found {}", what, peek().describe()));
    return false;
}

bool Parser::isStatementStart(Tok k) noexcept {
    switch (k) {
        case Tok::KwLet: case Tok::KwVar: case Tok::KwFunc: case Tok::KwClass:
        case Tok::KwIf: case Tok::KwWhile: case Tok::KwFor: case Tok::KwReturn:
        case Tok::KwBreak: case Tok::KwContinue: case Tok::KwImport: case Tok::KwMatch:
        case Tok::KwCoroutine: case Tok::KwGlobal: case Tok::KwPass:
            return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
//  Типы
// ---------------------------------------------------------------------------
TypeExprPtr Parser::parseType(DiagnosticList& diags) {
    auto t = std::make_unique<TypeExpr>();
    t->loc = peek().loc;

    if (match(Tok::LBracket)) {
        t->kind = TypeExpr::Kind::Array;
        t->params.push_back(parseType(diags));
        expect(Tok::RBracket, diags, "']'");
    } else if (match(Tok::LParen)) {
        // (A, B) -> C  либо  (A)
        std::vector<TypeExprPtr> args;
        if (!check(Tok::RParen)) {
            args.push_back(parseType(diags));
            while (match(Tok::Comma)) args.push_back(parseType(diags));
        }
        expect(Tok::RParen, diags, "')'");
        if (match(Tok::Arrow)) {
            t->kind = TypeExpr::Kind::Func;
            t->params = std::move(args);
            t->ret = parseType(diags);
        } else {
            t->kind = TypeExpr::Kind::Tuple;
            t->params = std::move(args);
        }
    } else if (check(Tok::Identifier)) {
        Token id = consume();
        t->kind = TypeExpr::Kind::Named;
        t->name = std::string(id.text);
        if (t->name == "Any")  t->kind = TypeExpr::Kind::Any;
        if (t->name == "Self") t->kind = TypeExpr::Kind::Self;
        if (t->name == "Nil")  t->kind = TypeExpr::Kind::Nil;
        if (t->name == "Map" && check(Tok::Lt)) {
            consume();
            t->kind = TypeExpr::Kind::Map;
            t->params.push_back(parseType(diags));
            expect(Tok::Comma, diags, "','");
            t->params.push_back(parseType(diags));
            expect(Tok::Gt, diags, "'>'");
        }
    } else if (match(Tok::KwNil)) {
        t->kind = TypeExpr::Kind::Nil;
    } else {
        error(peek(), diags, "expected a type annotation");
        t->kind = TypeExpr::Kind::Any;
    }

    // Опциональность: `T?`
    while (match(Tok::Question)) {
        auto opt = std::make_unique<TypeExpr>();
        opt->kind = TypeExpr::Kind::Optional;
        opt->loc = t->loc;
        opt->params.push_back(std::move(t));
        t = std::move(opt);
    }
    return t;
}

// ---------------------------------------------------------------------------
//  Модуль и инструкции
// ---------------------------------------------------------------------------
Module Parser::parseModule(DiagnosticList& diags) {
    Module m;
    m.name = name_;
    while (!check(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        if (StmtPtr s = parseStatement(diags)) m.body.push_back(std::move(s));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    return m;
}

ExprPtr Parser::parseExpressionOnly(DiagnosticList& diags) { return parseExpr(diags); }

StmtPtr Parser::parseStatement(DiagnosticList& diags) {
    const Token t = peek();
    switch (t.kind) {
        case Tok::KwLet:  return parseLet(diags, /*isVar=*/false, /*isGlobal=*/false);
        case Tok::KwVar:  return parseLet(diags, /*isVar=*/true,  /*isGlobal=*/false);
        case Tok::KwGlobal: {
            consume();
            // `global let x = ...` / `global var x = ...` / `global x = ...`
            if (check(Tok::KwLet)) return parseLet(diags, false, true);
            if (check(Tok::KwVar)) return parseLet(diags, true, true);
            return parseGlobalDecl(diags);
        }
        case Tok::KwFunc:      return parseFuncDecl(diags, false);
        case Tok::KwCoroutine:
            // `coroutine name(params) do ... end` — объявление;
            // `coroutine(fn)` — вызов фабрики корутин из стандартной библиотеки.
            if (peekKind(1) == Tok::LParen) return parseExprStmt(diags);
            return parseFuncDecl(diags, true);
        case Tok::KwClass:     return parseClassDecl(diags);
        case Tok::KwIf:        return parseIfStmt(diags);
        case Tok::KwWhile:     return parseWhileStmt(diags);
        case Tok::KwFor:       return parseForStmt(diags);
        case Tok::KwMatch:     return parseMatchStmt(diags);
        case Tok::KwImport:    return parseImport(diags);
        case Tok::KwReturn:    return parseReturn(diags);
        case Tok::KwBreak: {
            consume();
            auto s = std::make_unique<Stmt>(StmtKind::Break, t.loc);
            if (check(Tok::Identifier)) s->label = std::string(consume().text);
            return s;
        }
        case Tok::KwContinue: {
            consume();
            auto s = std::make_unique<Stmt>(StmtKind::Continue, t.loc);
            if (check(Tok::Identifier)) s->label = std::string(consume().text);
            return s;
        }
        case Tok::KwPass: {
            consume();
            return std::make_unique<Stmt>(StmtKind::Pass, t.loc);
        }
        case Tok::KwStatic: {
            consume();
            return parseFuncDecl(diags, false, true);
        }
        case Tok::KwYield: {
            consume();
            auto s = std::make_unique<Stmt>(StmtKind::Yield, t.loc);
            if (!check(Tok::EndOfFile) && !check(Tok::KwEnd) && !check(Tok::KwElse) && !check(Tok::KwElif))
                s->expr = parseExpr(diags);
            return s;
        }
        case Tok::KwResume: {
            consume();
            auto s = std::make_unique<Stmt>(StmtKind::Resume, t.loc);
            s->expr = parseExpr(diags);
            return s;
        }
        case Tok::LBrace: {
            // Блок-инструкция `{ ... }` (локальная область видимости).
            consume();
            auto s = std::make_unique<Stmt>(StmtKind::Block, t.loc);
            while (!check(Tok::RBrace) && !check(Tok::EndOfFile)) {
                const std::size_t before = pos_;
                if (StmtPtr inner = parseStatement(diags)) s->body.push_back(std::move(inner));
                if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
            }
            expect(Tok::RBrace, diags, "'}'");
            return s;
        }
        default:
            return parseExprStmt(diags);
    }
}

StmtPtr Parser::parseExprStmt(DiagnosticList& diags) {
    const SourceLoc loc = peek().loc;
    ExprPtr e = parseExpr(diags);
    auto s = std::make_unique<Stmt>(StmtKind::ExprStmt, loc);
    s->expr = std::move(e);
    match(Tok::Semicolon);
    return s;
}

StmtPtr Parser::parseGlobalDecl(DiagnosticList& diags) {
    // `global name = expr` — объявление переменной в глобальной области модуля.
    const SourceLoc loc = peek().loc;
    auto s = std::make_unique<Stmt>(StmtKind::Let, loc);
    s->isMutable = true;
    s->isGlobal = true;
    if (!expect(Tok::Identifier, diags, "global variable name")) return s;
    s->names.push_back(std::string(peek(-1).text));
    if (match(Tok::Colon)) s->types.push_back(parseType(diags));
    else s->types.push_back(nullptr);
    if (match(Tok::Eq)) s->value = parseExpr(diags);
    (void)match(Tok::Semicolon);
    return s;
}

StmtPtr Parser::parseLet(DiagnosticList& diags, bool isVar, bool isGlobal) {
    // `let`/`var` уже могут быть потреблены вызывающим кодом (например, `global x = ...`).
    const SourceLoc kwLoc = peek().loc;
    if (check(Tok::KwLet) || check(Tok::KwVar)) consume();
    auto s = std::make_unique<Stmt>(StmtKind::Let, kwLoc);
    s->isMutable = isVar;
    s->isGlobal = isGlobal;

    do {
        if (!expect(Tok::Identifier, diags, "variable name")) break;
        s->names.push_back(std::string(peek(-1).text));
        if (match(Tok::Colon)) s->types.push_back(parseType(diags));
        else s->types.push_back(nullptr);
    } while (match(Tok::Comma));

    if (match(Tok::Eq)) s->value = parseExpr(diags);
    match(Tok::Semicolon);
    return s;
}

std::vector<Param> Parser::parseParams(DiagnosticList& diags) {
    std::vector<Param> params;
    expect(Tok::LParen, diags, "'('");
    if (!check(Tok::RParen)) {
        do {
            Param p;
            p.loc = peek().loc;
            if (!consumeName(diags, p.name, "parameter name")) break;
            if (match(Tok::Colon)) p.type = parseType(diags);
            if (match(Tok::DotDotDot)) p.isRest = true;
            if (match(Tok::Eq)) p.defaultValue = parseExpr(diags);
            params.push_back(std::move(p));
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, diags, "')'");
    return params;
}

StmtPtr Parser::parseFuncDecl(DiagnosticList& diags, bool isCoroutine, bool isStatic) {
    const Token kw = consume(); // func | coroutine | (static уже съеден)
    auto s = std::make_unique<Stmt>(isCoroutine ? StmtKind::Coroutine : StmtKind::Func, kw.loc);
    s->isCoroutine = isCoroutine;
    s->isGlobal = isStatic;

    if (check(Tok::Identifier)) s->name = std::string(consume().text);
    else error(peek(), diags, "expected function name");

    s->params = parseParams(diags);
    if (match(Tok::Arrow)) s->returnType = parseType(diags);

    if (match(Tok::FatArrow)) {
        auto ret = std::make_unique<Stmt>(StmtKind::Return, peek().loc);
        ret->expr = parseExpr(diags);
        s->body.push_back(std::move(ret));
    } else {
        expect(Tok::KwDo, diags, "'do' before function body",
               "use '=> expr' for a single-expression body");
        while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
            const std::size_t before = pos_;
            if (StmtPtr inner = parseStatement(diags)) s->body.push_back(std::move(inner));
            if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
        }
        expect(Tok::KwEnd, diags, "'end' to close function body");
    }
    return s;
}

StmtPtr Parser::parseClassDecl(DiagnosticList& diags) {
    const Token kw = consume(); // class
    auto s = std::make_unique<Stmt>(StmtKind::Class, kw.loc);
    if (!expect(Tok::Identifier, diags, "class name")) { synchronize(diags); return s; }
    s->name = std::string(peek(-1).text);

    if (match(Tok::KwExtends)) {
        if (expect(Tok::Identifier, diags, "superclass name")) s->superClass = std::string(peek(-1).text);
    }

    expect(Tok::KwDo, diags, "'do' before class body");
    while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
        const SourceLoc mloc = peek().loc;
        bool isStatic = match(Tok::KwStatic);
        if (check(Tok::KwVar) || check(Tok::KwLet) || check(Tok::KwConst)) {
            const bool isConst = check(Tok::KwConst);
            consume();
            FieldDecl f;
            f.loc = mloc;
            f.isConst = isConst;
            f.isStatic = isStatic;
            if (!consumeName(diags, f.name, "field name")) break;
            if (match(Tok::Colon)) f.type = parseType(diags);
            if (match(Tok::Eq)) f.initializer = parseExpr(diags);
            s->fields.push_back(std::move(f));
            match(Tok::Semicolon);
            continue;
        }
        if (check(Tok::KwFunc) || check(Tok::KwCoroutine)) {
            const bool isCo = check(Tok::KwCoroutine);
            consume();
            MethodDecl m;
            m.loc = mloc;
            m.isStatic = isStatic;
            m.isCoroutine = isCo;
            if (!consumeName(diags, m.name, "method name")) break;
            m.params = parseParams(diags);
            if (match(Tok::Arrow)) m.returnType = parseType(diags);
            if (match(Tok::FatArrow)) {
                auto ret = std::make_unique<Stmt>(StmtKind::Return, peek().loc);
                ret->expr = parseExpr(diags);
                m.body.push_back(std::move(ret));
            } else {
                expect(Tok::KwDo, diags, "'do' before method body");
                while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
                    const std::size_t before = pos_;
                    if (StmtPtr inner = parseStatement(diags)) m.body.push_back(std::move(inner));
                    if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
                }
                expect(Tok::KwEnd, diags, "'end' to close method body");
            }
            s->methods.push_back(std::move(m));
            continue;
        }
        error(peek(), diags, "expected field or method declaration in class body");
        synchronize(diags);
        break;
    }
    expect(Tok::KwEnd, diags, "'end' to close class body");
    return s;
}

StmtPtr Parser::parseIfStmt(DiagnosticList& diags) {
    const Token kw = consume(); // if
    auto s = std::make_unique<Stmt>(StmtKind::If, kw.loc);
    s->cond = parseExpr(diags);
    expect(Tok::KwDo, diags, "'do' after if condition");
    while (!check(Tok::KwEnd) && !check(Tok::KwElif) && !check(Tok::KwElse) && !check(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        if (StmtPtr inner = parseStatement(diags)) s->body.push_back(std::move(inner));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    while (match(Tok::KwElif)) {
        s->elifConds.push_back(parseExpr(diags));
        expect(Tok::KwDo, diags, "'do' after elif condition");
        std::vector<StmtPtr> branch;
        while (!check(Tok::KwEnd) && !check(Tok::KwElif) && !check(Tok::KwElse) && !check(Tok::EndOfFile)) {
            const std::size_t before = pos_;
            if (StmtPtr inner = parseStatement(diags)) branch.push_back(std::move(inner));
            if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
        }
        s->elifBodies.push_back(std::move(branch));
    }
    if (match(Tok::KwElse)) {
        if (check(Tok::KwIf)) {
            // `else if ...` — вложенный if, у которого СВОЙ завершающий `end`.
            // Поэтому общий `end` всего if'а здесь не ожидается.
            auto nested = parseIfStmt(diags);
            s->elseBody.push_back(std::move(nested));
            return s;
        }
        if (check(Tok::KwDo)) {
            consume();
            while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
                const std::size_t before = pos_;
                if (StmtPtr inner = parseStatement(diags)) s->elseBody.push_back(std::move(inner));
                if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
            }
            // Блок else НЕ имеет собственного `end`: единственный завершающий
            // `end` закрывает всю конструкцию if (Lua-подобный синтаксис).
        } else {
            // `else` БЕЗ ключевого слова `do`. Поддерживаются две формы:
            //   1) короткая   — `else <expr>` (завершающий `end` необязателен);
            //   2) блочная    — несколько операторов, закрытых общим `end`.
            //
            // Раньше здесь разбирался РОВНО ОДИН оператор, поэтому блочная форма
            // ломалась: `else a = 1 / b = 2 / end` терял хвост, а `end`
            // доставался внешнему блоку (в теле класса это давало каскад
            // «expected field or method declaration in class body»).
            //
            // Разбор идёт до ограничителя ветви (end/elif/else/EOF); завершающий
            // `end` потребляем только если он действительно стоит следом — так
            // короткая форма без `end` по-прежнему работает.
            while (!check(Tok::KwEnd) && !check(Tok::KwElif) && !check(Tok::KwElse) &&
                   !check(Tok::EndOfFile)) {
                const std::size_t before = pos_;
                if (StmtPtr inner = parseStatement(diags)) s->elseBody.push_back(std::move(inner));
                if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
            }
            if (match(Tok::KwEnd)) return s;
            return s;
        }
    }
    expect(Tok::KwEnd, diags, "'end' to close if");
    return s;
}

StmtPtr Parser::parseWhileStmt(DiagnosticList& diags) {
    const Token kw = consume();
    auto s = std::make_unique<Stmt>(StmtKind::While, kw.loc);
    if (check(Tok::Identifier) && peekKind(1) == Tok::Colon) {
        s->label = std::string(consume().text);
        consume(); // ':'
    }
    s->cond = parseExpr(diags);
    expect(Tok::KwDo, diags, "'do' after while condition");
    while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        if (StmtPtr inner = parseStatement(diags)) s->body.push_back(std::move(inner));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    expect(Tok::KwEnd, diags, "'end' to close while");
    return s;
}

StmtPtr Parser::parseForStmt(DiagnosticList& diags) {
    const Token kw = consume();
    auto s = std::make_unique<Stmt>(StmtKind::For, kw.loc);
    if (check(Tok::Identifier) && peekKind(1) == Tok::Colon) {
        s->label = std::string(consume().text);
        consume();
    }
    if (!expect(Tok::Identifier, diags, "loop variable")) { synchronize(diags); return s; }
    s->varName = std::string(peek(-1).text);
    if (match(Tok::Comma)) {
        if (expect(Tok::Identifier, diags, "second loop variable")) s->varName2 = std::string(peek(-1).text);
    }
    expect(Tok::KwIn, diags, "'in' after loop variable");
    s->iterable = parseExpr(diags);
    expect(Tok::KwDo, diags, "'do' after for header");
    while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        if (StmtPtr inner = parseStatement(diags)) s->body.push_back(std::move(inner));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    expect(Tok::KwEnd, diags, "'end' to close for");
    return s;
}

StmtPtr Parser::parseReturn(DiagnosticList& diags) {
    const Token kw = consume();
    auto s = std::make_unique<Stmt>(StmtKind::Return, kw.loc);
    // ВНИМАНИЕ: здесь нельзя использовать isStatementStart() — `if`, `match`,
    // `for` и `while` являются и выражениями тоже, поэтому `return match v ...`
    // обязан разбираться как возврат значения блочного выражения.
    // `when` — мягкое ключевое слово: оно завершает ветвь match, но может быть
    // и обычным именем, поэтому здесь мы его НЕ считаем границей выражения.
    if (!check(Tok::EndOfFile) && !check(Tok::KwEnd) && !check(Tok::KwElse) && !check(Tok::KwElif))
        s->expr = parseExpr(diags);
    (void)match(Tok::Semicolon);
    return s;
}

StmtPtr Parser::parseImport(DiagnosticList& diags) {
    const Token kw = consume();
    auto s = std::make_unique<Stmt>(StmtKind::Import, kw.loc);
    if (check(Tok::String)) { s->path = consume().str; }
    else if (check(Tok::Identifier)) {
        s->path = std::string(consume().text);
        while (match(Tok::Dot)) {
            if (expect(Tok::Identifier, diags, "module path segment")) s->path += "." + std::string(peek(-1).text);
        }
    } else error(peek(), diags, "expected module path after 'import'");

    if (match(Tok::KwAs)) {
        if (expect(Tok::Identifier, diags, "import alias")) s->alias = std::string(peek(-1).text);
    }
    match(Tok::Semicolon);
    return s;
}

std::unique_ptr<Pattern> Parser::parsePattern(DiagnosticList& diags) {
    auto p = std::make_unique<Pattern>();
    p->loc = peek().loc;
    if (match(Tok::Star)) { p->kind = Pattern::Kind::Wildcard; return p; }
    if (check(Tok::LBracket)) {
        consume();
        p->kind = Pattern::Kind::Array;
        while (!check(Tok::RBracket) && !check(Tok::EndOfFile)) {
            p->sub.push_back(parsePattern(diags));
            if (!match(Tok::Comma)) break;
        }
        expect(Tok::RBracket, diags, "']'");
        return p;
    }
    if (check(Tok::LBrace)) {
        consume();
        p->kind = Pattern::Kind::Map;
        while (!check(Tok::RBrace) && !check(Tok::EndOfFile)) {
            if (check(Tok::String)) p->keys.push_back(consume().str);
            else if (check(Tok::Identifier)) p->keys.push_back(std::string(consume().text));
            else { error(peek(), diags, "expected key in map pattern"); break; }
            expect(Tok::Colon, diags, "':'");
            p->sub.push_back(parsePattern(diags));
            if (!match(Tok::Comma)) break;
        }
        expect(Tok::RBrace, diags, "'}'");
        return p;
    }
    if (check(Tok::KwIs)) {
        consume();
        p->kind = Pattern::Kind::Type;
        p->type = parseType(diags);
        return p;
    }
    if (check(Tok::Identifier) && peek().text == "_") {
        consume();
        p->kind = Pattern::Kind::Wildcard;
        return p;
    }
    if (check(Tok::Identifier) && peekKind(1) == Tok::KwIs) {
        p->binding = std::string(consume().text);
        consume(); // is
        p->kind = Pattern::Kind::Type;
        p->type = parseType(diags);
        return p;
    }
    // Литерал или binding
    if (check(Tok::Number) || check(Tok::String) || check(Tok::KwTrue) ||
        check(Tok::KwFalse) || check(Tok::KwNil) || check(Tok::Minus)) {
        p->kind = Pattern::Kind::Literal;
        p->literal = parsePrimary(diags);
        return p;
    }
    if (check(Tok::Identifier)) {
        p->kind = Pattern::Kind::Binding;
        p->binding = std::string(consume().text);
        return p;
    }
    error(peek(), diags, "expected a pattern");
    p->kind = Pattern::Kind::Wildcard;
    return p;
}

std::unique_ptr<MatchCase> Parser::parseMatchCase(DiagnosticList& diags) {
    auto c = std::make_unique<MatchCase>();
    c->loc = peek().loc;
    expect(Tok::KwWhen, diags, "'when'");
    c->pattern = parsePattern(diags);
    if (check(Tok::KwWhen)) { // when <pattern> when <guard>  — используем 'if'
        consume();
        c->guard = parseExpr(diags);
    } else if (match(Tok::KwIf)) {
        c->guard = parseExpr(diags);
    }
    expect(Tok::FatArrow, diags, "'=>' after match pattern");
    if (check(Tok::KwDo)) {
        consume();
        while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
            const std::size_t before = pos_;
            if (StmtPtr inner = parseStatement(diags)) c->body.push_back(std::move(inner));
            if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
        }
        expect(Tok::KwEnd, diags, "'end'");
    } else if (check(Tok::LBrace)) {
        consume();
        while (!check(Tok::RBrace) && !check(Tok::EndOfFile)) {
            const std::size_t before = pos_;
            if (StmtPtr inner = parseStatement(diags)) c->body.push_back(std::move(inner));
            if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
        }
        expect(Tok::RBrace, diags, "'}'");
    } else {
        auto s = std::make_unique<Stmt>(StmtKind::ExprStmt, peek().loc);
        s->expr = parseExpr(diags);
        c->body.push_back(std::move(s));
    }
    match(Tok::Comma);
    return c;
}

StmtPtr Parser::parseMatchStmt(DiagnosticList& diags) {
    const Token kw = consume();
    auto s = std::make_unique<Stmt>(StmtKind::Match, kw.loc);
    s->expr = parseExpr(diags);
    while (check(Tok::KwWhen)) s->cases.push_back(parseMatchCase(diags));
    match(Tok::KwEnd);
    return s;
}

StmtPtr Parser::parseBlock(DiagnosticList& diags) {
    const Token kw = consume(); // do
    auto s = std::make_unique<Stmt>(StmtKind::Block, kw.loc);
    while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        if (StmtPtr inner = parseStatement(diags)) s->body.push_back(std::move(inner));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    expect(Tok::KwEnd, diags, "'end'");
    return s;
}

// ---------------------------------------------------------------------------
//  Выражения
// ---------------------------------------------------------------------------
ExprPtr Parser::parseExpr(DiagnosticList& diags) { return parseAssignment(diags); }

ExprPtr Parser::parseAssignment(DiagnosticList& diags) {
    ExprPtr left = parseTernary(diags);
    if (check(Tok::Eq)) {
        const SourceLoc loc = consume().loc;
        ExprPtr right = parseAssignment(diags);
        auto e = std::make_unique<Expr>(ExprKind::Assign, loc);
        e->a = std::move(left);
        e->b = std::move(right);
        return e;
    }
    if (auto op = compoundFor(peekKind())) {
        const SourceLoc loc = consume().loc;
        ExprPtr right = parseAssignment(diags);
        auto e = std::make_unique<Expr>(ExprKind::CompoundAssign, loc);
        e->cmpOp = *op;
        e->a = std::move(left);
        e->b = std::move(right);
        return e;
    }
    return left;
}

ExprPtr Parser::parseTernary(DiagnosticList& diags) {
    // minPrec = 0: ниже по цепочке parseBinary сам отфильтрует операторы по
    // приоритету. Раньше здесь стояло 5, из-за чего оператор `or` (приоритет 4)
    // не разбирался ВООБЩЕ: `if a or b do ...` падал с «expected 'do' after if
    // condition, found 'or'», а `let x = a or b` превращал `or` в имя переменной.
    ExprPtr cond = parseBinary(diags, 0);
    if (match(Tok::Question)) {
        const SourceLoc loc = peek(-1).loc;
        auto e = std::make_unique<Expr>(ExprKind::Ternary, loc);
        e->a = std::move(cond);
        e->b = parseExpr(diags);
        expect(Tok::Colon, diags, "':' in ternary expression");
        e->c = parseExpr(diags);
        return e;
    }
    if (match(Tok::QuestionColon)) {
        // Элвис: `a ?: b` — значение `a`, если оно истинно, иначе `b`.
        // Компилируется в одну инструкцию Op::Elvis (без повторного вычисления `a`).
        const SourceLoc loc = peek(-1).loc;
        auto e = std::make_unique<Expr>(ExprKind::Ternary, loc);
        e->optionalChain = true;   // маркер элвиса
        e->a = std::move(cond);
        e->c = parseExpr(diags);
        return e;
    }
    return cond;
}

ExprPtr Parser::parseBinary(DiagnosticList& diags, int minPrec) {
    ExprPtr left = parseUnary(diags);
    for (;;) {
        const Tok k = peekKind();
        // Логические операторы
        if (minPrec <= 4 && (k == Tok::KwOr || k == Tok::PipePipe)) {
            const SourceLoc loc = consume().loc;
            auto e = std::make_unique<Expr>(ExprKind::Logical, loc);
            e->name = "or";
            e->a = std::move(left);
            e->b = parseBinary(diags, 5);
            left = std::move(e);
            continue;
        }
        if (minPrec <= 6 && (k == Tok::KwAnd || k == Tok::AmpAmp)) {
            const SourceLoc loc = consume().loc;
            auto e = std::make_unique<Expr>(ExprKind::Logical, loc);
            e->name = "and";
            e->a = std::move(left);
            e->b = parseBinary(diags, 7);
            left = std::move(e);
            continue;
        }
        if (minPrec <= 9 && k == Tok::KwIs) {
            const SourceLoc loc = consume().loc;
            auto e = std::make_unique<Expr>(ExprKind::Is, loc);
            e->a = std::move(left);
            e->type = parseType(diags);
            left = std::move(e);
            continue;
        }
        if (minPrec <= 14 && (k == Tok::DotDot || k == Tok::DotDotEq)) {
            const SourceLoc loc = consume().loc;
            auto e = std::make_unique<Expr>(ExprKind::Range, loc);
            e->inclusiveRange = (k == Tok::DotDotEq);
            e->a = std::move(left);
            e->b = parseBinary(diags, 15);
            return e; // range не ассоциативен
        }
        auto info = binInfoFor(k);
        if (!info || info->prec < minPrec) break;
        const SourceLoc loc = consume().loc;
        ExprPtr right = parseBinary(diags, (k == Tok::StarStar) ? info->prec : info->prec + 1);
        auto e = std::make_unique<Expr>(ExprKind::Binary, loc);
        e->binOp = info->op;
        e->a = std::move(left);
        e->b = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseUnary(DiagnosticList& diags) {
    const Tok k = peekKind();
    if (k == Tok::Minus || k == Tok::Bang || k == Tok::KwNot || k == Tok::Tilde) {
        const SourceLoc loc = consume().loc;
        auto e = std::make_unique<Expr>(ExprKind::Unary, loc);
        e->unOp = (k == Tok::Minus) ? UnOp::Neg
                : (k == Tok::Tilde) ? UnOp::BitNot
                                    : UnOp::Not;
        e->a = parseUnary(diags);
        return e;
    }
    if (k == Tok::KwYield) {
        const SourceLoc loc = consume().loc;
        auto e = std::make_unique<Expr>(ExprKind::Yield, loc);
        if (!check(Tok::EndOfFile) && !check(Tok::KwEnd) && !check(Tok::KwElse) &&
            !check(Tok::KwElif) && !check(Tok::KwWhen))
            e->a = parseExpr(diags);
        return e;
    }
    if (k == Tok::KwResume) {
        const SourceLoc loc = consume().loc;
        auto e = std::make_unique<Expr>(ExprKind::Resume, loc);
        e->a = parseExpr(diags);
        return e;
    }
    if (k == Tok::KwTry) {
        const SourceLoc loc = consume().loc;
        auto e = std::make_unique<Expr>(ExprKind::TryResult, loc);
        e->a = parseUnary(diags);
        return e;
    }
    return parsePostfix(diags);
}

ExprPtr Parser::parsePostfix(DiagnosticList& diags) {
    ExprPtr e = parsePrimary(diags);
    for (;;) {
        const SourceLoc loc = peek().loc;
        if (match(Tok::Dot) || match(Tok::QuestionDot)) {
            const bool opt = (peek(-1).kind == Tok::QuestionDot);
            std::string member;
            if (!consumeName(diags, member, "member name after '.'")) break;
            // `super.method(...)` — особый случай: это MethodCall с receiver'ом Super,
            // а не Call(Member(...)). Иначе компилятор не сможет отличить его от
            // обычного вызова и сгенерирует рекурсивный вызов собственного метода.
            if (check(Tok::LParen) && e->kind == ExprKind::Super) {
                auto m = std::make_unique<Expr>(ExprKind::MethodCall, loc);
                m->a = std::move(e);
                m->name = member;
                consume();
                if (!check(Tok::RParen)) {
                    m->args.push_back(parseExpr(diags));
                    while (match(Tok::Comma)) {
                        if (check(Tok::RParen)) break;
                        m->args.push_back(parseExpr(diags));
                    }
                }
                expect(Tok::RParen, diags, "')'");
                e = std::move(m);
                continue;
            }
            if (check(Tok::LParen)) {
                auto m = std::make_unique<Expr>(ExprKind::MethodCall, loc);
                m->optionalChain = opt;
                m->a = std::move(e);
                m->name = member;
                consume(); // '('
                if (!check(Tok::RParen)) {
                    m->args.push_back(parseExpr(diags));
                    while (match(Tok::Comma)) {
                        if (check(Tok::RParen)) break;
                        m->args.push_back(parseExpr(diags));
                    }
                }
                expect(Tok::RParen, diags, "')'");
                if (match(Tok::DotDotDot)) m->variadic = true;
                e = std::move(m);
            } else {
                auto mem = std::make_unique<Expr>(ExprKind::Member, loc);
                mem->optionalChain = opt;
                mem->a = std::move(e);
                mem->name = member;
                e = std::move(mem);
            }
            continue;
        }
        if (match(Tok::LBracket)) {
            auto idx = std::make_unique<Expr>(ExprKind::Index, loc);
            idx->a = std::move(e);
            idx->b = parseExpr(diags);
            expect(Tok::RBracket, diags, "']'");
            e = std::move(idx);
            continue;
        }
        if (check(Tok::LParen)) {
            auto call = std::make_unique<Expr>(ExprKind::Call, loc);
            call->a = std::move(e);
            consume();
            if (!check(Tok::RParen)) {
                call->args.push_back(parseExpr(diags));
                while (match(Tok::Comma)) {
                    if (check(Tok::RParen)) break;
                    call->args.push_back(parseExpr(diags));
                }
            }
            expect(Tok::RParen, diags, "')'");
            if (match(Tok::DotDotDot)) call->variadic = true;
            e = std::move(call);
            continue;
        }
        if (match(Tok::KwAs)) {
            auto cast = std::make_unique<Expr>(ExprKind::As, loc);
            cast->a = std::move(e);
            cast->type = parseType(diags);
            e = std::move(cast);
            continue;
        }
        if (match(Tok::Question)) {
            // Result propagation: `expr?`
            auto p = std::make_unique<Expr>(ExprKind::Cast, loc);
            p->name = "?";
            p->a = std::move(e);
            e = std::move(p);
            continue;
        }
        break;
    }
    return e;
}

bool Parser::startsLambda() const noexcept {
    if (peekKind() == Tok::Pipe) return true;
    if (peekKind() != Tok::LParen) return false;
    // ( ) => ...
    if (peekKind(1) == Tok::RParen) return peekKind(2) == Tok::FatArrow || peekKind(2) == Tok::Arrow;
    if (peekKind(1) != Tok::Identifier) return false;
    const Tok after = peekKind(2);
    if (after != Tok::Comma && after != Tok::RParen && after != Tok::Colon && after != Tok::Eq)
        return false;
    // Сканируем до ')' и смотрим, что дальше.
    int depth = 0;
    for (std::size_t i = pos_; i < toks_.size(); ++i) {
        if (toks_[i].kind == Tok::LParen) ++depth;
        else if (toks_[i].kind == Tok::RParen) {
            --depth;
            if (depth == 0) {
                const Tok nxt = (i + 1 < toks_.size()) ? toks_[i + 1].kind : Tok::EndOfFile;
                return nxt == Tok::FatArrow || nxt == Tok::Arrow || nxt == Tok::KwDo;
            }
        }
    }
    return false;
}

void Parser::makeLastExprImplicitReturn(std::vector<StmtPtr>& body) {
    if (body.empty()) return;
    Stmt* last = body.back().get();
    if (last->kind != StmtKind::ExprStmt || !last->expr) return;
    auto ret = std::make_unique<Stmt>(StmtKind::Return, last->loc);
    ret->expr = std::move(last->expr);
    body.back() = std::move(ret);
}

ExprPtr Parser::parseLambda(DiagnosticList& diags) {
    const SourceLoc loc = peek().loc;
    auto e = std::make_unique<Expr>(ExprKind::Lambda, loc);

    // 1) Параметры: `|a, b|` или `(a, b: Int) -> Ret`.
    if (match(Tok::Pipe)) {
        if (!check(Tok::Pipe)) {
            do {
                Param p;
                p.loc = peek().loc;
                if (!consumeName(diags, p.name, "parameter name")) break;
                if (match(Tok::Colon)) p.type = parseType(diags);
                if (match(Tok::Eq)) p.defaultValue = parseExpr(diags);
                e->params.push_back(std::move(p));
            } while (match(Tok::Comma));
        }
        expect(Tok::Pipe, diags, "'|' to close lambda parameters");
    } else {
        e->params = parseParams(diags);
        if (match(Tok::Arrow)) e->returnType = parseType(diags);
    }

    // 2) Тело. Поддерживаются формы:
    //      |x| expr                     (неявный return)
    //      |x| => expr
    //      |x| => { stmts }             (блок, значение = последнее выражение)
    //      |x| do stmts end
    //      (a) -> T do stmts end
    (void)match(Tok::FatArrow);

    if (check(Tok::LBrace)) {
        consume();
        e->isBlockBody = true;
        while (!check(Tok::RBrace) && !check(Tok::EndOfFile)) {
            const std::size_t before = pos_;
            if (StmtPtr inner = parseStatement(diags)) e->body.push_back(std::move(inner));
            if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
        }
        expect(Tok::RBrace, diags, "'}' to close the lambda body");
        makeLastExprImplicitReturn(e->body);
    } else if (check(Tok::KwDo)) {
        consume();
        e->isBlockBody = true;
        while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
            const std::size_t before = pos_;
            if (StmtPtr inner = parseStatement(diags)) e->body.push_back(std::move(inner));
            if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
        }
        expect(Tok::KwEnd, diags, "'end' to close the lambda body");
    } else {
        // Одно выражение => неявный возврат.
        e->isBlockBody = false;
        auto ret = std::make_unique<Stmt>(StmtKind::Return, peek().loc);
        ret->expr = parseExpr(diags);
        e->body.push_back(std::move(ret));
    }
    return e;
}

ExprPtr Parser::parseIfExpr(DiagnosticList& diags) {
    const Token kw = consume();
    auto e = std::make_unique<Expr>(ExprKind::If, kw.loc);
    e->a = parseExpr(diags);
    expect(Tok::KwDo, diags, "'do' after if condition");
    while (!check(Tok::KwEnd) && !check(Tok::KwElif) && !check(Tok::KwElse) && !check(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        if (StmtPtr inner = parseStatement(diags)) e->stmts.push_back(std::move(inner));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    while (match(Tok::KwElif)) {
        e->elifConds.push_back(parseExpr(diags));
        expect(Tok::KwDo, diags, "'do' after elif condition");
        std::vector<StmtPtr> branch;
        while (!check(Tok::KwEnd) && !check(Tok::KwElif) && !check(Tok::KwElse) && !check(Tok::EndOfFile)) {
            const std::size_t before = pos_;
            if (StmtPtr inner = parseStatement(diags)) branch.push_back(std::move(inner));
            if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
        }
        e->elifBodies.push_back(std::move(branch));
    }
    if (match(Tok::KwElse)) {
        if (check(Tok::KwIf)) {
            auto nested = parseIfExpr(diags);
            auto wrap = std::make_unique<Stmt>(StmtKind::ExprStmt, nested->loc);
            wrap->expr = std::move(nested);
            e->elseBody.push_back(std::move(wrap));
            return e;   // у вложенного if свой `end`
        } else if (check(Tok::KwDo)) {
            consume();
            while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
                const std::size_t before = pos_;
                if (StmtPtr inner = parseStatement(diags)) e->elseBody.push_back(std::move(inner));
                if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
            }
            // Единый `end` закрывает весь if (см. parseIfStmt).
        } else {
            if (StmtPtr inner = parseStatement(diags)) e->elseBody.push_back(std::move(inner));
            if (check(Tok::KwEnd)) consume();
            return e;
        }
    }
    expect(Tok::KwEnd, diags, "'end' to close if");
    return e;
}

ExprPtr Parser::parseWhileExpr(DiagnosticList& diags) {
    const Token kw = consume();
    auto e = std::make_unique<Expr>(ExprKind::While, kw.loc);
    e->a = parseExpr(diags);
    expect(Tok::KwDo, diags, "'do' after while condition");
    while (!check(Tok::KwEnd) && !check(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        if (StmtPtr inner = parseStatement(diags)) e->stmts.push_back(std::move(inner));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    expect(Tok::KwEnd, diags, "'end'");
    return e;
}

ExprPtr Parser::parseForExpr(DiagnosticList& diags, bool requireEnd) {
    const Token kw = consume();
    auto e = std::make_unique<Expr>(ExprKind::For, kw.loc);
    if (!expect(Tok::Identifier, diags, "loop variable")) return e;
    e->name = std::string(peek(-1).text);
    if (match(Tok::Comma)) {
        if (expect(Tok::Identifier, diags, "second loop variable")) e->strs.push_back(std::string(peek(-1).text));
    }
    expect(Tok::KwIn, diags, "'in'");
    e->a = parseExpr(diags);
    expect(Tok::KwDo, diags, "'do'");
    while (!check(Tok::KwEnd) && !check(Tok::EndOfFile) &&
           !(requireEnd == false && check(Tok::RBracket))) {
        const std::size_t before = pos_;
        if (StmtPtr inner = parseStatement(diags)) e->stmts.push_back(std::move(inner));
        if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
    }
    // В list comprehension `[for x in c do expr]` закрывающая `]` заменяет `end`.
    if (!match(Tok::KwEnd) && requireEnd) expect(Tok::KwEnd, diags, "'end'");
    return e;
}

ExprPtr Parser::parseMatchExpr(DiagnosticList& diags) {
    const Token kw = consume();
    auto e = std::make_unique<Expr>(ExprKind::Match, kw.loc);
    e->a = parseExpr(diags);
    while (check(Tok::KwWhen)) e->cases.push_back(parseMatchCase(diags));
    match(Tok::KwEnd);
    return e;
}

ExprPtr Parser::parseStringWithInterp(const Token& first, DiagnosticList& diags) {
    // Поток токенов интерполированной строки:
    //   String(prefix, interp=true) InterpBegin <expr> InterpEnd String(part) [InterpBegin ...] String(tail)
    // `first` — начальный String; текущий токен — первый InterpBegin.
    auto e = std::make_unique<Expr>(ExprKind::InterpolatedString, first.loc);
    e->strs.push_back(first.str);
    while (match(Tok::InterpBegin)) {
        e->args.push_back(parseExpr(diags));
        expect(Tok::InterpEnd, diags, "'}' to close the interpolation");
        if (check(Tok::String)) e->strs.push_back(consume().str);
        else e->strs.emplace_back();
    }
    return e;
}

ExprPtr Parser::parsePrimary(DiagnosticList& diags) {
    const Token t = peek();
    switch (t.kind) {
        case Tok::KwNil: {
            consume();
            return std::make_unique<Expr>(ExprKind::Nil, t.loc);
        }
        case Tok::KwTrue: case Tok::KwFalse: {
            consume();
            auto e = std::make_unique<Expr>(ExprKind::Bool, t.loc);
            e->boolean = (t.kind == Tok::KwTrue);
            return e;
        }
        case Tok::Number: {
            consume();
            auto e = std::make_unique<Expr>(ExprKind::Number, t.loc);
            e->number = t.number;
            return e;
        }
        case Tok::String: {
            consume();
            if (t.interp && check(Tok::InterpBegin)) return parseStringWithInterp(t, diags);
            auto e = std::make_unique<Expr>(ExprKind::String, t.loc);
            e->name = t.str;
            return e;
        }
        case Tok::InterpBegin: {
            // Лексер открывает интерполированную строку токеном InterpBegin,
            // в поле `str` которого лежит литеральная часть до первого '{'.
            consume();
            auto e = std::make_unique<Expr>(ExprKind::InterpolatedString, t.loc);
            e->strs.push_back(t.str);
            for (;;) {
                e->args.push_back(parseExpr(diags));
                expect(Tok::InterpEnd, diags, "'}' to close the interpolation");
                if (check(Tok::String)) { e->strs.push_back(consume().str); break; }
                if (!match(Tok::InterpBegin)) { e->strs.emplace_back(); break; }
                e->strs.push_back(peek(-1).str);
            }
            return e;
        }
        case Tok::Identifier:
        case Tok::KwCoroutine:
        case Tok::KwResume: {
            // `resume` и `coroutine` — одновременно ключевые слова и имена
            // функций стандартной библиотеки: в позиции выражения без аргументов
            // они трактуются как обычные идентификаторы.
            consume();
            auto e = std::make_unique<Expr>(ExprKind::Identifier, t.loc);
            e->name = std::string(t.text);
            return e;
        }
        case Tok::KwThis: {
            consume();
            return std::make_unique<Expr>(ExprKind::This, t.loc);
        }
        case Tok::KwSuper: {
            consume();
            auto e = std::make_unique<Expr>(ExprKind::Super, t.loc);
            expect(Tok::Dot, diags, "'.' after 'super'");
            if (expect(Tok::Identifier, diags, "method name after 'super.'")) e->name = std::string(peek(-1).text);
            return e;
        }
        case Tok::KwNew: {
            consume();
            auto e = std::make_unique<Expr>(ExprKind::New, t.loc);
            if (!expect(Tok::Identifier, diags, "class name after 'new'")) return e;
            e->name = std::string(peek(-1).text);
            if (match(Tok::LParen)) {
                if (!check(Tok::RParen)) {
                    e->args.push_back(parseExpr(diags));
                    while (match(Tok::Comma)) {
                        if (check(Tok::RParen)) break;
                        e->args.push_back(parseExpr(diags));
                    }
                }
                expect(Tok::RParen, diags, "')'");
            }
            return e;
        }
        case Tok::KwIf:     return parseIfExpr(diags);
        case Tok::KwWhile:  return parseWhileExpr(diags);
        case Tok::KwFor:    return parseForExpr(diags);
        case Tok::KwMatch:  return parseMatchExpr(diags);
        case Tok::LParen: {
            if (startsLambda()) return parseLambda(diags);
            consume();
            ExprPtr first = parseExpr(diags);
            if (match(Tok::Comma)) {
                auto tup = std::make_unique<Expr>(ExprKind::TupleLit, t.loc);
                tup->args.push_back(std::move(first));
                do {
                    if (check(Tok::RParen)) break;
                    tup->args.push_back(parseExpr(diags));
                } while (match(Tok::Comma));
                expect(Tok::RParen, diags, "')'");
                return tup;
            }
            expect(Tok::RParen, diags, "')'");
            auto g = std::make_unique<Expr>(ExprKind::Group, t.loc);
            g->a = std::move(first);
            return g;
        }
        case Tok::LBracket: {
            consume();
            // List comprehension: `[for x in coll do expr]`.
            if (check(Tok::KwFor)) {
                ExprPtr fe = parseForExpr(diags, /*requireEnd=*/false);
                expect(Tok::RBracket, diags, "']' to close the list comprehension");
                return fe;
            }
            auto arr = std::make_unique<Expr>(ExprKind::ArrayLit, t.loc);
            if (!check(Tok::RBracket)) {
                for (;;) {
                    if (check(Tok::RBracket)) break;
                    if (match(Tok::DotDotDot)) {
                        auto sp = std::make_unique<Expr>(ExprKind::Spread, peek().loc);
                        sp->a = parseExpr(diags);
                        arr->args.push_back(std::move(sp));
                    } else {
                        arr->args.push_back(parseExpr(diags));
                    }
                    if (!match(Tok::Comma)) break;
                }
            }
            expect(Tok::RBracket, diags, "']'");
            return arr;
        }
        case Tok::LBrace: {
            consume();
            auto m = std::make_unique<Expr>(ExprKind::MapLit, t.loc);
            if (!check(Tok::RBrace)) {
                for (;;) {
                    if (check(Tok::RBrace)) break;
                    ExprPtr key;
                    if ((check(Tok::Identifier) || isSoftKeyword(peekKind())) &&
                        (peekKind(1) == Tok::Colon || peekKind(1) == Tok::Eq)) {
                        Token id = consume();
                        key = std::make_unique<Expr>(ExprKind::String, id.loc);
                        key->name = std::string(id.text);
                        consume(); // ':' or '='
                    } else if (check(Tok::LBracket)) {
                        consume();
                        key = parseExpr(diags);
                        expect(Tok::RBracket, diags, "']'");
                        expect(Tok::Colon, diags, "':'");
                    } else {
                        key = parseExpr(diags);
                        expect(Tok::Colon, diags, "':' in map literal");
                    }
                    m->keys.push_back(std::move(key));
                    m->args.push_back(parseExpr(diags));   // ':' уже потреблён выше
                    if (!match(Tok::Comma)) break;
                }
            }
            expect(Tok::RBrace, diags, "'}'");
            return m;
        }
        case Tok::Pipe:
            // `|x| expr` — лямбда; одиночный `|` в префиксной позиции невозможен.
            return parseLambda(diags);
        case Tok::PipePipe: {
            // `|| expr` — лямбда без аргументов. Лексер склеивает два `|`
            // в один токен PipePipe, поэтому нужна отдельная ветка.
            consume();
            auto lam = std::make_unique<Expr>(ExprKind::Lambda, t.loc);
            if (check(Tok::LBrace)) {
                // `|| { stmts }` — блочное тело (значение = последнее выражение).
                // Без этой ветки `{` после `||` разбирался как ЛИТЕРАЛ MAP'а, и
                // `{"enter": || { onEnter() }}` падал с «expected ':' in map
                // literal» — то есть обработчики состояний FSM было невозможно
                // записать короткой лямбдой.
                consume();
                lam->isBlockBody = true;
                while (!check(Tok::RBrace) && !check(Tok::EndOfFile)) {
                    const std::size_t before = pos_;
                    if (StmtPtr inner = parseStatement(diags)) lam->body.push_back(std::move(inner));
                    if (pos_ == before) { synchronize(diags); if (pos_ == before) ++pos_; }
                }
                expect(Tok::RBrace, diags, "'}' to close the lambda body");
                makeLastExprImplicitReturn(lam->body);
                return lam;
            }
            auto ret = std::make_unique<Stmt>(StmtKind::Return, peek().loc);
            ret->expr = parseExpr(diags);
            lam->body.push_back(std::move(ret));
            return lam;
        }
        case Tok::Minus: {
            // Унарный минус уже обработан в parseUnary; сюда попадаем для литералов в паттернах.
            consume();
            auto inner = parsePrimary(diags);
            auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
            e->unOp = UnOp::Neg;
            e->a = std::move(inner);
            return e;
        }
        default:
            if (isSoftKeyword(t.kind)) {
                // Мягкие ключевые слова (`when`, `is`, `as`, `in`, `and`, ...)
                // разрешены в качестве имён: они неоднозначны только в
                // специфических позициях, которые обработаны выше.
                consume();
                auto e = std::make_unique<Expr>(ExprKind::Identifier, t.loc);
                e->name = std::string(t.text);
                return e;
            }
            error(t, diags, format("unexpected {}", t.describe()),
                  "expressions cannot start with this token");
            consume();
            return std::make_unique<Expr>(ExprKind::Nil, t.loc);
    }
}

// ---------------------------------------------------------------------------
Module parseSource(std::string_view src, SourceName name, DiagnosticList& diags) {
    Lexer lex(src, name);
    auto tokens = lex.tokenize(diags);
    Parser p(std::move(tokens), std::move(name));
    return p.parseModule(diags);
}

} // namespace lv
