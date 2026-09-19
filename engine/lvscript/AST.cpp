/**
 * @file    AST.cpp
 * @brief   Печать типов и дамп AST (используется отладчиком и Visual Scripting).
 */
#include "AST.h"

#include <sstream>

namespace lv {

std::string TypeExpr::toString() const {
    switch (kind) {
        case Kind::Infer: return "_";
        case Kind::Named: return name;
        case Kind::Any:   return "Any";
        case Kind::Self:  return "Self";
        case Kind::Nil:   return "Nil";
        case Kind::Array: return params.empty() ? "[]" : ("[" + params[0]->toString() + "]");
        case Kind::Map:
            return params.size() == 2 ? ("Map<" + params[0]->toString() + ", " + params[1]->toString() + ">")
                                      : "Map<?,?>";
        case Kind::Optional:
            return params.empty() ? "?" : (params[0]->toString() + "?");
        case Kind::Tuple: {
            std::string s = "(";
            for (std::size_t i = 0; i < params.size(); ++i) { if (i) s += ", "; s += params[i]->toString(); }
            return s + ")";
        }
        case Kind::Func: {
            std::string s = "(";
            for (std::size_t i = 0; i < params.size(); ++i) { if (i) s += ", "; s += params[i]->toString(); }
            s += ") -> ";
            s += ret ? ret->toString() : "_";
            return s;
        }
    }
    return "?";
}

std::string Type::toString() const {
    switch (kind) {
        case Kind::Unknown: return resolved && resolves ? resolves->toString() : format("?{}", id);
        case Kind::Int:     return "Int";
        case Kind::Float:   return "Float";
        case Kind::Bool:    return "Bool";
        case Kind::String:  return "String";
        case Kind::Nil:     return "Nil";
        case Kind::Any:     return "Any";
        case Kind::Vec2:    return "Vec2";
        case Kind::Vec3:    return "Vec3";
        case Kind::Quat:    return "Quat";
        case Kind::Color:   return "Color";
        case Kind::Entity:  return "Entity";
        case Kind::Module:  return "Module(" + name + ")";
        case Kind::Class:   return name;
        case Kind::Array:   return args.empty() ? "[Any]" : ("[" + args[0].toString() + "]");
        case Kind::Map:
            return args.size() == 2 ? ("Map<" + args[0].toString() + ", " + args[1].toString() + ">")
                                    : "Map<Any, Any>";
        case Kind::Optional:
            return args.empty() ? "Any?" : (args[0].toString() + "?");
        case Kind::Tuple: {
            std::string s = "(";
            for (std::size_t i = 0; i < args.size(); ++i) { if (i) s += ", "; s += args[i].toString(); }
            return s + ")";
        }
        case Kind::Func: {
            std::string s = "(";
            for (std::size_t i = 0; i < args.size(); ++i) { if (i) s += ", "; s += args[i].toString(); }
            s += ") -> ";
            s += ret ? ret->toString() : "Any";
            return s;
        }
    }
    return "?";
}

// ---------------------------------------------------------------------------
namespace {

const char* exprKindName(ExprKind k) {
    switch (k) {
        case ExprKind::Nil: return "Nil";
        case ExprKind::Bool: return "Bool";
        case ExprKind::Number: return "Number";
        case ExprKind::String: return "String";
        case ExprKind::InterpolatedString: return "InterpString";
        case ExprKind::Identifier: return "Ident";
        case ExprKind::This: return "This";
        case ExprKind::Super: return "Super";
        case ExprKind::Unary: return "Unary";
        case ExprKind::Binary: return "Binary";
        case ExprKind::Logical: return "Logical";
        case ExprKind::Ternary: return "Ternary";
        case ExprKind::Coalesce: return "Coalesce";
        case ExprKind::Call: return "Call";
        case ExprKind::MethodCall: return "MethodCall";
        case ExprKind::Index: return "Index";
        case ExprKind::Member: return "Member";
        case ExprKind::Assign: return "Assign";
        case ExprKind::CompoundAssign: return "CompoundAssign";
        case ExprKind::Lambda: return "Lambda";
        case ExprKind::ArrayLit: return "ArrayLit";
        case ExprKind::MapLit: return "MapLit";
        case ExprKind::TupleLit: return "TupleLit";
        case ExprKind::Spread: return "Spread";
        case ExprKind::New: return "New";
        case ExprKind::Is: return "Is";
        case ExprKind::As: return "As";
        case ExprKind::Cast: return "Cast";
        case ExprKind::Range: return "Range";
        case ExprKind::Group: return "Group";
        case ExprKind::If: return "IfExpr";
        case ExprKind::While: return "WhileExpr";
        case ExprKind::For: return "ForExpr";
        case ExprKind::Match: return "MatchExpr";
        case ExprKind::Block: return "BlockExpr";
        case ExprKind::Yield: return "Yield";
        case ExprKind::Resume: return "Resume";
        case ExprKind::TryResult: return "Try";
        case ExprKind::Await: return "Await";
        case ExprKind::IndexAssign: return "IndexAssign";
        case ExprKind::MemberAssign: return "MemberAssign";
    }
    return "?";
}

const char* stmtKindName(StmtKind k) {
    switch (k) {
        case StmtKind::Let: return "Let";
        case StmtKind::ExprStmt: return "Expr";
        case StmtKind::Block: return "Block";
        case StmtKind::If: return "If";
        case StmtKind::While: return "While";
        case StmtKind::For: return "For";
        case StmtKind::Return: return "Return";
        case StmtKind::Break: return "Break";
        case StmtKind::Continue: return "Continue";
        case StmtKind::Func: return "Func";
        case StmtKind::Class: return "Class";
        case StmtKind::Coroutine: return "Coroutine";
        case StmtKind::Import: return "Import";
        case StmtKind::Match: return "Match";
        case StmtKind::Yield: return "Yield";
        case StmtKind::Resume: return "Resume";
        case StmtKind::Global: return "Global";
        case StmtKind::Pass: return "Pass";
    }
    return "?";
}

struct Dumper {
    std::ostringstream os;
    int depth = 0;

    void indent() { for (int i = 0; i < depth; ++i) os << "  "; }
    void line(std::string_view s) { indent(); os << s << "\n"; }

    void dump(const StmtPtr& s) {
        if (!s) { line("(null-stmt)"); return; }
        indent();
        os << stmtKindName(s->kind) << " @" << s->loc.line << ":" << s->loc.column;
        if (!s->name.empty())      os << " name=" << s->name;
        if (!s->names.empty())     { os << " names=["; for (auto& n : s->names) os << n << ","; os << "]"; }
        if (!s->varName.empty())   os << " var=" << s->varName;
        if (!s->path.empty())      os << " path=" << s->path;
        os << "\n";
        ++depth;
        if (s->cond)     { line("cond:"); dump(s->cond); }
        if (s->iterable) { line("iterable:"); dump(s->iterable); }
        if (s->value)    { line("value:"); dump(s->value); }
        if (s->expr)     { dump(s->expr); }
        for (auto& f : s->fields) {
            line("field " + f.name + (f.type ? (": " + f.type->toString()) : ""));
            if (f.initializer) { ++depth; dump(f.initializer); --depth; }
        }
        for (auto& m : s->methods) {
            line("method " + m.name);
            ++depth;
            for (auto& st : m.body) dump(st);
            --depth;
        }
        for (auto& st : s->body) dump(st);
        for (std::size_t i = 0; i < s->elifConds.size(); ++i) {
            line("elif:"); dump(s->elifConds[i]);
            ++depth; for (auto& st : s->elifBodies[i]) dump(st); --depth;
        }
        if (!s->elseBody.empty()) { line("else:"); ++depth; for (auto& st : s->elseBody) dump(st); --depth; }
        for (auto& c : s->cases) {
            line("case:");
            ++depth;
            for (auto& st : c->body) dump(st);
            --depth;
        }
        --depth;
    }

    void dump(const ExprPtr& e) {
        if (!e) { line("(null-expr)"); return; }
        indent();
        os << exprKindName(e->kind) << " @" << e->loc.line << ":" << e->loc.column;
        if (!e->name.empty()) os << " '" << e->name << "'";
        if (e->kind == ExprKind::Number) os << " = " << e->number;
        if (e->kind == ExprKind::Bool)   os << " = " << (e->boolean ? "true" : "false");
        if (e->kind == ExprKind::Binary) os << " op=" << static_cast<int>(e->binOp);
        if (e->resolvedType.kind != Type::Kind::Unknown) os << " : " << e->resolvedType.toString();
        os << "\n";
        ++depth;
        if (e->a) dump(e->a);
        if (e->b) dump(e->b);
        if (e->c) dump(e->c);
        for (auto& k : e->keys) dump(k);
        for (auto& a : e->args) dump(a);
        for (auto& st : e->stmts) dump(st);
        for (std::size_t i = 0; i < e->elifConds.size(); ++i) {
            line("elif:"); dump(e->elifConds[i]);
            ++depth; for (auto& st : e->elifBodies[i]) dump(st); --depth;
        }
        if (!e->elseBody.empty()) { line("else:"); ++depth; for (auto& st : e->elseBody) dump(st); --depth; }
        for (auto& c : e->cases) { line("case:"); ++depth; for (auto& st : c->body) dump(st); --depth; }
        for (auto& p : e->params) line("param " + p.name + (p.type ? (": " + p.type->toString()) : ""));
        for (auto& st : e->body) dump(st);
        --depth;
    }
};

} // namespace

std::string dumpAst(const Module& m) {
    Dumper d;
    d.line("module " + m.name);
    ++d.depth;
    for (auto& s : m.body) d.dump(s);
    return d.os.str();
}

} // namespace lv
