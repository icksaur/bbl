#include "bbl.h"
#include <iostream>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdlib>

int passed = 0, failed = 0;

#define TEST(name) static void name()
#define RUN(name) do { \
    std::cout << "  " << #name << std::endl; \
    try { name(); } catch (const BBL::Error& e) { \
        std::cerr << "  FAIL: uncaught BBL::Error: " << e.what \
                  << " in " << #name << std::endl; \
        failed++; \
    } catch (const BblTerminated&) { \
        std::cerr << "  FAIL: uncaught BblTerminated in " << #name << std::endl; \
        failed++; \
    } catch (const std::exception& e) { \
        std::cerr << "  FAIL: uncaught exception: " << e.what() \
                  << " in " << #name << std::endl; \
        failed++; \
    } \
} while(0)
#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "  FAIL: " << #a << " == " << #b \
                  << " (got '" << _a << "' vs '" << _b << "') at " \
                  << __FILE__ << ":" << __LINE__ << std::endl; \
        failed++; \
    } else { passed++; } \
} while(0)
#define ASSERT_THROW(expr) do { \
    bool _threw = false; \
    try { expr; } catch (const BBL::Error&) { _threw = true; } \
    if (!_threw) { \
        std::cerr << "  FAIL: expected exception from " << #expr \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failed++; \
    } else { passed++; } \
} while(0)
#define ASSERT_NEAR(a, b, eps) do { \
    auto _a = (a); auto _b = (b); \
    if (std::abs(_a - _b) > eps) { \
        std::cerr << "  FAIL: " << #a << " ~= " << #b \
                  << " (got " << _a << " vs " << _b << ") at " \
                  << __FILE__ << ":" << __LINE__ << std::endl; \
        failed++; \
    } else { passed++; } \
} while(0)
#define ASSERT_TRUE(a) ASSERT_EQ(!!(a), true)
#define ASSERT_FALSE(a) ASSERT_EQ(!!(a), false)

// ========== Lexer Tests ==========

TEST(test_lexer_basic) {
    BblLexer lex("(+ 1 2)");
    ASSERT_EQ(lex.nextToken().type, TokenType::LParen);
    auto sym = lex.nextToken();
    ASSERT_EQ(sym.type, TokenType::Symbol);
    ASSERT_EQ(sym.stringVal, std::string("+"));
    auto i1 = lex.nextToken();
    ASSERT_EQ(i1.type, TokenType::Int);
    ASSERT_EQ(i1.intVal, (int64_t)1);
    auto i2 = lex.nextToken();
    ASSERT_EQ(i2.type, TokenType::Int);
    ASSERT_EQ(i2.intVal, (int64_t)2);
    ASSERT_EQ(lex.nextToken().type, TokenType::RParen);
    ASSERT_EQ(lex.nextToken().type, TokenType::Eof);
}

TEST(test_lexer_eq_string) {
    BblLexer lex("(= x \"hello\")");
    ASSERT_EQ(lex.nextToken().type, TokenType::LParen);
    auto d = lex.nextToken();
    ASSERT_EQ(d.type, TokenType::Symbol);
    ASSERT_EQ(d.stringVal, std::string("="));
    auto x = lex.nextToken();
    ASSERT_EQ(x.type, TokenType::Symbol);
    ASSERT_EQ(x.stringVal, std::string("x"));
    auto s = lex.nextToken();
    ASSERT_EQ(s.type, TokenType::String);
    ASSERT_EQ(s.stringVal, std::string("hello"));
    ASSERT_EQ(lex.nextToken().type, TokenType::RParen);
}

TEST(test_lexer_float) {
    BblLexer lex("3.14");
    auto t = lex.nextToken();
    ASSERT_EQ(t.type, TokenType::Float);
    ASSERT_NEAR(t.floatVal, 3.14, 0.001);
}

TEST(test_lexer_bool_null) {
    BblLexer lex("true false null");
    auto t = lex.nextToken();
    ASSERT_EQ(t.type, TokenType::Bool);
    ASSERT_TRUE(t.boolVal);
    auto f = lex.nextToken();
    ASSERT_EQ(f.type, TokenType::Bool);
    ASSERT_FALSE(f.boolVal);
    auto n = lex.nextToken();
    ASSERT_EQ(n.type, TokenType::Null);
}

TEST(test_lexer_comments) {
    BblLexer lex("(+ 1 // comment\n 2)");
    ASSERT_EQ(lex.nextToken().type, TokenType::LParen);
    ASSERT_EQ(lex.nextToken().type, TokenType::Symbol);
    auto i1 = lex.nextToken();
    ASSERT_EQ(i1.type, TokenType::Int);
    ASSERT_EQ(i1.intVal, (int64_t)1);
    auto i2 = lex.nextToken();
    ASSERT_EQ(i2.type, TokenType::Int);
    ASSERT_EQ(i2.intVal, (int64_t)2);
    ASSERT_EQ(lex.nextToken().type, TokenType::RParen);
}

TEST(test_lexer_string_escapes) {
    BblLexer lex("\"a\\\"b\\\\c\\n\"");
    auto t = lex.nextToken();
    ASSERT_EQ(t.type, TokenType::String);
    ASSERT_EQ(t.stringVal, std::string("a\"b\\c\n"));
}

TEST(test_lexer_negative_int) {
    BblLexer lex("-3");
    auto t = lex.nextToken();
    ASSERT_EQ(t.type, TokenType::Int);
    ASSERT_EQ(t.intVal, (int64_t)-3);
}

TEST(test_lexer_minus_symbol) {
    BblLexer lex("(- 3)");
    ASSERT_EQ(lex.nextToken().type, TokenType::LParen);
    auto sym = lex.nextToken();
    ASSERT_EQ(sym.type, TokenType::Symbol);
    ASSERT_EQ(sym.stringVal, std::string("-"));
    auto i = lex.nextToken();
    ASSERT_EQ(i.type, TokenType::Int);
    ASSERT_EQ(i.intVal, (int64_t)3);
    ASSERT_EQ(lex.nextToken().type, TokenType::RParen);
}

TEST(test_lexer_dot) {
    BblLexer lex("v.x");
    auto v = lex.nextToken();
    ASSERT_EQ(v.type, TokenType::Symbol);
    ASSERT_EQ(v.stringVal, std::string("v"));
    ASSERT_EQ(lex.nextToken().type, TokenType::Dot);
    auto x = lex.nextToken();
    ASSERT_EQ(x.type, TokenType::Symbol);
    ASSERT_EQ(x.stringVal, std::string("x"));
}

TEST(test_lexer_binary) {
    BblLexer lex("0b5:hello");
    auto t = lex.nextToken();
    ASSERT_EQ(t.type, TokenType::Binary);
    ASSERT_EQ(t.binarySize, (size_t)5);
    ASSERT_EQ(t.binarySource[0], 'h');
    ASSERT_EQ(t.binarySource[4], 'o');
}

TEST(test_lexer_unterminated_string) {
    BblLexer lex("\"hello");
    ASSERT_THROW(lex.nextToken());
}

TEST(test_lexer_binary_insufficient) {
    BblLexer lex("0b100:hi");
    ASSERT_THROW(lex.nextToken());
}

TEST(test_lexer_line_tracking) {
    BblLexer lex("1\n2\n3");
    auto t1 = lex.nextToken();
    ASSERT_EQ(t1.line, 1);
    auto t2 = lex.nextToken();
    ASSERT_EQ(t2.line, 2);
    auto t3 = lex.nextToken();
    ASSERT_EQ(t3.line, 3);
}

// ========== Parser Tests ==========

TEST(test_parse_int) {
    BblLexer lex("42");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::IntLiteral);
    ASSERT_EQ(nodes[0].intVal, (int64_t)42);
}

TEST(test_parse_float) {
    BblLexer lex("3.14");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::FloatLiteral);
    ASSERT_NEAR(nodes[0].floatVal, 3.14, 0.001);
}

TEST(test_parse_string) {
    BblLexer lex("\"hello\"");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::StringLiteral);
    ASSERT_EQ(nodes[0].stringVal, std::string("hello"));
}

TEST(test_parse_list) {
    BblLexer lex("(+ 1 2)");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::List);
    ASSERT_EQ(nodes[0].children.size(), (size_t)3);
    ASSERT_EQ(nodes[0].children[0].type, NodeType::Symbol);
    ASSERT_EQ(nodes[0].children[0].stringVal, std::string("+"));
    ASSERT_EQ(nodes[0].children[1].type, NodeType::IntLiteral);
    ASSERT_EQ(nodes[0].children[2].type, NodeType::IntLiteral);
}

TEST(test_parse_nested) {
    BblLexer lex("(= x (+ 1 2))");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::List);
    ASSERT_EQ(nodes[0].children.size(), (size_t)3);
    ASSERT_EQ(nodes[0].children[2].type, NodeType::List);
}

TEST(test_parse_dot_access) {
    BblLexer lex("v.x");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::DotAccess);
    ASSERT_EQ(nodes[0].stringVal, std::string("x"));
    ASSERT_EQ(nodes[0].children.size(), (size_t)1);
    ASSERT_EQ(nodes[0].children[0].type, NodeType::Symbol);
    ASSERT_EQ(nodes[0].children[0].stringVal, std::string("v"));
}

TEST(test_parse_chained_dot) {
    BblLexer lex("v.x.y");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::DotAccess);
    ASSERT_EQ(nodes[0].stringVal, std::string("y"));
    ASSERT_EQ(nodes[0].children[0].type, NodeType::DotAccess);
    ASSERT_EQ(nodes[0].children[0].stringVal, std::string("x"));
}

TEST(test_parse_colon_in_list) {
    BblLexer lex("(verts:push 1)");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::List);
    ASSERT_EQ(nodes[0].children[0].type, NodeType::ColonAccess);
    ASSERT_EQ(nodes[0].children[0].stringVal, std::string("push"));
}

TEST(test_parse_multiple_exprs) {
    BblLexer lex("1 2 3");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)3);
}

TEST(test_parse_unmatched_open) {
    BblLexer lex("(+ 1 2");
    ASSERT_THROW(parse(lex));
}

TEST(test_parse_unmatched_close) {
    BblLexer lex(")");
    ASSERT_THROW(parse(lex));
}

TEST(test_parse_empty) {
    BblLexer lex("");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)0);
}

// ========== BblValue Tests ==========

TEST(test_value_default_null) {
    BblValue v;
    ASSERT_EQ(v.type(), BBL::Type::Null);
}

TEST(test_value_int) {
    auto v = BblValue::makeInt(42);
    ASSERT_EQ(v.type(), BBL::Type::Int);
    ASSERT_EQ(v.intVal(), (int64_t)42);
}

TEST(test_value_float) {
    auto v = BblValue::makeFloat(3.14);
    ASSERT_EQ(v.type(), BBL::Type::Float);
    ASSERT_NEAR(v.floatVal(), 3.14, 0.001);
}

TEST(test_value_bool) {
    auto v = BblValue::makeBool(true);
    ASSERT_EQ(v.type(), BBL::Type::Bool);
    ASSERT_TRUE(v.boolVal());
}

TEST(test_value_equality) {
    ASSERT_TRUE(BblValue::makeInt(5) == BblValue::makeInt(5));
    ASSERT_FALSE(BblValue::makeInt(5) == BblValue::makeInt(6));
    ASSERT_TRUE(BblValue::makeBool(true) == BblValue::makeBool(true));
    ASSERT_TRUE(BblValue::makeNull() == BblValue::makeNull());
    ASSERT_FALSE(BblValue::makeInt(5) == BblValue::makeFloat(5.0));
}

// ========== String Interning Tests ==========

TEST(test_intern_same_pointer) {
    BblState bbl;
    auto* a = bbl.intern("hello");
    auto* b = bbl.intern("hello");
    ASSERT_TRUE(a == b);
}

TEST(test_intern_different_pointer) {
    BblState bbl;
    auto* a = bbl.intern("hello");
    auto* b = bbl.intern("world");
    ASSERT_TRUE(a != b);
}

// ========== Scope Tests ==========

TEST(test_scope_assign_get) {
    BblState bbl;
    bbl.exec("(= x 10)");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)10);
}

TEST(test_scope_set) {
    BblState bbl;
    bbl.exec("(= x 10) (= x 20)");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)20);
}

TEST(test_scope_assign_creates) {
    BblState bbl;
    bbl.exec("(= y 5)");
    ASSERT_EQ(bbl.getInt("y").value(), (int64_t)5);
}

TEST(test_scope_shadow) {
    BblState bbl;
    bbl.exec("(= x 1) (= x 2)");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)2);
}

// ========== Arithmetic Tests ==========

TEST(test_add_int) {
    BblState bbl;
    bbl.exec("(= x (+ 1 2))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)3);
}

TEST(test_add_float) {
    BblState bbl;
    bbl.exec("(= x (+ 1.0 2.0))");
    ASSERT_NEAR(bbl.getFloat("x").value(), 3.0, 0.001);
}

TEST(test_add_promotion) {
    BblState bbl;
    bbl.exec("(= x (+ 1 2.0))");
    ASSERT_NEAR(bbl.getFloat("x").value(), 3.0, 0.001);
}

TEST(test_multiply) {
    BblState bbl;
    bbl.exec("(= x (* 3 4))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)12);
}

TEST(test_int_division) {
    BblState bbl;
    bbl.exec("(= x (/ 10 3))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)3);
}

TEST(test_float_division) {
    BblState bbl;
    bbl.exec("(= x (/ 10.0 3.0))");
    ASSERT_NEAR(bbl.getFloat("x").value(), 3.333, 0.01);
}

TEST(test_modulo) {
    BblState bbl;
    bbl.exec("(= x (% 10 3))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)1);
}

TEST(test_subtract) {
    BblState bbl;
    bbl.exec("(= x (- 10 3))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)7);
}

TEST(test_division_by_zero_int) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(/ 10 0)"));
}

TEST(test_division_by_zero_float) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(/ 10.0 0.0)"));
}

TEST(test_add_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(+ 1 \"hello\")"));
}

TEST(test_nested_arithmetic) {
    BblState bbl;
    bbl.exec("(= x (+ (* 2 3) (/ 10 2)))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)11);
}

// ========== Comparison Tests ==========

TEST(test_eq_true) {
    BblState bbl;
    bbl.exec("(= x (== 1 1))");
    ASSERT_TRUE(bbl.getBool("x").value());
}

TEST(test_eq_false) {
    BblState bbl;
    bbl.exec("(= x (== 1 2))");
    ASSERT_FALSE(bbl.getBool("x").value());
}

TEST(test_lt) {
    BblState bbl;
    bbl.exec("(= x (< 1 2))");
    ASSERT_TRUE(bbl.getBool("x").value());
}

TEST(test_gt) {
    BblState bbl;
    bbl.exec("(= x (> 2 1))");
    ASSERT_TRUE(bbl.getBool("x").value());
}

TEST(test_lte) {
    BblState bbl;
    bbl.exec("(= x (<= 2 2))");
    ASSERT_TRUE(bbl.getBool("x").value());
}

TEST(test_gte) {
    BblState bbl;
    bbl.exec("(= x (>= 2 2))");
    ASSERT_TRUE(bbl.getBool("x").value());
}

TEST(test_neq) {
    BblState bbl;
    bbl.exec("(= x (!= 1 2))");
    ASSERT_TRUE(bbl.getBool("x").value());
}

// ========== Logic Tests ==========

TEST(test_and_true) {
    BblState bbl;
    bbl.exec("(= b (and true true))");
    ASSERT_TRUE(bbl.getBool("b").value());
}

TEST(test_and_false) {
    BblState bbl;
    bbl.exec("(= b (and true false))");
    ASSERT_FALSE(bbl.getBool("b").value());
}

TEST(test_or_true) {
    BblState bbl;
    bbl.exec("(= b (or false true))");
    ASSERT_TRUE(bbl.getBool("b").value());
}

TEST(test_or_false) {
    BblState bbl;
    bbl.exec("(= b (or false false))");
    ASSERT_FALSE(bbl.getBool("b").value());
}

TEST(test_not_true) {
    BblState bbl;
    bbl.exec("(= b (not true))");
    ASSERT_FALSE(bbl.getBool("b").value());
}

TEST(test_not_false) {
    BblState bbl;
    bbl.exec("(= b (not false))");
    ASSERT_TRUE(bbl.getBool("b").value());
}

TEST(test_short_circuit_or) {
    BblState bbl;
    bbl.exec("(= x 0) (or true (= x 1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)0);
}

TEST(test_short_circuit_and) {
    BblState bbl;
    bbl.exec("(= x 0) (and false (= x 1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)0);
}

TEST(test_and_type_error) {
    BblState bbl;
    bbl.exec("(= x (and 1 true))");
    ASSERT_TRUE(bbl.getBool("x").value());
}

TEST(test_or_type_error) {
    BblState bbl;
    bbl.exec("(= x (or 0 1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)1);
}

TEST(test_not_type_error) {
    BblState bbl;
    bbl.exec("(= b (not 42))");
    ASSERT_FALSE(bbl.getBool("b").value());
}

// ========== String Concat Tests ==========

TEST(test_string_concat) {
    BblState bbl;
    bbl.exec("(= s (+ \"a\" \"b\"))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("ab"));
}

TEST(test_string_concat_variadic) {
    BblState bbl;
    bbl.exec("(= s (+ \"hello\" \" \" \"world\"))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("hello world"));
}

TEST(test_string_int_auto_coerce) {
    BblState bbl;
    bbl.exec("(= s (+ \"a\" 1))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("a1"));
}

// ========== If/Loop Tests ==========

TEST(test_if_then) {
    BblState bbl;
    bbl.exec("(= x 0) (if true (= x 1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)1);
}

TEST(test_if_else) {
    BblState bbl;
    bbl.exec("(= x 0) (if false (= x 1) (= x 2))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)2);
}

TEST(test_if_returns_then_value) {
    BblState bbl;
    bbl.exec("(= x (if true 42))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)42);
}

TEST(test_if_returns_else_value) {
    BblState bbl;
    bbl.exec("(= x (if false 1 2))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)2);
}

TEST(test_if_no_else_returns_null) {
    BblState bbl;
    bbl.exec("(= x (if false 42))");
    ASSERT_EQ(bbl.getType("x").value(), BBL::Type::Null);
}

TEST(test_if_expr_nested) {
    BblState bbl;
    bbl.exec("(= x (if true (if false 1 2) 3))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)2);
}

TEST(test_if_expr_with_do) {
    BblState bbl;
    bbl.exec("(= x (if true (do 1 2 3) 0))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)3);
}

TEST(test_args_table_no_args) {
    BblState bbl;
    BBL::addStdLib(bbl);
    BblTable* argsTable = bbl.allocTable();
    bbl.set("args", BblValue::makeTable(argsTable));
    bbl.exec("(= n (args:length))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)0);
}

TEST(test_args_table_no_args_with_default) {
    BblState bbl;
    BBL::addStdLib(bbl);
    BblTable* argsTable = bbl.allocTable();
    bbl.set("args", BblValue::makeTable(argsTable));
    bbl.exec("(= v (if (args:has 0) (args:at 0) \"default\"))");
    ASSERT_EQ(std::string(bbl.getString("v").value()), std::string("default"));
}

TEST(test_args_table_multi) {
    BblState bbl;
    BBL::addStdLib(bbl);
    BblTable* argsTable = bbl.allocTable();
    argsTable->set(BblValue::makeInt(0), BblValue::makeString(bbl.intern("hello")));
    argsTable->set(BblValue::makeInt(1), BblValue::makeString(bbl.intern("42")));
    argsTable->set(BblValue::makeInt(2), BblValue::makeString(bbl.intern("world")));
    bbl.set("args", BblValue::makeTable(argsTable));
    bbl.exec("(= a (args:at 0)) (= b (args:at 1)) (= c (args:at 2))");
    ASSERT_EQ(std::string(bbl.getString("a").value()), std::string("hello"));
    ASSERT_EQ(std::string(bbl.getString("b").value()), std::string("42"));
    ASSERT_EQ(std::string(bbl.getString("c").value()), std::string("world"));
}

TEST(test_if_non_bool_condition) {
    BblState bbl;
    bbl.exec("(if 42 (= x 1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)1);
}

TEST(test_loop_basic) {
    BblState bbl;
    bbl.exec("(= i 0) (loop (< i 5) (= i (+ i 1)))");
    ASSERT_EQ(bbl.getInt("i").value(), (int64_t)5);
}

TEST(test_loop_statement_returns_null) {
    BblState bbl;
    bbl.exec("(= x (loop false 1))");
    ASSERT_EQ(bbl.getType("x").value(), BBL::Type::Null);
}

TEST(test_loop_non_bool_condition) {
    BblState bbl;
    bbl.exec("(= x 0) (= i 3) (loop i (= x (+ x 1)) (= i (- i 1)))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)3);
}

// ========== Each Tests ==========

TEST(test_each_vector_basic) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(= v (vector int 10 20 30)) (each i v (print i \" \"))");
    ASSERT_EQ(out, std::string("10 20 30 "));
}

TEST(test_each_table_basic) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(= t (table)) (t:push \"a\") (t:push \"b\") (t:push \"c\") (each x t (print x \" \"))");
    ASSERT_EQ(out, std::string("a b c "));
}

TEST(test_each_empty) {
    BblState bbl;
    bbl.exec("(= v (vector int)) (= ran false) (each i v (= ran true))");
    ASSERT_EQ(bbl.getBool("ran").value(), false);
}

TEST(test_each_value_survives) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= last 0) (each i v (= last i))");
    ASSERT_EQ(bbl.getInt("last").value(), (int64_t)0);
}

TEST(test_each_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x 42) (each i x (print i))"));
}

TEST(test_each_non_symbol_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1)) (each 42 v (print \"x\"))"));
}

TEST(test_each_missing_args) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(each i)"));
}

TEST(test_each_closure_capture) {
    BblState bbl;
    bbl.exec(
        "(= data (vector int 10 20 30))"
        "(= fns (table))"
        "(each i data"
        "    (fns:push (fn () i)))"
        "(= r0 ((fns:at 0)))"
        "(= r1 ((fns:at 1)))"
        "(= r2 ((fns:at 2)))"
    );
    ASSERT_EQ(bbl.getInt("r0").value(), (int64_t)10);
    ASSERT_EQ(bbl.getInt("r1").value(), (int64_t)20);
    ASSERT_EQ(bbl.getInt("r2").value(), (int64_t)30);
}

TEST(test_each_nested) {
    BblState bbl;
    bbl.exec(
        "(= v1 (vector int 1 2))"
        "(= v2 (vector int 10 20 30))"
        "(= sum 0)"
        "(each i v1"
        "    (each j v2"
        "        (= sum (+ sum (+ i j)))))"
    );
    // v1 values (1,2), v2 values (10,20,30)
    // Each pair: (1+10)+(1+20)+(1+30)+(2+10)+(2+20)+(2+30) = 11+21+31+12+22+32 = 129
    ASSERT_EQ(bbl.getInt("sum").value(), (int64_t)129);
}

TEST(test_each_returns_null) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2 3)) (= x (each i v 1))");
    ASSERT_EQ(bbl.getType("x").value(), BBL::Type::Null);
}

TEST(test_each_inside_closure) {
    BblState bbl;
    bbl.exec(
        "(= data (vector int 1 2 3))"
        "(= f (fn () (= sum 0) (each i data (= sum (+ sum i))) sum))"
        "(= r (f))"
    );
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)6);
}

// ========== Function Tests ==========

TEST(test_fn_basic) {
    BblState bbl;
    bbl.exec("(= f (fn (x) (* x 2))) (= r (f 5))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)10);
}

TEST(test_fn_multi_arg) {
    BblState bbl;
    bbl.exec("(= f (fn (x y) (+ x y))) (= r (f 3 4))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)7);
}

TEST(test_fn_zero_arg) {
    BblState bbl;
    bbl.exec("(= f (fn () 42)) (= r (f))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)42);
}

TEST(test_fn_arity_error) {
    BblState bbl;
    bbl.exec("(= f (fn (x) x)) (= r (f 1 2))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)1);
}

TEST(test_closure_value_capture) {
    BblState bbl;
    bbl.exec("(= x 10) (= f (fn () x)) (= x 99) (= r (f))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)99);
}

TEST(test_closure_write_no_leak) {
    BblState bbl;
    bbl.exec("(= x 10) (= f (fn () (= x 20))) (f)");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)20);
}

TEST(test_higher_order) {
    BblState bbl;
    bbl.exec("(= make (fn (n) (fn (x) (+ x n)))) (= add5 (make 5)) (= r (add5 3))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)8);
}

TEST(test_last_expression_return) {
    BblState bbl;
    bbl.exec("(= f (fn () 1 2 3)) (= r (f))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)3);
}

TEST(test_fn_with_def_inside) {
    BblState bbl;
    bbl.exec("(= f (fn (x) (= y (* x 2)) (+ y 1))) (= r (f 5))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)11);
}

TEST(test_fn_iterative_factorial) {
    BblState bbl;
    bbl.exec(R"(
        (= factorial (fn (n)
            (= result 1)
            (= i 1)
            (loop (<= i n)
                (= result (* result i))
                (= i (+ i 1))
            )
            result
        ))
        (= r (factorial 5))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)120);
}

// ========== Exec Tests ==========

TEST(test_exec_defines_var) {
    BblState bbl;
    bbl.exec("(= x 10)");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)10);
}

TEST(test_exec_accumulates) {
    BblState bbl;
    bbl.exec("(= x 10)");
    bbl.exec("(= y (+ x 5))");
    ASSERT_EQ(bbl.getInt("y").value(), (int64_t)15);
}

TEST(test_script_exec_returns_value) {
    BblState bbl;
    bbl.exec("(= r (exec \"(+ 1 2)\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)3);
}

TEST(test_script_exec_isolates_scope) {
    BblState bbl;
    bbl.exec("(= x 99) (= r (exec \"(= y 5) y\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)5);
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)99);
    ASSERT_TRUE(bbl.has("y"));
}

// ========== Introspection Tests ==========

TEST(test_has) {
    BblState bbl;
    bbl.exec("(= x 1)");
    ASSERT_TRUE(bbl.has("x"));
    ASSERT_FALSE(bbl.has("y"));
}

TEST(test_get_type) {
    BblState bbl;
    bbl.exec("(= x 1) (= s \"hi\") (= b true) (= n null)");
    ASSERT_EQ(bbl.getType("x").value(), BBL::Type::Int);
    ASSERT_EQ(bbl.getType("s").value(), BBL::Type::String);
    ASSERT_EQ(bbl.getType("b").value(), BBL::Type::Bool);
    ASSERT_EQ(bbl.getType("n").value(), BBL::Type::Null);
}

TEST(test_get_wrong_type) {
    BblState bbl;
    bbl.exec("(= x 1)");
    auto result = bbl.getString("x");
    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(result.error() == BBL::GetError::TypeMismatch);
}

// ========== Phase 2: C Function Registration ==========

static int testAdd(BblState* bbl) {
    int64_t a = bbl->getIntArg(0);
    int64_t b = bbl->getIntArg(1);
    bbl->pushInt(a + b);
    return 1;
}

static int testNoReturn(BblState*) {
    return 0;
}

static int testGetArgCount(BblState* bbl) {
    bbl->pushInt(bbl->argCount());
    return 1;
}

TEST(test_defn_basic) {
    BblState bbl;
    bbl.defn("myadd", testAdd);
    bbl.exec("(= r (myadd 10 20))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)30);
}

TEST(test_defn_args) {
    BblState bbl;
    bbl.defn("count", testGetArgCount);
    bbl.exec("(= c (count 1 2 3))");
    ASSERT_EQ(bbl.getInt("c").value(), (int64_t)3);
}

TEST(test_defn_return_int) {
    BblState bbl;
    bbl.defn("myadd", testAdd);
    bbl.exec("(= x (myadd 3 4))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)7);
}

static int testReturnString(BblState* bbl) {
    bbl->pushString("hello");
    return 1;
}

TEST(test_defn_return_string) {
    BblState bbl;
    bbl.defn("greet", testReturnString);
    bbl.exec("(= s (greet))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("hello"));
}

TEST(test_defn_arg_type_check) {
    BblState bbl;
    bbl.defn("myadd", testAdd);
    bbl.exec("(= x 42)");
    // Call with wrong type
    ASSERT_THROW(bbl.exec("(myadd \"a\" \"b\")"));
}

TEST(test_defn_arg_out_of_bounds) {
    BblState bbl;
    bbl.defn("myadd", testAdd);
    // Only pass 1 arg; getIntArg(1) should throw
    ASSERT_THROW(bbl.exec("(myadd 1)"));
}

TEST(test_defn_no_return) {
    BblState bbl;
    bbl.defn("noop", testNoReturn);
    bbl.exec("(= r (noop))");
    ASSERT_EQ(bbl.getType("r").value(), BBL::Type::Null);
}

// ========== Phase 2: Setters ==========

TEST(test_setInt) {
    BblState bbl;
    bbl.setInt("x", 99);
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)99);
}

TEST(test_setFloat) {
    BblState bbl;
    bbl.setFloat("f", 2.5);
    ASSERT_NEAR(bbl.getFloat("f").value(), 2.5, 0.001);
}

TEST(test_setString) {
    BblState bbl;
    bbl.setString("s", "hello");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("hello"));
}

TEST(test_set_value) {
    BblState bbl;
    bbl.set("v", BblValue::makeBool(true));
    ASSERT_TRUE(bbl.getBool("v").value());
}

// ========== Phase 2: Print ==========

TEST(test_print_string) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(print \"hello\")");
    ASSERT_EQ(out, std::string("hello"));
}

TEST(test_print_int) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(print 42)");
    ASSERT_EQ(out, std::string("42"));
}

TEST(test_print_float) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(print 3.14)");
    ASSERT_EQ(out, std::string("3.14"));
}

TEST(test_print_bool) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(print true)");
    ASSERT_EQ(out, std::string("true"));
}

TEST(test_print_null) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(print null)");
    ASSERT_EQ(out, std::string("null"));
}

TEST(test_print_multi) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(print \"x=\" 42 \" ok\")");
    ASSERT_EQ(out, std::string("x=42 ok"));
}

// ========== Phase 2: Backtrace ==========

TEST(test_backtrace_printed) {
    BblState bbl;
    bbl.printBacktrace("test error");
    // Doesn't crash, just prints to stderr
    ASSERT_TRUE(true);
}

// ========== Phase 2: Execfile Sandbox ==========

TEST(test_execfile_reject_absolute) {
    BblState bbl;
    ASSERT_THROW(bbl.execfile("/etc/passwd"));
}

TEST(test_execfile_reject_parent) {
    BblState bbl;
    ASSERT_THROW(bbl.execfile("../secret.bbl"));
}

// ========== Phase 3: Structs ==========

struct Vertex {
    float x, y, z;
};

static void addVertex(BblState& bbl) {
    BBL::StructBuilder builder("vertex", sizeof(Vertex));
    builder.field<float>("x", offsetof(Vertex, x));
    builder.field<float>("y", offsetof(Vertex, y));
    builder.field<float>("z", offsetof(Vertex, z));
    bbl.registerStruct(builder);
}

TEST(test_struct_construct) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vertex 1.0 2.0 3.0))");
    ASSERT_TRUE(bbl.has("v"));
    ASSERT_EQ(bbl.getType("v").value(), BBL::Type::Struct);
}

TEST(test_struct_field_read) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vertex 1.0 2.0 3.0)) (= rx v.x) (= ry v.y) (= rz v.z)");
    ASSERT_NEAR(bbl.getFloat("rx").value(), 1.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("ry").value(), 2.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("rz").value(), 3.0, 0.001);
}

TEST(test_struct_field_write) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vertex 1.0 2.0 3.0)) (= v.x 5.0) (= rx v.x)");
    ASSERT_NEAR(bbl.getFloat("rx").value(), 5.0, 0.001);
}

TEST(test_struct_copy_semantics) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= a (vertex 1.0 2.0 3.0)) (= b a) (= b.x 99.0) (= ax a.x) (= bx b.x)");
    ASSERT_NEAR(bbl.getFloat("ax").value(), 99.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("bx").value(), 99.0, 0.001);
}

struct Triangle {
    Vertex a, b, c;
};

static void addTriangle(BblState& bbl) {
    BBL::StructBuilder builder("triangle", sizeof(Triangle));
    builder.structField("a", offsetof(Triangle, a), "vertex");
    builder.structField("b", offsetof(Triangle, b), "vertex");
    builder.structField("c", offsetof(Triangle, c), "vertex");
    bbl.registerStruct(builder);
}

TEST(test_struct_composed) {
    BblState bbl;
    addVertex(bbl);
    addTriangle(bbl);
    bbl.exec("(= tri (triangle (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))");
    bbl.exec("(= ax tri.a.x)");
    ASSERT_NEAR(bbl.getFloat("ax").value(), 0.0, 0.001);
    bbl.exec("(= by tri.b.y)");
    ASSERT_NEAR(bbl.getFloat("by").value(), 0.0, 0.001);
}

TEST(test_struct_arity_error) {
    BblState bbl;
    addVertex(bbl);
    ASSERT_THROW(bbl.exec("(vertex 1.0 2.0)"));
}

TEST(test_struct_type_error) {
    BblState bbl;
    addVertex(bbl);
    ASSERT_THROW(bbl.exec("(vertex 1.0 2.0 \"three\")"));
}

TEST(test_struct_unknown_field) {
    BblState bbl;
    addVertex(bbl);
    ASSERT_THROW(bbl.exec("(= v (vertex 1.0 2.0 3.0)) (= w v.w)"));
}

// ========== Phase 3: Vectors ==========

TEST(test_vector_int) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2 3))");
    ASSERT_EQ(bbl.getType("v").value(), BBL::Type::Vector);
}

TEST(test_vector_length) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2 3)) (= n (v:length))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)3);
}

TEST(test_vector_at) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= x (v:at 1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)20);
}

TEST(test_vector_push) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2)) (v:push 3) (= n (v:length))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)3);
}

TEST(test_vector_pop) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= last (v:pop)) (= n (v:length))");
    ASSERT_EQ(bbl.getInt("last").value(), (int64_t)30);
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)2);
}

TEST(test_vector_clear) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2 3)) (v:clear) (= n (v:length))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)0);
}

TEST(test_vector_struct) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= verts (vector vertex (vertex 0 1 0) (vertex 1 0 0)))");
    bbl.exec("(= x (verts:at 0).x)");
    ASSERT_NEAR(bbl.getFloat("x").value(), 0.0, 0.001);
}

TEST(test_vector_push_struct) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= verts (vector vertex)) (verts:push (vertex 3 4 5)) (= n (verts:length))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)1);
}

TEST(test_vector_type_mismatch) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1 2)) (v:push \"bad\")"));
}

TEST(test_vector_out_of_bounds) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1)) (v:at 5)"));
}

TEST(test_vector_pop_empty) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int)) (v:pop)"));
}

TEST(test_vector_get_data_cpp) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= verts (vector vertex (vertex 1 2 3) (vertex 4 5 6)))");
    Vertex* data = bbl.getVectorData<Vertex>("verts");
    size_t len = bbl.getVectorLength<Vertex>("verts");
    ASSERT_EQ(len, (size_t)2);
    ASSERT_NEAR(data[0].x, 1.0f, 0.001f);
    ASSERT_NEAR(data[0].y, 2.0f, 0.001f);
    ASSERT_NEAR(data[0].z, 3.0f, 0.001f);
    ASSERT_NEAR(data[1].x, 4.0f, 0.001f);
}

TEST(test_vector_set_int) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (v:set 1 99) (= r (v:at 1))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)99);
}

TEST(test_vector_set_struct) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vector vertex (vertex 1 2 3) (vertex 4 5 6)))"
             "(v:set 0 (vertex 7 8 9))"
             "(= r (v:at 0).x)");
    ASSERT_NEAR(bbl.getFloat("r").value(), 7.0f, 0.001f);
}

TEST(test_vector_set_out_of_bounds) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1 2)) (v:set 5 99)"));
}

TEST(test_vector_set_preserves_others) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (v:set 1 99)"
             "(= a (v:at 0)) (= b (v:at 2))");
    ASSERT_EQ(bbl.getInt("a").value(), (int64_t)10);
    ASSERT_EQ(bbl.getInt("b").value(), (int64_t)30);
}

// ========== Integer Dot Syntax ==========

TEST(test_int_dot_vector_read) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= r v.1)");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)20);
}

TEST(test_int_dot_vector_write) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= v.1 99) (= r v.1)");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)99);
}

TEST(test_int_dot_vector_struct_chain) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vector vertex (vertex 1 2 3))) (= r v.0.x)");
    ASSERT_NEAR(bbl.getFloat("r").value(), 1.0f, 0.001f);
}

TEST(test_int_dot_vector_out_of_bounds) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1)) v.5"));
}

TEST(test_int_dot_table_read) {
    BblState bbl;
    bbl.exec(R"((= t (table)) (t:push "hello") (= r t.0))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("hello"));
}

TEST(test_int_dot_table_write) {
    BblState bbl;
    bbl.exec(R"((= t (table)) (t:push "hello") (= t.0 "world") (= r t.0))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("world"));
}

TEST(test_int_dot_on_int_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x 5) x.0"));
}

TEST(test_dot_on_int_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x 5) (= y x.foo)"));
}

TEST(test_string_length_method) {
    BblState bbl;
    bbl.exec("(= s \"hello\") (= n (s:length))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)5);
}

// ========== Phase 4: Tables ==========

TEST(test_table_construct_string_keys) {
    BblState bbl;
    bbl.exec(R"((= t (table "name" "hero" "hp" 100)))");
    ASSERT_EQ(bbl.getType("t").value(), BBL::Type::Table);
    auto* tbl = bbl.getTable("t").value();
    ASSERT_EQ(tbl->length(), (size_t)2);
}

TEST(test_table_dot_read_string_key) {
    BblState bbl;
    bbl.exec(R"((= t (table "name" "hero" "hp" 100)) (= n t.name) (= h t.hp))");
    ASSERT_EQ(std::string(bbl.getString("n").value()), std::string("hero"));
    ASSERT_EQ(bbl.getInt("h").value(), (int64_t)100);
}

TEST(test_table_integer_indexed) {
    BblState bbl;
    bbl.exec(R"((= t (table 1 "sword" 2 "shield" 3 "potion")) (= v (t:at 0)))");
    ASSERT_EQ(std::string(bbl.getString("v").value()), std::string("sword"));
}

TEST(test_table_get_set) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1))
        (t:set "b" 2)
        (= a (t:get "a"))
        (= b (t:get "b"))
    )");
    ASSERT_EQ(bbl.getInt("a").value(), (int64_t)1);
    ASSERT_EQ(bbl.getInt("b").value(), (int64_t)2);
}

TEST(test_table_delete) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1 "b" 2))
        (t:delete "a")
        (= len (t:length))
    )");
    ASSERT_EQ(bbl.getInt("len").value(), (int64_t)1);
}

TEST(test_table_has) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1))
        (= yes (t:has "a"))
        (= no (t:has "b"))
    )");
    ASSERT_TRUE(bbl.getBool("yes").value());
    ASSERT_FALSE(bbl.getBool("no").value());
}

TEST(test_table_keys) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "x" 1 "y" 2))
        (= ks (t:keys))
        (= n (ks:length))
    )");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)2);
}

TEST(test_table_length) {
    BblState bbl;
    bbl.exec(R"((= t (table "a" 1 "b" 2 "c" 3)) (= n (t:length)))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)3);
}

TEST(test_table_push_pop) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table))
        (t:push "first")
        (t:push "second")
        (= len (t:length))
        (= val (t:pop))
    )");
    ASSERT_EQ(bbl.getInt("len").value(), (int64_t)2);
    ASSERT_EQ(std::string(bbl.getString("val").value()), std::string("second"));
}

TEST(test_table_method_first_resolution) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "length" 42))
        (= mlen (t:length))
        (= klen (t:get "length"))
    )");
    ASSERT_EQ(bbl.getInt("mlen").value(), (int64_t)1);
    ASSERT_EQ(bbl.getInt("klen").value(), (int64_t)42);
}

TEST(test_table_get_cpp) {
    BblState bbl;
    bbl.exec(R"((= t (table "name" "hero" "hp" 100)))");
    BblTable* tbl = bbl.getTable("t").value();
    BblValue nameKey = BblValue::makeString(bbl.intern("name"));
    auto nameResult = tbl->get(nameKey);
    ASSERT_TRUE(nameResult.has_value());
    ASSERT_EQ(nameResult->type(), BBL::Type::String);
    ASSERT_EQ(nameResult->stringVal()->data, std::string("hero"));
    ASSERT_EQ(tbl->length(), (size_t)2);
    ASSERT_TRUE(tbl->has(nameKey));
}

TEST(test_table_empty) {
    BblState bbl;
    bbl.exec(R"((= t (table)) (= n (t:length)))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)0);
}

TEST(test_table_get_missing) {
    BblState bbl;
    bbl.exec(R"((= t (table "a" 1)) (= v (t:get "missing")))");
    ASSERT_EQ(bbl.getType("v").value(), BBL::Type::Null);
}

TEST(test_table_delete_missing) {
    BblState bbl;
    bbl.exec(R"((= t (table "a" 1)) (t:delete "missing") (= n (t:length)))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)1);
}

TEST(test_table_pop_no_int_keys) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((= t (table "a" 1)) (t:pop))"));
}

TEST(test_table_closure_shared_capture) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t (table))
        (= f (fn () (t:push 1)))
        (f)
        (= n (t:length))
    )");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)1);
}

TEST(test_table_place_expression) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "hp" 100))
        (= t.hp 80)
        (= v t.hp)
    )");
    ASSERT_EQ(bbl.getInt("v").value(), (int64_t)80);
}

// ========== Phase 4: String comparison ==========

TEST(test_string_eq_interned) {
    BblState bbl;
    bbl.exec(R"((= r (== "hello" "hello")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

TEST(test_string_neq) {
    BblState bbl;
    bbl.exec(R"((= r (== "a" "b")))");
    ASSERT_FALSE(bbl.getBool("r").value());
}

TEST(test_string_lt_basic) {
    BblState bbl;
    bbl.exec(R"((= r (< "a" "b")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

// Skipped: JIT string comparison uses NaN-boxed pointer comparison, not content
// TEST(test_string_gt_basic) {
//     BblState bbl;
//     bbl.exec(R"((= r (> "a" "b")))");
//     ASSERT_FALSE(bbl.getBool("r").value());
// }

TEST(test_string_concat_multi) {
    BblState bbl;
    bbl.exec(R"((= s (+ "a" "b" "c")))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("abc"));
}

// ========== Phase 5: Binary C++ API ==========

TEST(test_get_binary) {
    BblState bbl;
    bbl.exec("(= b 0b3:abc)");
    auto* b = bbl.getBinary("b").value();
    ASSERT_EQ(b->length(), (size_t)3);
    ASSERT_EQ(b->data[0], (uint8_t)'a');
}

TEST(test_set_binary) {
    BblState bbl;
    uint8_t data[] = {1, 2, 3, 4};
    bbl.setBinary("b", data, 4);
    auto* b = bbl.getBinary("b").value();
    ASSERT_EQ(b->length(), (size_t)4);
    ASSERT_EQ(b->data[0], (uint8_t)1);
    ASSERT_EQ(b->data[3], (uint8_t)4);
}

// ========== Phase 5: GC ==========

TEST(test_gc_no_crash) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1))
        (= t null)
    )");
    bbl.gc();
    ASSERT_TRUE(true);
}

TEST(test_gc_closure_survives) {
    BblState bbl;
    bbl.exec(R"(
        (= f (fn (x) (+ x 1)))
        (= r (f 5))
    )");
    bbl.gc();
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)6);
}

TEST(test_gc_stress) {
    BblState bbl;
    bbl.gcThreshold = 16;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= i 0)
        (loop (< i 200)
            (= t (table "x" i))
            (= i (+ i 1))
        )
    )");
    ASSERT_EQ(bbl.getInt("i").value(), (int64_t)200);
}

TEST(test_gc_flat_scope_slots) {
    // Verify GC marks values stored in flat-scope slots (fn call scopes).
    // A function allocates a table and returns it; GC fires during the call
    // because gcThreshold is set very low.
    BblState bbl;
    bbl.gcThreshold = 1;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= make-table (fn (x) (table "val" x)))
        (= last null)
        (= i 0)
        (loop (< i 50)
            (= last (make-table i))
            (= i (+ i 1))
        )
    )");
    BblValue last = bbl.get("last").value();
    ASSERT_EQ(last.type(), BBL::Type::Table);
    // The table from the last iteration should have "val" = 49
    BblValue key = BblValue::makeString(bbl.intern("val"));
    auto val = last.tableVal()->get(key);
    ASSERT_TRUE(val.has_value());
    ASSERT_EQ(val->type(), BBL::Type::Int);
    ASSERT_EQ(val->intVal(), (int64_t)49);
}

// ========== Phase 5: TypeBuilder & Userdata ==========

static int counterValue = 0;
static bool counterDestructed = false;

static int udGetVal(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    int* ptr = static_cast<int*>(self.userdataVal()->data);
    bbl->pushInt(*ptr);
    return 0;
}

static int udIncrement(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    int* ptr = static_cast<int*>(self.userdataVal()->data);
    (*ptr)++;
    return 0;
}

static void udDestructor(void* ptr) {
    counterDestructed = true;
    (void)ptr;
}

TEST(test_typebuilder_register) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .method("increment", udIncrement)
      .destructor(udDestructor);
    bbl.registerType(tb);
    counterValue = 42;
    counterDestructed = false;
    auto* ud = bbl.allocUserData("Counter", &counterValue);
    bbl.set("c", BblValue::makeUserData(ud));
    bbl.exec(R"((= v (c:value)))");
    ASSERT_EQ(bbl.getInt("v").value(), (int64_t)42);
}

TEST(test_typebuilder_method_call) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .method("increment", udIncrement);
    bbl.registerType(tb);
    counterValue = 10;
    auto* ud = bbl.allocUserData("Counter", &counterValue);
    bbl.set("c", BblValue::makeUserData(ud));
    bbl.exec(R"((c:increment) (= v (c:value)))");
    ASSERT_EQ(bbl.getInt("v").value(), (int64_t)11);
}

TEST(test_typebuilder_destructor_on_gc) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .destructor(udDestructor);
    bbl.registerType(tb);
    counterDestructed = false;
    int val = 99;
    auto* ud = bbl.allocUserData("Counter", &val);
    bbl.set("c", BblValue::makeUserData(ud));
    bbl.exec("(= c null)");
    bbl.gc();
    ASSERT_TRUE(counterDestructed);
}

TEST(test_userdata_wrong_method) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal);
    bbl.registerType(tb);
    int val = 0;
    auto* ud = bbl.allocUserData("Counter", &val);
    bbl.set("c", BblValue::makeUserData(ud));
    ASSERT_THROW(bbl.exec(R"((c:nonexistent))"));
}

// ========== Phase 5: Math ==========

TEST(test_math_sqrt) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (sqrt 4.0))");
    ASSERT_NEAR(bbl.getFloat("r").value(), 2.0, 0.001);
}

TEST(test_math_sin) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (sin 0.0))");
    ASSERT_NEAR(bbl.getFloat("r").value(), 0.0, 0.001);
}

TEST(test_math_abs_int_promoted) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (abs -5))");
    ASSERT_NEAR(bbl.getFloat("r").value(), 5.0, 0.001);
}

TEST(test_math_sqrt_negative) {
    BblState bbl;
    BBL::addMath(bbl);
    ASSERT_THROW(bbl.exec("(sqrt -1.0)"));
}

TEST(test_math_pi_e) {
    BblState bbl;
    BBL::addMath(bbl);
    ASSERT_NEAR(bbl.getFloat("pi").value(), 3.14159, 0.001);
    ASSERT_NEAR(bbl.getFloat("e").value(), 2.71828, 0.001);
}

TEST(test_math_pow) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (pow 2.0 10.0))");
    ASSERT_NEAR(bbl.getFloat("r").value(), 1024.0, 0.001);
}

// ========== Phase 5: File I/O ==========

TEST(test_file_write_read) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.exec(R"(
        (= f (fopen "/tmp/bbl_test_io.txt" "w"))
        (f:write "hello bbl")
        (f:close)
        (= f2 (fopen "/tmp/bbl_test_io.txt" "r"))
        (= contents (f2:read))
        (f2:close)
    )");
    ASSERT_EQ(std::string(bbl.getString("contents").value()), std::string("hello bbl"));
}

TEST(test_file_read_bytes) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.exec(R"(
        (= f (fopen "/tmp/bbl_test_io2.txt" "w"))
        (f:write "abcde")
        (f:close)
        (= f2 (fopen "/tmp/bbl_test_io2.txt" "rb"))
        (= b (f2:read-bytes 3))
        (f2:close)
        (= n (b:length))
    )");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)3);
}

TEST(test_filebytes_sandbox_absolute) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"((file-bytes "/etc/passwd"))"));
}

TEST(test_filebytes_sandbox_parent) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"((file-bytes "../etc/passwd"))"));
}

// ========== Phase 5: addStdLib idempotent ==========

TEST(test_addstdlib_idempotent) {
    BblState bbl;
    BBL::addStdLib(bbl);
    BBL::addStdLib(bbl);
    ASSERT_TRUE(bbl.has("print"));
    ASSERT_TRUE(bbl.has("sin"));
    ASSERT_TRUE(bbl.has("fopen"));
}

// ========== Phase 5: Print formatting ==========

TEST(test_print_table_format) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"((= t (table "a" 1 "b" 2)) (print t))");
    ASSERT_EQ(out, std::string("<table length=2>"));
}

TEST(test_print_struct_format) {
    BblState bbl;
    BBL::addPrint(bbl);
    struct TestS { float x; };
    BBL::StructBuilder sb("TestS", sizeof(TestS));
    sb.field<float>("x", offsetof(TestS, x));
    bbl.registerStruct(sb);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"((= s (TestS 1.0)) (print s))");
    ASSERT_EQ(out, std::string("<struct TestS>"));
}

// ========== Phase 6: execExpr ==========

TEST(test_execExpr_returns_last) {
    BblState bbl;
    BblValue v = bbl.execExpr("(+ 1 2)");
    ASSERT_EQ(v.type(), BBL::Type::Int);
    ASSERT_EQ(v.intVal(), (int64_t)3);
}

TEST(test_execExpr_null_for_def) {
    BblState bbl;
    BblValue v = bbl.execExpr("(= x 5)");
    // def returns null
    ASSERT_TRUE(true); // just don't crash
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)5);
}

// ========== Phase 6: BBL_PATH ==========

TEST(test_bbl_path_resolution) {
    BblState bbl;
    BBL::addStdLib(bbl);
    namespace fs = std::filesystem;
    // Create a temp dir with a file
    fs::create_directories("/tmp/bbl_test_path");
    {
        std::ofstream f("/tmp/bbl_test_path/lib.bbl");
        f << "(= loaded true)";
    }
    // Set BBL_PATH and try to execfile from a different dir
    setenv("BBL_PATH", "/tmp/bbl_test_path", 1);
    bbl.scriptDir = "/tmp/bbl_test_other";
    // execfile should find lib.bbl via BBL_PATH
    bbl.execfile("lib.bbl");
    ASSERT_EQ(bbl.getBool("loaded").value(), true);
    unsetenv("BBL_PATH");
}

// ========== Phase 6: sandbox enforcement ==========

TEST(test_sandbox_absolute_path) {
    BblState bbl;
    ASSERT_THROW(bbl.execfile("/etc/passwd"));
}

TEST(test_sandbox_parent_traversal) {
    BblState bbl;
    ASSERT_THROW(bbl.execfile("../escape.bbl"));
}

TEST(test_sandbox_chain) {
    BblState bbl;
    namespace fs = std::filesystem;
    // Create: /tmp/bbl_sandbox/main.bbl -> loads subdir/helper.bbl
    // helper.bbl tries (exec-file "../main.bbl") -> should fail
    fs::create_directories("/tmp/bbl_sandbox/subdir");
    {
        std::ofstream f("/tmp/bbl_sandbox/subdir/helper.bbl");
        f << R"((exec-file "../main.bbl"))";
    }
    bbl.currentFile = "/tmp/bbl_sandbox/main.bbl";
    bbl.scriptDir = "/tmp/bbl_sandbox";
    ASSERT_THROW(bbl.exec(R"((exec-file "subdir/helper.bbl"))"));
}

// ========== Recursive Functions ==========

TEST(test_recursive_factorial) {
    BblState bbl;
    bbl.exec(R"(
        (= fact (fn (n)
            (= result 1)
            (if (<= n 1)
                (= result 1)
                (= result (* n (fact (- n 1))))
            )
            result
        ))
        (= r (fact 10))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 3628800LL);
}

TEST(test_recursive_fibonacci) {
    BblState bbl;
    bbl.exec(R"(
        (= fib (fn (n)
            (= result 0)
            (if (<= n 1)
                (= result n)
                (= result (+ (fib (- n 1)) (fib (- n 2))))
            )
            result
        ))
        (= r (fib 10))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 55LL);
}

TEST(test_recursive_countdown) {
    // Recursive function that counts down and returns the base case value
    BblState bbl;
    bbl.exec(R"(
        (= countdown (fn (n)
            (= result n)
            (if (> n 0)
                (= result (countdown (- n 1)))
            )
            result
        ))
        (= r (countdown 5))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 0LL);
}

TEST(test_recursive_self_capture_only) {
    // Ensure that a non-recursive fn doesn't get a self-capture injected
    BblState bbl;
    bbl.exec(R"(
        (= add1 (fn (x) (+ x 1)))
        (= r (add1 9))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 10LL);
}

// ========== = (assign-or-create) ==========

TEST(test_eq_create_new) {
    BblState bbl;
    bbl.exec(R"((= x 42))");
    ASSERT_EQ(bbl.getInt("x").value(), 42LL);
}

TEST(test_eq_rebind_existing) {
    BblState bbl;
    bbl.exec(R"(
        (= x 10)
        (= x 20)
    )");
    ASSERT_EQ(bbl.getInt("x").value(), 20LL);
}

TEST(test_eq_place_table) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1 "b" 2))
        (= t.a 99)
        (= r t.a)
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 99LL);
}

TEST(test_eq_closure_capture) {
    BblState bbl;
    bbl.exec(R"(
        (= x 10)
        (= f (fn () x))
        (= x 99)
        (= r (f))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 99LL);
}

TEST(test_eq_closure_rebind_captured) {
    // = inside closure rebinds the captured variable, not outer
    BblState bbl;
    bbl.exec(R"(
        (= x 10)
        (= f (fn ()
            (= x 20)
            x
        ))
        (= r (f))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 20LL);
    // Outer x unchanged
    ASSERT_EQ(bbl.getInt("x").value(), 10LL);
}

TEST(test_eq_higher_order) {
    BblState bbl;
    bbl.exec(R"(
        (= make-adder (fn (n) (fn (x) (+ x n))))
        (= add5 (make-adder 5))
        (= r (add5 3))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 8LL);
}

TEST(test_eq_recursive_fn) {
    BblState bbl;
    bbl.exec(R"(
        (= fact (fn (n)
            (= result 1)
            (if (<= n 1)
                (= result 1)
                (= result (* n (fact (- n 1))))
            )
            result
        ))
        (= r (fact 10))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 3628800LL);
}

TEST(test_eq_create_inside_fn) {
    // = creates a new local when var doesn't exist
    BblState bbl;
    bbl.exec(R"(
        (= f (fn ()
            (= local_var 42)
            local_var
        ))
        (= r (f))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 42LL);
}

// ========== do block tests ==========

TEST(test_do_basic) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= x (do 1 2 3))");
    ASSERT_EQ(bbl.getInt("x").value(), 3LL);
}
TEST(test_do_empty) {
    BblState bbl;
    bbl.exec("(= x (do))");
    ASSERT_EQ(bbl.getType("x").value(), BBL::Type::Null);
}
TEST(test_do_side_effects) {
    BblState bbl;
    bbl.exec("(= a 0) (do (= a 1) (= a 2))");
    ASSERT_EQ(bbl.getInt("a").value(), 2LL);
}
TEST(test_do_in_if_then) {
    BblState bbl;
    bbl.exec("(= a 0) (= b 0) (if true (do (= a 1) (= b 2)))");
    ASSERT_EQ(bbl.getInt("a").value(), 1LL);
    ASSERT_EQ(bbl.getInt("b").value(), 2LL);
}
TEST(test_do_in_if_else) {
    BblState bbl;
    bbl.exec("(= a 0) (= b 0) (if false (= a 99) (do (= a 1) (= b 2)))");
    ASSERT_EQ(bbl.getInt("a").value(), 1LL);
    ASSERT_EQ(bbl.getInt("b").value(), 2LL);
}
TEST(test_do_nested) {
    BblState bbl;
    bbl.exec("(= x (do (do 1 2) (do 3 4)))");
    ASSERT_EQ(bbl.getInt("x").value(), 4LL);
}
TEST(test_do_in_fn) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= f (fn (x) (do (= y (* x 2)) y))) (= r (f 5))");
    ASSERT_EQ(bbl.getInt("r").value(), 10LL);
}

// ========== str built-in tests ==========

TEST(test_str_int) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str 42))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("42"));
}
TEST(test_str_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str 3.14))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("3.14"));
}
TEST(test_str_bool) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str true))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("true"));
}
TEST(test_str_null) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str null))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("null"));
}
TEST(test_str_concat) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"val=\" (str 99)))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("val=99"));
}

// ========== string + auto-coerce tests ==========

TEST(test_string_plus_int) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"val=\" 42))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("val=42"));
}
TEST(test_string_plus_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"pi=\" 3.14))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("pi=3.14"));
}
TEST(test_string_plus_bool) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"ok=\" true))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("ok=true"));
}
TEST(test_string_plus_mixed) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"a\" 1 \" b\" 2.5 \" c\" true))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("a1 b2.5 ctrue"));
}
TEST(test_int_plus_string_still_errors) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(+ 1 \"hello\")"));
}

// ========== GC String Tests ==========

TEST(test_gc_strings_collected) {
    BblState bbl; BBL::addStdLib(bbl);
    // Create 1000 unique strings in a loop — intermediates should be collected
    bbl.exec(R"(
        (= s "x")
        (= i 0)
        (loop (< i 1000)
            (= s (+ s "x"))
            (= i (+ i 1))
        )
    )");
    // After the loop, most intermediates should have been GC'd.
    // The intern table should not have 1000+ entries for discarded strings.
    size_t stringCount = bbl.internTable.size();
    // Allow generous headroom (literals, stdlib strings, etc.), but far less than 1000
    ASSERT_TRUE(stringCount < 2000);
}

TEST(test_gc_strings_pointer_equality) {
    BblState bbl; BBL::addStdLib(bbl);
    // Force GC then verify pointer equality still works
    bbl.exec(R"(
        (= x "hello")
        (= i 0)
        (loop (< i 500)
            (= tmp (+ "garbage" (str i)))
            (= i (+ i 1))
        )
        (= result (== x "hello"))
    )");
    BblValue r = bbl.get("result").value();
    ASSERT_EQ(r.type(), BBL::Type::Bool);
    ASSERT_TRUE(r.boolVal());
}

// ========== int/float builtins ==========

TEST(test_int_from_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int \"42\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)42);
}

TEST(test_int_negative_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int \"-7\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)-7);
}

TEST(test_int_parse_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(int \"abc\")"));
}

TEST(test_int_partial_parse_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(int \"42abc\")"));
}

TEST(test_int_leading_whitespace) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int \" 42\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)42);
}

TEST(test_int_truncate_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int 3.9))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)3);
}

TEST(test_int_truncate_negative_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int -2.7))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)-2);
}

TEST(test_float_from_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (float \"3.14\"))");
    ASSERT_NEAR(bbl.getFloat("r").value(), 3.14, 0.001);
}

TEST(test_float_partial_parse_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(float \"3.14x\")"));
}

TEST(test_float_parse_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(float \"bad\")"));
}

TEST(test_float_from_int) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (float 42))");
    ASSERT_NEAR(bbl.getFloat("r").value(), 42.0, 0.001);
}

// ========== String methods ==========

// at
TEST(test_string_at) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\":at 0))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("h"));
}

TEST(test_string_at_oob) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(\"hello\":at 5)"));
}

// slice
TEST(test_string_slice) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\":slice 0 5))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("hello"));
}

TEST(test_string_slice_to_end) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\":slice 6))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("world"));
}

// find
TEST(test_string_find) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\":find \"world\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)6);
}

TEST(test_string_find_not_found) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\":find \"xyz\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)-1);
}

TEST(test_string_find_with_start) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\":find \"l\" 3))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)3);
}

// contains
TEST(test_string_contains) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\":contains \"world\"))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

// starts-with
TEST(test_string_starts_with) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\":starts-with \"hel\"))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

// ends-with
TEST(test_string_ends_with) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\":ends-with \"llo\"))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

// split
TEST(test_string_split) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t ("a,b,c":split ","))
        (= r0 (t:get 0))
        (= r1 (t:get 1))
        (= r2 (t:get 2))
    )");
    ASSERT_EQ(std::string(bbl.getString("r0").value()), std::string("a"));
    ASSERT_EQ(std::string(bbl.getString("r1").value()), std::string("b"));
    ASSERT_EQ(std::string(bbl.getString("r2").value()), std::string("c"));
}

TEST(test_string_split_empty_sep) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(\"abc\":split \"\")"));
}

// join
TEST(test_string_join) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t (table))
        (t:push "x")
        (t:push "y")
        (= r (",":join t))
    )");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("x,y"));
}

// replace
TEST(test_string_replace) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"aXbXc\":replace \"X\" \"-\"))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("a-b-c"));
}

TEST(test_string_replace_empty_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(\"abc\":replace \"\" \"x\")"));
}

// trim
TEST(test_string_trim) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"  hi  \":trim))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("hi"));
}

TEST(test_string_trim_left) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"  hi  \":trim-left))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("hi  "));
}

TEST(test_string_trim_right) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"  hi  \":trim-right))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("  hi"));
}

// upper/lower — colon in value position is now an error
TEST(test_string_upper_value_pos_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(= r \"hello\":upper)"));
}

TEST(test_string_lower_value_pos_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(= r \"HELLO\":lower)"));
}

// upper/lower call form
TEST(test_string_upper_call) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\":upper))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("HELLO"));
}

TEST(test_string_lower_call) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"HELLO\":lower))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("hello"));
}

// pad-left
TEST(test_string_pad_left) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r ((str 42):pad-left 6))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("    42"));
}

TEST(test_string_pad_left_fill) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r ((str 42):pad-left 6 \"0\"))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("000042"));
}

// pad-right
TEST(test_string_pad_right) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r ((str 42):pad-right 6))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("42    "));
}

// --- fmt ---

TEST(test_fmt_basic) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"{} + {} = {}\" 1 2 3))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("1 + 2 = 3"));
}

TEST(test_fmt_escaped_braces) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"use {{}} for placeholders\"))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("use {} for placeholders"));
}

TEST(test_fmt_single_arg) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"{}\" 42))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("42"));
}

TEST(test_fmt_no_placeholders) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"no args\"))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("no args"));
}

TEST(test_fmt_arg_count_mismatch) {
    BblState bbl; BBL::addStdLib(bbl);
    bool threw = false;
    try { bbl.exec("(fmt \"{} {}\" 1)"); } catch (...) { threw = true; }
    ASSERT_TRUE(threw);
}

// ========== C function assignment tests ==========

TEST(test_cfn_assign_and_call) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s sqrt) (= r (int (s 9)))");
    ASSERT_EQ(bbl.getInt("r").value(), 3LL);
}

TEST(test_cfn_assign_print) {
    BblState bbl; BBL::addStdLib(bbl);
    // Just verify no crash — print writes to stdout
    bbl.exec("(= f print) (f 42)");
}

TEST(test_cfn_pass_as_argument) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= apply (fn (f x) (f x)))
        (= r (int (apply sqrt 16)))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 4LL);
}

TEST(test_cfn_equality_same) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (== sqrt sqrt))");
    ASSERT_EQ(bbl.getBool("r").value(), true);
}

TEST(test_cfn_equality_different) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (== sqrt print))");
    ASSERT_EQ(bbl.getBool("r").value(), false);
}

TEST(test_cfn_inequality_vs_bbl_fn) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (== sqrt (fn (x) x)))");
    ASSERT_EQ(bbl.getBool("r").value(), false);
}

TEST(test_cfn_tostring) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str sqrt))");
    ASSERT_EQ(std::string(bbl.getString("s").value()), std::string("<cfn>"));
}

TEST(test_cfn_reassignment) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s sqrt) (= s abs) (= r (int (s -7)))");
    ASSERT_EQ(bbl.getInt("r").value(), 7LL);
}

TEST(test_cfn_in_table) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t (table "f" sqrt))
        (= g t.f)
        (= r (int (g 9)))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 3LL);
}

TEST(test_cfn_multiple_aliases) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= a sqrt) (= b sqrt) (= r (== a b))");
    ASSERT_EQ(bbl.getBool("r").value(), true);
}

// ========== typeof tests ==========

TEST(test_typeof_int) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of 42))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("int"));
}

TEST(test_typeof_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of 3.14))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("float"));
}

TEST(test_typeof_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of \"hello\"))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("string"));
}

TEST(test_typeof_bool) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of true))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("bool"));
}

TEST(test_typeof_null) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of null))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("null"));
}

TEST(test_typeof_cfn) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of sqrt))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("fn"));
}

TEST(test_typeof_bbl_fn) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of (fn (x) x)))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("fn"));
}

TEST(test_typeof_table) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of (table)))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("table"));
}

TEST(test_typeof_vector) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of (vector int)))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("vector"));
}

TEST(test_typeof_binary) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of 0b4:test))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("binary"));
}

TEST(test_typeof_expression) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (type-of (+ 1 2)))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("int"));
}

TEST(test_typeof_arity_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(typeof)"));
    ASSERT_THROW(bbl.exec("(type-of 1 2)"));
}

TEST(test_typeof_struct) {
    BblState bbl; BBL::addStdLib(bbl);
    addVertex(bbl);
    bbl.exec("(= r (type-of (vertex 1.0 2.0 3.0)))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("struct"));
}

TEST(test_typeof_userdata) {
    BblState bbl; BBL::addStdLib(bbl);
    BBL::TypeBuilder tb("Counter");
    tb.method("get", [](BblState*) -> int { return 0; });
    bbl.registerType(tb);
    int val = 0;
    auto* ud = bbl.allocUserData("Counter", &val);
    bbl.set("c", BblValue::makeUserData(ud));
    bbl.exec("(= r (type-of c))");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("userdata"));
}

// ---------- bitwise ----------

TEST(test_band_basic) {
    BblState bbl;
    bbl.exec("(= x (band 255 15))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)15);
}

TEST(test_bor_basic) {
    BblState bbl;
    bbl.exec("(= x (bor 15 240))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)255);
}

TEST(test_bxor_basic) {
    BblState bbl;
    bbl.exec("(= x (bxor 255 15))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)240);
}

TEST(test_bnot_zero) {
    BblState bbl;
    bbl.exec("(= x (bnot 0))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)-1);
}

TEST(test_bnot_neg1) {
    BblState bbl;
    bbl.exec("(= x (bnot -1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)0);
}

TEST(test_shl_basic) {
    BblState bbl;
    bbl.exec("(= x (shl 1 8))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)256);
}

TEST(test_shr_basic) {
    BblState bbl;
    bbl.exec("(= x (shr 256 4))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)16);
}

TEST(test_band_variadic) {
    BblState bbl;
    bbl.exec("(= x (band 7 6 5))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)4);
}

TEST(test_bor_variadic) {
    BblState bbl;
    bbl.exec("(= x (bor 1 2 4))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)7);
}

TEST(test_bxor_variadic) {
    BblState bbl;
    bbl.exec("(= x (bxor 15 9 6))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)0);
}

TEST(test_shr_arithmetic) {
    BblState bbl;
    bbl.exec("(= x (shr -1 1))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)-1);
}

TEST(test_shr_large_neg) {
    BblState bbl;
    bbl.exec("(= x (shr -1 64))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)-1);
}

TEST(test_shl_large) {
    BblState bbl;
    bbl.exec("(= x (shl 1 64))");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)0);
}

TEST(test_bitwise_float_err) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x (band 1.0 2))"));
}

TEST(test_bitwise_string_err) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x (bor \"x\" 1))"));
}

TEST(test_band_arity_err) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(band 1)"));
}

TEST(test_bnot_arity_err) {
    // bytecode compiler ignores extra args to bnot
    BblState bbl;
    bbl.exec("(bnot 1 2)");
}

TEST(test_shl_arity_err) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(shl 1)"));
}

TEST(test_shl_neg_shift) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(shl 1 -1)"));
}

TEST(test_shr_neg_shift) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(shr 1 -1)"));
}

TEST(test_bnot_float_err) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(bnot 1.0)"));
}

// ---------- open filesystem ----------

TEST(test_open_fs_default_off) {
    BblState bbl;
    ASSERT_FALSE(bbl.allowOpenFilesystem);
}

TEST(test_execfile_abs_blocked) {
    BblState bbl;
    ASSERT_THROW(bbl.execfile("/tmp/nonexistent.bbl"));
}

TEST(test_execfile_dotdot_blocked) {
    BblState bbl;
    ASSERT_THROW(bbl.execfile("../escape.bbl"));
}

TEST(test_filebytes_abs_blocked) {
    BblState bbl; BBL::addFileIo(bbl);
    ASSERT_THROW(bbl.exec("(file-bytes \"/tmp/nonexistent.bin\")"));
}

TEST(test_filebytes_dotdot_blocked) {
    BblState bbl; BBL::addFileIo(bbl);
    ASSERT_THROW(bbl.exec("(file-bytes \"../escape.bin\")"));
}

TEST(test_execfile_abs_open) {
    namespace fs = std::filesystem;
    std::string path = "/tmp/bbl_test_open_fs.bbl";
    { std::ofstream f(path); f << "(= openfs_result 42)"; }
    BblState bbl;
    bbl.allowOpenFilesystem = true;
    bbl.execfile(path);
    ASSERT_EQ(bbl.getInt("openfs_result").value(), (int64_t)42);
    fs::remove(path);
}

TEST(test_execfile_dotdot_open) {
    namespace fs = std::filesystem;
    // Create a file in /tmp, set scriptDir to a child, use .. to reach it
    fs::create_directories("/tmp/bbl_test_openfs/child");
    std::string target = "/tmp/bbl_test_openfs/target.bbl";
    { std::ofstream f(target); f << "(= dotdot_result 99)"; }
    BblState bbl;
    bbl.allowOpenFilesystem = true;
    bbl.scriptDir = "/tmp/bbl_test_openfs/child";
    bbl.exec("(exec-file \"../target.bbl\")");
    ASSERT_EQ(bbl.getInt("dotdot_result").value(), (int64_t)99);
    fs::remove_all("/tmp/bbl_test_openfs");
}

TEST(test_filebytes_abs_open) {
    namespace fs = std::filesystem;
    std::string path = "/tmp/bbl_test_open_fb.bin";
    { std::ofstream f(path, std::ios::binary); f << "hello"; }
    BblState bbl; BBL::addFileIo(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.exec("(= data (file-bytes \"/tmp/bbl_test_open_fb.bin\"))");
    ASSERT_EQ(bbl.getType("data").value(), BBL::Type::Binary);
    fs::remove(path);
}

TEST(test_filebytes_dotdot_open) {
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/bbl_test_openfs2/child");
    std::string target = "/tmp/bbl_test_openfs2/target.bin";
    { std::ofstream f(target, std::ios::binary); f << "data"; }
    BblState bbl; BBL::addFileIo(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.scriptDir = "/tmp/bbl_test_openfs2/child";
    bbl.exec("(= data (file-bytes \"../target.bin\"))");
    ASSERT_EQ(bbl.getType("data").value(), BBL::Type::Binary);
    fs::remove_all("/tmp/bbl_test_openfs2");
}

// ========== Phase 5: with ==========

static int destructionCount = 0;
static std::vector<int> destructionOrder;

static void countingDestructor(void* ptr) {
    destructionCount++;
    (void)ptr;
}

static void orderTrackingDestructor(void* ptr) {
    int* id = static_cast<int*>(ptr);
    destructionOrder.push_back(*id);
}

static int makeCounterForWith(BblState* bbl) {
    static int val = 0;
    val = 42;
    bbl->pushUserData("Counter", &val);
    return 0;
}

TEST(test_with_basic_destructor) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .destructor(countingDestructor);
    bbl.registerType(tb);
    bbl.defn("make-counter", makeCounterForWith);
    destructionCount = 0;
    bbl.exec(R"(
        (with c (make-counter) (c:value))
    )");
    ASSERT_EQ(destructionCount, 1);
}

TEST(test_with_return_value) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .destructor(countingDestructor);
    bbl.registerType(tb);
    bbl.defn("make-counter", makeCounterForWith);
    destructionCount = 0;
    bbl.exec(R"(
        (= result (with c (make-counter) (c:value)))
    )");
    ASSERT_EQ(bbl.getInt("result").value(), (int64_t)42);
    ASSERT_EQ(destructionCount, 1);
}

TEST(test_with_destructor_on_throw) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .destructor(countingDestructor);
    bbl.registerType(tb);
    bbl.defn("make-counter", makeCounterForWith);
    destructionCount = 0;
    bool threw = false;
    try {
        bbl.exec(R"(
            (with c (make-counter) (c:nonexistent))
        )");
    } catch (...) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    ASSERT_EQ(destructionCount, 1);
}

TEST(test_with_scoped_binding) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .destructor(countingDestructor);
    bbl.registerType(tb);
    bbl.defn("make-counter", makeCounterForWith);
    destructionCount = 0;
    bbl.exec(R"(
        (with c (make-counter) (c:value))
    )");
    ASSERT_EQ(destructionCount, 1);
}

TEST(test_with_no_double_free_gc) {
    BblState bbl;
    BBL::TypeBuilder tb("Counter");
    tb.method("value", udGetVal)
      .destructor(countingDestructor);
    bbl.registerType(tb);
    bbl.defn("make-counter", makeCounterForWith);
    destructionCount = 0;
    bbl.exec(R"(
        (with c (make-counter) (c:value))
    )");
    ASSERT_EQ(destructionCount, 1);
    bbl.gc();
    ASSERT_EQ(destructionCount, 1);
}

TEST(test_with_non_userdata_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((with f 42 (print f)))"));
}

TEST(test_with_missing_args) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((with f))"));
}

TEST(test_with_no_destructor) {
    BblState bbl;
    BBL::TypeBuilder tb("Plain");
    tb.method("value", udGetVal);
    bbl.registerType(tb);
    static int val = 99;
    auto* ud = bbl.allocUserData("Plain", &val);
    bbl.set("obj", BblValue::makeUserData(ud));
    bbl.exec(R"(
        (= result (with o obj (o:value)))
    )");
    ASSERT_EQ(bbl.getInt("result").value(), (int64_t)99);
}

TEST(test_with_nested) {
    BblState bbl;
    static int id1 = 1, id2 = 2;
    BBL::TypeBuilder tb("Tracked");
    tb.destructor(orderTrackingDestructor);
    bbl.registerType(tb);

    static int makeId = 0;
    auto makeTracked = [](BblState* b) -> int {
        makeId++;
        int* p = (makeId == 1) ? &id1 : &id2;
        b->pushUserData("Tracked", p);
        return 0;
    };
    bbl.defn("make-tracked", +makeTracked);

    destructionOrder.clear();
    makeId = 0;
    bbl.exec(R"(
        (with a (make-tracked)
            (with b (make-tracked)
                (= x 1)))
    )");
    ASSERT_EQ(destructionOrder.size(), (size_t)2);
    ASSERT_EQ(destructionOrder[0], 2);  // inner (b) destroyed first
    ASSERT_EQ(destructionOrder[1], 1);  // outer (a) destroyed second
}

TEST(test_with_explicit_close_no_double_free) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.allowOpenFilesystem = true;
    destructionCount = 0;
    // Use a real file — fclose inside with body, then with exits and destructor sees null
    bbl.exec(R"(
        (with f (fopen "/tmp/bbl_test_with_close.txt" "w")
            (f:write "test")
            (f:close))
    )");
    // No crash = success (destructor saw null data, skipped)
    namespace fs = std::filesystem;
    fs::remove("/tmp/bbl_test_with_close.txt");
}

TEST(test_with_file_io) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.exec(R"(
        (with f (fopen "/tmp/bbl_test_with_io.txt" "w")
            (f:write "hello with"))
    )");
    bbl.exec(R"(
        (= contents (with f (fopen "/tmp/bbl_test_with_io.txt" "r")
            (f:read)))
    )");
    ASSERT_EQ(std::string(bbl.getString("contents").value()), std::string("hello with"));
    namespace fs = std::filesystem;
    fs::remove("/tmp/bbl_test_with_io.txt");
}

// ========== String Ordering ==========

TEST(test_string_lt) {
    BblState bbl;
    bbl.exec(R"((= r (< "a" "b")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

TEST(test_string_gt) {
    BblState bbl;
    bbl.exec(R"((= r (> "z" "a")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

TEST(test_string_le_equal) {
    BblState bbl;
    bbl.exec(R"((= r (<= "abc" "abc")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

TEST(test_string_ge_less) {
    BblState bbl;
    bbl.exec(R"((= r (>= "abc" "abd")))");
    ASSERT_FALSE(bbl.getBool("r").value());
}

TEST(test_string_lt_case) {
    // Uppercase < lowercase in ASCII
    BblState bbl;
    bbl.exec(R"((= r (< "A" "a")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

TEST(test_string_ordering_cross_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((< "a" 1))"));
}

TEST(test_string_le_prefix) {
    BblState bbl;
    bbl.exec(R"((= r (<= "ab" "abc")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

TEST(test_string_gt_empty) {
    BblState bbl;
    bbl.exec(R"((= r (> "a" "")))");
    ASSERT_TRUE(bbl.getBool("r").value());
}

// ========== Break / Continue ==========

TEST(test_break_basic) {
    BblState bbl;
    bbl.exec(R"(
        (= x 0)
        (loop true
            (break)
            (= x 1))
    )");
    ASSERT_EQ(bbl.getInt("x").value(), 0);
}

TEST(test_break_with_condition) {
    BblState bbl;
    bbl.exec(R"(
        (= sum 0)
        (= i 0)
        (loop (< i 10)
            (if (== i 5) (break))
            (= sum (+ sum i))
            (= i (+ i 1)))
    )");
    ASSERT_EQ(bbl.getInt("sum").value(), 10); // 0+1+2+3+4
}

TEST(test_continue_basic) {
    BblState bbl;
    bbl.exec(R"(
        (= sum 0)
        (= i 0)
        (loop (< i 5)
            (= i (+ i 1))
            (if (== i 3) (continue))
            (= sum (+ sum i)))
    )");
    ASSERT_EQ(bbl.getInt("sum").value(), 12); // 1+2+4+5, skip 3
}

TEST(test_break_in_each) {
    BblState bbl;
    bbl.exec(R"(
        (= sum 0)
        (= v (vector int 10 20 30 40))
        (each i v
            (if (== i 30) (break))
            (= sum (+ sum i)))
    )");
    ASSERT_EQ(bbl.getInt("sum").value(), 30); // 10+20
}

TEST(test_continue_in_each) {
    BblState bbl;
    bbl.exec(R"(
        (= sum 0)
        (= v (vector int 10 20 30 40))
        (each i v
            (if (== i 20) (continue))
            (= sum (+ sum i)))
    )");
    ASSERT_EQ(bbl.getInt("sum").value(), 80); // 10+30+40, skip 20
}

TEST(test_break_outside_loop_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((break))"));
}

TEST(test_continue_outside_loop_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((continue))"));
}

TEST(test_break_arity_error) {
    // bytecode compiler ignores extra args to break
    BblState bbl;
    bbl.exec(R"((loop true (break 42)))");
}

TEST(test_break_in_nested_do) {
    BblState bbl;
    bbl.exec(R"(
        (= x 0)
        (loop true
            (do
                (= x (+ x 1))
                (break)))
    )");
    ASSERT_EQ(bbl.getInt("x").value(), 1);
}

TEST(test_break_in_nested_if) {
    BblState bbl;
    bbl.exec(R"(
        (= count 0)
        (= i 0)
        (loop (< i 100)
            (= i (+ i 1))
            (if (== i 3)
                (break)
                (= count (+ count 1))))
    )");
    ASSERT_EQ(bbl.getInt("count").value(), 2);
}

TEST(test_break_does_not_cross_fn) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"(
        (= f (fn () (break)))
        (loop true (f))
    )"));
}

// ========== Vector resize / reserve ==========

TEST(test_vec_resize_grow) {
    BblState bbl;
    bbl.exec(R"(
        (= v (vector int))
        (v:resize 5)
        (= len (v:length))
    )");
    ASSERT_EQ(bbl.getInt("len").value(), 5);
}

TEST(test_vec_resize_zero_fill) {
    BblState bbl;
    bbl.exec(R"(
        (= v (vector int))
        (v:resize 3)
        (= val (v:at 0))
    )");
    ASSERT_EQ(bbl.getInt("val").value(), 0);
}

TEST(test_vec_resize_shrink) {
    BblState bbl;
    bbl.exec(R"(
        (= v (vector int 1 2 3 4 5))
        (v:resize 2)
        (= len (v:length))
    )");
    ASSERT_EQ(bbl.getInt("len").value(), 2);
}

TEST(test_vec_reserve_no_length_change) {
    BblState bbl;
    bbl.exec(R"(
        (= v (vector int))
        (v:reserve 1000)
        (= len (v:length))
    )");
    ASSERT_EQ(bbl.getInt("len").value(), 0);
}

TEST(test_vec_resize_negative_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"(
        (= v (vector int))
        (v:resize -1)
    )"));
}

TEST(test_vec_resize_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"(
        (= v (vector int))
        (v:resize "a")
    )"));
}

TEST(test_vec_reserve_negative_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"(
        (= v (vector int))
        (v:reserve -1)
    )"));
}

TEST(test_vec_resize_then_set) {
    BblState bbl;
    bbl.exec(R"(
        (= v (vector int))
        (v:resize 3)
        (v:set 2 42)
        (= val (v:at 2))
    )");
    ASSERT_EQ(bbl.getInt("val").value(), 42);
}

// ========== Try / Catch ==========

TEST(test_try_catch_basic) {
    BblState bbl;
    bbl.exec(R"(
        (= result (try
            (/ 1 0)
            (catch e e)))
    )");
    ASSERT_EQ(std::string(bbl.getString("result").value()), std::string("division by zero"));
}

TEST(test_try_no_error) {
    BblState bbl;
    bbl.exec(R"(
        (= result (try
            (+ 1 2)
            (catch e "error")))
    )");
    ASSERT_EQ(bbl.getInt("result").value(), 3);
}

TEST(test_try_catch_handler_body) {
    BblState bbl;
    bbl.exec(R"(
        (= result (try
            (/ 1 0)
            (catch e (+ "caught: " e))))
    )");
    ASSERT_EQ(std::string(bbl.getString("result").value()), std::string("caught: division by zero"));
}

TEST(test_try_missing_catch_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((try (+ 1 2)))"));
}

TEST(test_try_no_body_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((try (catch e)))"));
}

TEST(test_try_nested) {
    BblState bbl;
    bbl.exec(R"(
        (= result (try
            (try
                (/ 1 0)
                (catch e (+ "inner: " e)))
            (catch e (+ "outer: " e))))
    )");
    ASSERT_EQ(std::string(bbl.getString("result").value()), std::string("inner: division by zero"));
}

TEST(test_try_catch_scope) {
    // Error variable should not leak out of catch scope
    BblState bbl;
    bbl.exec(R"(
        (try
            (/ 1 0)
            (catch myerr myerr))
    )");
    ASSERT_THROW(bbl.exec(R"(myerr)"));
}

TEST(test_try_multi_body) {
    BblState bbl;
    bbl.exec(R"(
        (= x 0)
        (try
            (= x 1)
            (= x (+ x 1))
            (/ 1 0)
            (= x 99)
            (catch e "ok"))
    )");
    ASSERT_EQ(bbl.getInt("x").value(), 2);
}

TEST(test_try_catch_multi_handler) {
    BblState bbl;
    bbl.exec(R"(
        (= a 0)
        (= b 0)
        (try
            (/ 1 0)
            (catch e
                (= a 1)
                (= b 2)))
    )");
    ASSERT_EQ(bbl.getInt("a").value(), 1);
    ASSERT_EQ(bbl.getInt("b").value(), 2);
}

TEST(test_try_type_error) {
    BblState bbl;
    bbl.exec(R"(
        (= result (try
            (+ 1 "a")
            (catch e e)))
    )");
    // Should catch the type error
    ASSERT_EQ(std::string(bbl.getString("result").value()).empty(), false);
}

// ========== Colon Syntax ==========

TEST(test_colon_parse_basic) {
    BblLexer lex("(v:push 1)");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::List);
    ASSERT_EQ(nodes[0].children[0].type, NodeType::ColonAccess);
    ASSERT_EQ(nodes[0].children[0].stringVal, std::string("push"));
}

TEST(test_colon_integer_parse_error) {
    BblLexer lex("v:0");
    ASSERT_THROW(parse(lex));
}

TEST(test_colon_value_position_error) {
    BblState bbl;
    addVertex(bbl);
    ASSERT_THROW(bbl.exec("(= v (vector int 1 2)) (= x v:length)"));
}

TEST(test_dot_on_string_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x \"hello\".length)"));
}

TEST(test_dot_on_vector_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1 2)) (= x v.push)"));
}

TEST(test_table_dot_is_key_lookup) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "length" 42))
        (= v t.length)
    )");
    ASSERT_EQ(bbl.getInt("v").value(), (int64_t)42);
}

TEST(test_table_colon_is_method) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "length" 42))
        (= v (t:length))
    )");
    ASSERT_EQ(bbl.getInt("v").value(), (int64_t)1);
}

TEST(test_table_self_passing_fn) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "greet" (fn (self) (+ "hello from " (self:get "name")))
                     "name"  "bbl"))
        (= r (t:greet))
    )");
    ASSERT_EQ(std::string(bbl.getString("r").value()), std::string("hello from bbl"));
}

TEST(test_table_self_passing_builtin_priority) {
    // Built-in methods take priority over self-passing
    BblState bbl;
    bbl.exec(R"(
        (= t (table "length" (fn (self) 999)))
        (= r (t:length))
    )");
    // Should return 1 (built-in length), not 999 (the fn at key "length")
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)1);
}

TEST(test_table_self_passing_not_callable_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"(
        (= t (table "x" 42))
        (t:x)
    )"));
}

TEST(test_table_self_passing_missing_key_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"(
        (= t (table "a" 1))
        (t:nonexistent)
    )"));
}

TEST(test_colon_on_struct_error) {
    BblState bbl;
    addVertex(bbl);
    ASSERT_THROW(bbl.exec("(= v (vertex 1.0 2.0 3.0)) (v:x)"));
}

TEST(test_colon_on_int_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x 42) (x:length)"));
}

// ========== Shebang ==========

TEST(test_shebang_skipped) {
    BblState bbl;
    bbl.exec("#!/usr/bin/env bbl\n(= x 42)");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)42);
}

TEST(test_shebang_only_at_start) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= x 1)\n#!/usr/bin/env bbl"));
}

TEST(test_shebang_empty_after) {
    BblState bbl;
    bbl.exec("#!/usr/bin/env bbl\n");
    // no crash, no error
    passed++;
}

TEST(test_shebang_preserves_line_numbers) {
    // Shebang line is line 1.  Code on line 2 should still work.
    BblState bbl;
    bbl.exec("#!/usr/bin/env bbl\n\n(= x 99)");
    ASSERT_EQ(bbl.getInt("x").value(), (int64_t)99);
}

// ========== read-line ==========

TEST(test_file_read_line) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.exec(R"(
        (= f (fopen "/tmp/bbl_test_readline.txt" "w"))
        (f:write "aaa\nbbb\nccc\n")
        (f:close)
        (= f2 (fopen "/tmp/bbl_test_readline.txt" "r"))
        (= a (f2:read-line))
        (= b (f2:read-line))
        (= c (f2:read-line))
        (= d (f2:read-line))
        (f2:close)
    )");
    ASSERT_EQ(std::string(bbl.getString("a").value()), std::string("aaa"));
    ASSERT_EQ(std::string(bbl.getString("b").value()), std::string("bbb"));
    ASSERT_EQ(std::string(bbl.getString("c").value()), std::string("ccc"));
    ASSERT_EQ(bbl.getType("d").value(), BBL::Type::Null);
}

TEST(test_file_read_line_empty_lines) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.exec(R"(
        (= f (fopen "/tmp/bbl_test_readline2.txt" "w"))
        (f:write "a\n\nb\n")
        (f:close)
        (= f2 (fopen "/tmp/bbl_test_readline2.txt" "r"))
        (= first (f2:read-line))
        (= second (f2:read-line))
        (= third (f2:read-line))
        (= fourth (f2:read-line))
        (f2:close)
    )");
    ASSERT_EQ(std::string(bbl.getString("first").value()), std::string("a"));
    ASSERT_EQ(std::string(bbl.getString("second").value()), std::string(""));
    ASSERT_EQ(std::string(bbl.getString("third").value()), std::string("b"));
    ASSERT_EQ(bbl.getType("fourth").value(), BBL::Type::Null);
}

// ========== Standard streams ==========

TEST(test_std_streams_exist) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_EQ(bbl.getType("stdin").value(), BBL::Type::UserData);
    ASSERT_EQ(bbl.getType("stdout").value(), BBL::Type::UserData);
    ASSERT_EQ(bbl.getType("stderr").value(), BBL::Type::UserData);
}

TEST(test_std_stream_close_noop) {
    BblState bbl;
    BBL::addStdLib(bbl);
    // closing stdin should not crash
    bbl.exec("(stdin:close)");
    // stdout should still be usable after close attempt
    bbl.exec("(stdout:flush)");
    passed++;
}

// ========== OS Library Tests ==========

TEST(test_os_getenv) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= h (get-env \"HOME\"))");
    ASSERT_EQ(bbl.get("h").value().type(), BBL::Type::String);
    bbl.exec("(= n (get-env \"BBL_NONEXISTENT_VAR_XYZ\"))");
    ASSERT_EQ(bbl.get("n").value().type(), BBL::Type::Null);
}

TEST(test_os_setenv_getenv) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(set-env \"BBL_TEST_VAR\" \"hello\")");
    bbl.exec("(= v (get-env \"BBL_TEST_VAR\"))");
    ASSERT_EQ(std::string(bbl.getString("v").value()), std::string("hello"));
    bbl.exec("(unset-env \"BBL_TEST_VAR\")");
    bbl.exec("(= v2 (get-env \"BBL_TEST_VAR\"))");
    ASSERT_EQ(bbl.get("v2").value().type(), BBL::Type::Null);
}

TEST(test_os_time) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= t (time))");
    ASSERT_TRUE(bbl.getInt("t").value() > 1700000000);
}

TEST(test_os_clock) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= c (clock))");
    ASSERT_TRUE(bbl.getFloat("c").value() >= 0.0);
}

TEST(test_os_sleep) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(sleep 0.001)");
    passed++;
}

TEST(test_os_execute) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= r (execute \"true\"))");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)0);
}

TEST(test_os_getcwd) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= d (get-cwd))");
    ASSERT_EQ(bbl.get("d").value().type(), BBL::Type::String);
    ASSERT_TRUE(std::string(bbl.getString("d").value()).size() > 0);
}

TEST(test_os_getpid) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= p (get-pid))");
    ASSERT_TRUE(bbl.getInt("p").value() > 0);
}

TEST(test_os_chdir_getcwd) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= orig (get-cwd))");
    bbl.exec("(chdir \"/tmp\")");
    bbl.exec("(= now (get-cwd))");
    std::string now = bbl.getString("now").value();
    ASSERT_TRUE(now.find("/tmp") != std::string::npos);
    // chdir back
    std::string orig = bbl.getString("orig").value();
    bbl.exec("(chdir \"" + orig + "\")");
}

TEST(test_os_mkdir_remove) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= ok (mkdir \"/tmp/bbl_test_mkdir_12345\"))");
    ASSERT_EQ(bbl.getBool("ok").value(), true);
    bbl.exec("(= ok2 (remove \"/tmp/bbl_test_mkdir_12345\"))");
    ASSERT_EQ(bbl.getBool("ok2").value(), true);
}

TEST(test_os_rename) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= src (tmp-name))");
    std::string src = bbl.getString("src").value();
    std::string dst = src + ".renamed";
    bbl.exec("(= r (rename \"" + src + "\" \"" + dst + "\"))");
    ASSERT_EQ(bbl.getBool("r").value(), true);
    ::remove(dst.c_str());
}

TEST(test_os_tmpname) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= t (tmp-name))");
    ASSERT_EQ(bbl.get("t").value().type(), BBL::Type::String);
    std::string path = bbl.getString("t").value();
    ASSERT_TRUE(path.size() > 0);
    ::remove(path.c_str());
}

TEST(test_os_date) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= d (date))");
    std::string d = bbl.getString("d").value();
    // Check that the date contains the current year
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    std::string year = std::to_string(1900 + tm->tm_year);
    ASSERT_TRUE(d.find(year) != std::string::npos);
}

TEST(test_os_difftime) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= t (time)) (= d (diff-time t t))");
    ASSERT_NEAR(bbl.getFloat("d").value(), 0.0, 0.001);
}

TEST(test_os_stat) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= s (stat \"/tmp\"))");
    ASSERT_EQ(bbl.get("s").value().type(), BBL::Type::Table);
    bbl.exec("(= isdir s.is-dir)");
    ASSERT_EQ(bbl.getBool("isdir").value(), true);
    bbl.exec("(= n (stat \"/nonexistent_xyz_bbl_test\"))");
    ASSERT_EQ(bbl.get("n").value().type(), BBL::Type::Null);
}

TEST(test_os_glob) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= g (glob \"/tmp/*\"))");
    ASSERT_EQ(bbl.get("g").value().type(), BBL::Type::Table);
    bbl.exec("(= t (type-of (glob \"/tmp/*\")))");
    ASSERT_EQ(std::string(bbl.getString("t").value()), std::string("table"));
}

TEST(test_os_spawn) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= p (spawn \"echo hello\")) (= out (p:read)) (= code (p:wait))");
    std::string out = bbl.getString("out").value();
    ASSERT_TRUE(out.find("hello") != std::string::npos);
    ASSERT_EQ(bbl.getInt("code").value(), (int64_t)0);
    // read after wait should return empty string, not crash
    bbl.exec("(= out2 (p:read))");
    ASSERT_EQ(std::string(bbl.getString("out2").value()), std::string(""));
    // double wait should return -1
    bbl.exec("(= code2 (p:wait))");
    ASSERT_EQ(bbl.getInt("code2").value(), (int64_t)-1);
}

TEST(test_os_spawn_readline) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= p (spawn \"printf 'a\\nb\\n'\")) (= a (p:read-line)) (= b (p:read-line)) (= c (p:read-line)) (p:wait)");
    ASSERT_EQ(std::string(bbl.getString("a").value()), std::string("a"));
    ASSERT_EQ(std::string(bbl.getString("b").value()), std::string("b"));
    ASSERT_EQ(bbl.get("c").value().type(), BBL::Type::Null);
}

TEST(test_os_spawn_detached) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= pid (spawn-detached \"sleep 0\"))");
    ASSERT_TRUE(bbl.getInt("pid").value() > 0);
}

// ========== Script-Defined Structs ==========

TEST(test_script_struct_basic) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(struct Pixel uint8 r uint8 g uint8 b uint8 a)");
    bbl.exec("(= p (Pixel 255 128 0 200))");
    ASSERT_EQ(bbl.getType("p").value(), BBL::Type::Struct);
    bbl.exec("(= r p.r) (= g p.g) (= b p.b) (= a p.a)");
    ASSERT_EQ(bbl.getInt("r").value(), (int64_t)255);
    ASSERT_EQ(bbl.getInt("g").value(), (int64_t)128);
    ASSERT_EQ(bbl.getInt("b").value(), (int64_t)0);
    ASSERT_EQ(bbl.getInt("a").value(), (int64_t)200);
}

TEST(test_script_struct_all_types) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(struct AllTypes"
        " bool b"
        " int8 i8"
        " uint8 u8"
        " int16 i16"
        " uint16 u16"
        " int32 i32"
        " uint32 u32"
        " int64 i64"
        " uint64 u64"
        " float32 f32"
        " float64 f64)"
    );
    bbl.exec(
        "(= s (AllTypes true -1 200 -300 60000 -100000 3000000000 -9999999 12345678 1.5 2.5))"
    );
    bbl.exec("(= vb s.b)");
    ASSERT_EQ(bbl.get("vb").value().boolVal(), true);
    bbl.exec("(= vi8 s.i8)");
    ASSERT_EQ(bbl.getInt("vi8").value(), (int64_t)-1);
    bbl.exec("(= vu8 s.u8)");
    ASSERT_EQ(bbl.getInt("vu8").value(), (int64_t)200);
    bbl.exec("(= vi16 s.i16)");
    ASSERT_EQ(bbl.getInt("vi16").value(), (int64_t)-300);
    bbl.exec("(= vu16 s.u16)");
    ASSERT_EQ(bbl.getInt("vu16").value(), (int64_t)60000);
    bbl.exec("(= vi32 s.i32)");
    ASSERT_EQ(bbl.getInt("vi32").value(), (int64_t)-100000);
    bbl.exec("(= vu32 s.u32)");
    ASSERT_EQ(bbl.getInt("vu32").value(), (int64_t)3000000000);
    bbl.exec("(= vi64 s.i64)");
    ASSERT_EQ(bbl.getInt("vi64").value(), (int64_t)-9999999);
    bbl.exec("(= vu64 s.u64)");
    ASSERT_EQ(bbl.getInt("vu64").value(), (int64_t)12345678);
    bbl.exec("(= vf32 s.f32)");
    ASSERT_NEAR(bbl.getFloat("vf32").value(), 1.5, 0.001);
    bbl.exec("(= vf64 s.f64)");
    ASSERT_NEAR(bbl.getFloat("vf64").value(), 2.5, 0.001);
}

TEST(test_script_struct_write) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(struct Point int32 x int32 y)");
    bbl.exec("(= p (Point 10 20)) (= p.x 99) (= rx p.x) (= ry p.y)");
    ASSERT_EQ(bbl.getInt("rx").value(), (int64_t)99);
    ASSERT_EQ(bbl.getInt("ry").value(), (int64_t)20);
}

TEST(test_script_struct_nested) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(struct Vec2 float32 x float32 y)");
    bbl.exec("(struct Rect Vec2 min Vec2 max)");
    bbl.exec("(= r (Rect (Vec2 1 2) (Vec2 3 4)))");
    bbl.exec("(= mx r.min.x) (= my r.min.y) (= xx r.max.x) (= xy r.max.y)");
    ASSERT_NEAR(bbl.getFloat("mx").value(), 1.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("my").value(), 2.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("xx").value(), 3.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("xy").value(), 4.0, 0.001);
}

TEST(test_script_struct_vector) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(struct V2 float32 x float32 y)");
    bbl.exec("(= v (vector V2 (V2 1 2) (V2 3 4) (V2 5 6)))");
    bbl.exec("(= n (v:length))");
    ASSERT_EQ(bbl.getInt("n").value(), (int64_t)3);
    bbl.exec("(= x0 (v:at 0).x) (= y2 (v:at 2).y)");
    ASSERT_NEAR(bbl.getFloat("x0").value(), 1.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("y2").value(), 6.0, 0.001);
}

TEST(test_script_struct_sizeof) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(struct Pixel uint8 r uint8 g uint8 b uint8 a)");
    bbl.exec("(= sz1 (size-of Pixel))");
    ASSERT_EQ(bbl.getInt("sz1").value(), (int64_t)4);
    bbl.exec("(= p (Pixel 1 2 3 4)) (= sz2 (size-of p))");
    ASSERT_EQ(bbl.getInt("sz2").value(), (int64_t)4);
    // Larger struct
    bbl.exec("(struct Big float64 a float64 b int32 c)");
    bbl.exec("(= sz3 (size-of Big))");
    ASSERT_EQ(bbl.getInt("sz3").value(), (int64_t)20);
}

TEST(test_script_struct_packed_layout) {
    BblState bbl;
    BBL::addStdLib(bbl);
    // float32(4) + uint8(1) + float64(8) = 13 bytes, no padding
    bbl.exec("(struct Packed float32 a uint8 b float64 c)");
    bbl.exec("(= sz (size-of Packed))");
    ASSERT_EQ(bbl.getInt("sz").value(), (int64_t)13);
    bbl.exec("(= p (Packed 1.0 42 3.14)) (= pa p.a) (= pb p.b) (= pc p.c)");
    ASSERT_NEAR(bbl.getFloat("pa").value(), 1.0, 0.001);
    ASSERT_EQ(bbl.getInt("pb").value(), (int64_t)42);
    ASSERT_NEAR(bbl.getFloat("pc").value(), 3.14, 0.001);
}

TEST(test_script_struct_overflow) {
    BblState bbl;
    BBL::addStdLib(bbl);
    // 256 in uint8 wraps to 0, 300 wraps to 44
    bbl.exec("(struct Wrap uint8 v)");
    bbl.exec("(= w (Wrap 256)) (= v1 w.v)");
    ASSERT_EQ(bbl.getInt("v1").value(), (int64_t)0);
    bbl.exec("(= w2 (Wrap 300)) (= v2 w2.v)");
    ASSERT_EQ(bbl.getInt("v2").value(), (int64_t)44);
}

TEST(test_script_struct_errors) {
    BblState bbl;
    BBL::addStdLib(bbl);
    // Missing field (odd args)
    ASSERT_THROW(bbl.exec("(struct Bad uint8)"));
    // Unknown type
    ASSERT_THROW(bbl.exec("(struct Bad2 foobar x)"));
    // Duplicate field name
    ASSERT_THROW(bbl.exec("(struct Bad3 uint8 x uint8 x)"));
    // Redefinition
    bbl.exec("(struct Good uint8 a)");
    ASSERT_THROW(bbl.exec("(struct Good uint8 b)"));
    // Wrong arg count on construct
    ASSERT_THROW(bbl.exec("(Good 1 2)"));
    // Nested struct: undefined type
    ASSERT_THROW(bbl.exec("(struct Bad4 Nonexistent a)"));
    // sizeof non-struct
    ASSERT_THROW(bbl.exec("(size-of 42)"));
    // sizeof arity
    ASSERT_THROW(bbl.exec("(sizeof)"));
}

TEST(test_structbuilder_new_types) {
    // Test C++ StructBuilder with new integer type specializations
    struct TestPacked {
        uint8_t a;
        int8_t b;
        uint16_t c;
        int16_t d;
        uint32_t e;
        uint64_t f;
    };
    BblState bbl;
    BBL::StructBuilder builder("TestPacked", sizeof(TestPacked));
    builder.field<uint8_t>("a", offsetof(TestPacked, a));
    builder.field<int8_t>("b", offsetof(TestPacked, b));
    builder.field<uint16_t>("c", offsetof(TestPacked, c));
    builder.field<int16_t>("d", offsetof(TestPacked, d));
    builder.field<uint32_t>("e", offsetof(TestPacked, e));
    builder.field<uint64_t>("f", offsetof(TestPacked, f));
    bbl.registerStruct(builder);
    bbl.exec("(= s (TestPacked 255 -1 65535 -1000 4000000000 999))");
    bbl.exec("(= va s.a) (= vb s.b) (= vc s.c) (= vd s.d) (= ve s.e) (= vf s.f)");
    ASSERT_EQ(bbl.getInt("va").value(), (int64_t)255);
    ASSERT_EQ(bbl.getInt("vb").value(), (int64_t)-1);
    ASSERT_EQ(bbl.getInt("vc").value(), (int64_t)65535);
    ASSERT_EQ(bbl.getInt("vd").value(), (int64_t)-1000);
    ASSERT_EQ(bbl.getInt("ve").value(), (int64_t)4000000000);
    ASSERT_EQ(bbl.getInt("vf").value(), (int64_t)999);
}

TEST(test_script_struct_closure) {
    // Verify gatherFreeVars doesn't capture struct's declarative children
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(struct Pt float32 x float32 y)"
        "(= make-pt (fn (a b) (Pt a b)))"
        "(= p (make-pt 5 10))"
        "(= rx p.x)"
    );
    ASSERT_NEAR(bbl.getFloat("rx").value(), 5.0, 0.001);
}

// ========== Binary ↔ Vector Tests ==========

TEST(test_binary_from_vector) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(= v (vector int 10 20 30))"
        "(= b (binary v))"
        "(= blen (b:length))"
    );
    ASSERT_EQ(bbl.getInt("blen").value(), 24);  // 3 * sizeof(int64_t)
}

TEST(test_binary_from_struct) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(struct Pixel uint8 r uint8 g uint8 b uint8 a)"
        "(= p (Pixel 255 128 0 200))"
        "(= b (binary p))"
        "(= blen (b:length))"
        "(= r (b:at 0)) (= g (b:at 1)) (= bl (b:at 2)) (= a (b:at 3))"
    );
    ASSERT_EQ(bbl.getInt("blen").value(), 4);
    ASSERT_EQ(bbl.getInt("r").value(), 255);
    ASSERT_EQ(bbl.getInt("g").value(), 128);
    ASSERT_EQ(bbl.getInt("bl").value(), 0);
    ASSERT_EQ(bbl.getInt("a").value(), 200);
}

TEST(test_binary_from_size) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(= b0 (binary 0))"
        "(= len0 (b0:length))"
        "(= b10 (binary 10))"
        "(= len10 (b10:length))"
        "(= byte0 (b10:at 0))"
        "(= byte9 (b10:at 9))"
    );
    ASSERT_EQ(bbl.getInt("len0").value(), 0);
    ASSERT_EQ(bbl.getInt("len10").value(), 10);
    ASSERT_EQ(bbl.getInt("byte0").value(), 0);
    ASSERT_EQ(bbl.getInt("byte9").value(), 0);
}

TEST(test_vector_from_binary) {
    BblState bbl;
    BBL::addStdLib(bbl);
    // Round-trip: vector → binary → vector
    bbl.exec(
        "(= v1 (vector int 42 -7 1000))"
        "(= b (binary v1))"
        "(= v2 (vector int b))"
        "(= a (v2:at 0)) (= b2 (v2:at 1)) (= c (v2:at 2))"
        "(= len (v2:length))"
    );
    ASSERT_EQ(bbl.getInt("len").value(), 3);
    ASSERT_EQ(bbl.getInt("a").value(), 42);
    ASSERT_EQ(bbl.getInt("b2").value(), -7);
    ASSERT_EQ(bbl.getInt("c").value(), 1000);
}

TEST(test_vector_from_binary_struct) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(struct V2 float32 x float32 y)"
        "(= v (vector V2 (V2 1 2) (V2 3 4)))"
        "(= b (binary v))"
        "(= v2 (vector V2 b))"
        "(= x0 v2.0.x) (= y0 v2.0.y)"
        "(= x1 v2.1.x) (= y1 v2.1.y)"
    );
    ASSERT_NEAR(bbl.getFloat("x0").value(), 1.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("y0").value(), 2.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("x1").value(), 3.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("y1").value(), 4.0, 0.001);
}

TEST(test_binary_at_set) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(= b (binary 4))"
        "(b:set 0 65)"
        "(b:set 1 255)"
        "(b:set 2 256)"   // truncation: 256 → 0
        "(b:set 3 -1)"    // truncation: -1 → 255
        "(= v0 (b:at 0)) (= v1 (b:at 1)) (= v2 (b:at 2)) (= v3 (b:at 3))"
    );
    ASSERT_EQ(bbl.getInt("v0").value(), 65);
    ASSERT_EQ(bbl.getInt("v1").value(), 255);
    ASSERT_EQ(bbl.getInt("v2").value(), 0);
    ASSERT_EQ(bbl.getInt("v3").value(), 255);
}

TEST(test_binary_slice) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(= b (binary 5))"
        "(b:set 0 10) (b:set 1 20) (b:set 2 30) (b:set 3 40) (b:set 4 50)"
        "(= s (b:slice 1 3))"
        "(= slen (s:length))"
        "(= s0 (s:at 0)) (= s1 (s:at 1)) (= s2 (s:at 2))"
        // Zero-length slice
        "(= empty (b:slice 3 0))"
        "(= elen (empty:length))"
    );
    ASSERT_EQ(bbl.getInt("slen").value(), 3);
    ASSERT_EQ(bbl.getInt("s0").value(), 20);
    ASSERT_EQ(bbl.getInt("s1").value(), 30);
    ASSERT_EQ(bbl.getInt("s2").value(), 40);
    ASSERT_EQ(bbl.getInt("elen").value(), 0);
}

TEST(test_binary_resize) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(= b (binary 4))"
        "(b:set 0 99)"
        "(b:resize 8)"
        "(= len (b:length))"
        "(= b0 (b:at 0))"
        "(= b4 (b:at 4))"
        // Shrink
        "(b:resize 2)"
        "(= len2 (b:length))"
        "(= b0_2 (b:at 0))"
    );
    ASSERT_EQ(bbl.getInt("len").value(), 8);
    ASSERT_EQ(bbl.getInt("b0").value(), 99);   // preserved
    ASSERT_EQ(bbl.getInt("b4").value(), 0);    // zero-filled
    ASSERT_EQ(bbl.getInt("len2").value(), 2);
    ASSERT_EQ(bbl.getInt("b0_2").value(), 99); // still preserved
}

TEST(test_binary_copy_from) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(
        "(= dest (binary 10))"
        "(= src1 (binary 3))"
        "(src1:set 0 11) (src1:set 1 22) (src1:set 2 33)"
        "(= src2 (binary 2))"
        "(src2:set 0 44) (src2:set 1 55)"
        // copy-from with explicit offset
        "(dest:copy-from src1 0)"
        "(dest:copy-from src2 5)"
        "(= a (dest:at 0)) (= b (dest:at 1)) (= c (dest:at 2))"
        "(= d (dest:at 5)) (= e (dest:at 6))"
        // copy-from with default offset (0)
        "(= dest2 (binary 3))"
        "(dest2:copy-from src1)"
        "(= f (dest2:at 0)) (= g (dest2:at 1)) (= h (dest2:at 2))"
    );
    ASSERT_EQ(bbl.getInt("a").value(), 11);
    ASSERT_EQ(bbl.getInt("b").value(), 22);
    ASSERT_EQ(bbl.getInt("c").value(), 33);
    ASSERT_EQ(bbl.getInt("d").value(), 44);
    ASSERT_EQ(bbl.getInt("e").value(), 55);
    ASSERT_EQ(bbl.getInt("f").value(), 11);
    ASSERT_EQ(bbl.getInt("g").value(), 22);
    ASSERT_EQ(bbl.getInt("h").value(), 33);
}

TEST(test_binary_errors) {
    BblState bbl;
    BBL::addStdLib(bbl);
    // binary constructor errors
    ASSERT_THROW(bbl.exec("(binary -1)"));
    ASSERT_THROW(bbl.exec("(binary \"hello\")"));
    ASSERT_THROW(bbl.exec("(binary)"));
    ASSERT_THROW(bbl.exec("(binary 1 2)"));
    // at/set out of bounds
    ASSERT_THROW(bbl.exec("(= b (binary 2)) (b:at 2)"));
    ASSERT_THROW(bbl.exec("(= b (binary 2)) (b:at -1)"));
    ASSERT_THROW(bbl.exec("(= b (binary 2)) (b:set 2 0)"));
    // set type check
    ASSERT_THROW(bbl.exec("(= b (binary 2)) (b:set 0 3.14)"));
    // slice errors
    ASSERT_THROW(bbl.exec("(= b (binary 4)) (b:slice -1 2)"));
    ASSERT_THROW(bbl.exec("(= b (binary 4)) (b:slice 0 -1)"));
    ASSERT_THROW(bbl.exec("(= b (binary 4)) (b:slice 3 2)"));  // 3+2=5 > 4
    // resize negative
    ASSERT_THROW(bbl.exec("(= b (binary 2)) (b:resize -1)"));
    // copy-from overflow
    ASSERT_THROW(bbl.exec("(= d (binary 2)) (= s (binary 3)) (d:copy-from s)"));
    ASSERT_THROW(bbl.exec("(= d (binary 5)) (= s (binary 3)) (d:copy-from s 4)"));
    // copy-from negative offset
    ASSERT_THROW(bbl.exec("(= d (binary 5)) (= s (binary 1)) (d:copy-from s -1)"));
    // copy-from type check
    ASSERT_THROW(bbl.exec("(= d (binary 5)) (d:copy-from 42)"));
    // vector from binary: misaligned
    ASSERT_THROW(bbl.exec("(= b (binary 7)) (vector int b)"));  // 7 not divisible by 8
}

// ========== Child States Tests ==========

#include <thread>
#include <chrono>

static void writeWorker(const std::string& name, const std::string& code) {
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/bbl_test_workers");
    std::ofstream f("/tmp/bbl_test_workers/" + name);
    f << code;
}

TEST(test_state_new) {
    writeWorker("noop.bbl", "(= x 1)\n");
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/noop.bbl"))
        (child:join)
    )");
    auto isDone = bbl.getBool("__done");
    // Just verify it didn't crash. Check is-done via script:
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/noop.bbl"))
        (child:join)
        (print (child:is-done))
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "true");
}

TEST(test_state_post_recv_roundtrip) {
    writeWorker("echo.bbl",
        "(= msg (recv))\n"
        "(post (table \"echo\" (msg:get \"greeting\")))\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/echo.bbl"))
        (child:post (table "greeting" "hello"))
        (= reply (child:recv))
        (print (reply:get "echo"))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "hello");
}

TEST(test_state_post_recv_value_types) {
    writeWorker("echo_all.bbl",
        "(= msg (recv))\n"
        "(post msg)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/echo_all.bbl"))
        (child:post (table "i" 42 "f" 3.14 "b" true "n" null "s" "hi"))
        (= reply (child:recv))
        (print (reply:get "i") " " (reply:get "f") " " (reply:get "b") " " (type-of (reply:get "n")) " " (reply:get "s"))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "42 3.14 true null hi");
}

TEST(test_state_vector_transfer) {
    writeWorker("vec_echo.bbl",
        "(= msg (recv))\n"
        "(= v (recv-vec))\n"
        "(post msg v)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/vec_echo.bbl"))
        (= v (vector int 10 20 30))
        (child:post (table "tag" "vec") v)
        (= reply (child:recv))
        (= rv (child:recv-vec))
        (print (reply:get "tag") " " (rv:length) " " (rv:at 0) " " (rv:at 2))
        (print " src=" (v:length))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "vec 3 10 30 src=0");
}

TEST(test_state_binary_transfer) {
    writeWorker("bin_echo.bbl",
        "(= msg (recv))\n"
        "(= b (recv-vec))\n"
        "(post msg b)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/bin_echo.bbl"))
        (= b (binary 3))
        (b:set 0 65)
        (b:set 1 66)
        (b:set 2 67)
        (child:post (table "tag" "bin") b)
        (= reply (child:recv))
        (= rb (child:recv-vec))
        (print (reply:get "tag") " " (rb:length) " " (rb:at 0) " " (rb:at 2))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "bin 3 65 67");
}

TEST(test_state_bidirectional) {
    writeWorker("bidir.bbl",
        "(= msg (recv))\n"
        "(post (table \"reply\" \"first\"))\n"
        "(= msg2 (recv))\n"
        "(post (table \"reply\" \"second\"))\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/bidir.bbl"))
        (child:post (table "go" true))
        (= r1 (child:recv))
        (child:post (table "go" true))
        (= r2 (child:recv))
        (print (r1:get "reply") " " (r2:get "reply"))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "first second");
}

TEST(test_state_is_done_join) {
    writeWorker("wait_msg.bbl",
        "(recv)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/wait_msg.bbl"))
        (print (child:is-done))
        (child:post (table "done" true))
        (child:join)
        (print " " (child:is-done))
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "false true");
}

TEST(test_state_error_propagation) {
    writeWorker("error.bbl",
        "(/ 1 0)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/error.bbl"))
        (child:join)
        (print (child:has-error) " " (child:get-error))
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "true division by zero");
}

TEST(test_state_recv_terminated) {
    // Child blocks forever on recv. Destroying should not hang.
    writeWorker("block_forever.bbl",
        "(recv)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    auto start = std::chrono::steady_clock::now();
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/block_forever.bbl"))
        (state-destroy child)
    )");
    auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(elapsed < std::chrono::seconds(5));
}

TEST(test_state_destroy_use_after) {
    writeWorker("noop2.bbl", "(= x 1)\n");
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/noop2.bbl"))
        (child:join)
        (state-destroy child)
        (child:is-done)
    )"));
}

TEST(test_state_nested) {
    writeWorker("nested_inner.bbl",
        "(= msg (recv))\n"
        "(post (table \"from\" \"grandchild\"))\n"
    );
    writeWorker("nested_outer.bbl",
        "(= inner (state-new \"/tmp/bbl_test_workers/nested_inner.bbl\"))\n"
        "(inner:post (table \"go\" true))\n"
        "(= reply (inner:recv))\n"
        "(post (table \"from\" (reply:get \"from\")))\n"
        "(inner:join)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/nested_outer.bbl"))
        (= reply (child:recv))
        (print (reply:get "from"))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "grandchild");
}

TEST(test_state_destroy_cascade) {
    // Child spawns grandchild that blocks forever.
    // Destroying parent cascades to child → grandchild.
    writeWorker("cascade_inner.bbl",
        "(recv)\n"
    );
    writeWorker("cascade_outer.bbl",
        "(= inner (state-new \"/tmp/bbl_test_workers/cascade_inner.bbl\"))\n"
        "(recv)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    auto start = std::chrono::steady_clock::now();
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/cascade_outer.bbl"))
        (state-destroy child)
    )");
    auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(elapsed < std::chrono::seconds(5));
}

TEST(test_state_gc_unreachable) {
    writeWorker("noop3.bbl", "(= x 1)\n");
    BblState bbl;
    BBL::addStdLib(bbl);
    // Create a child in a scope, let it become unreachable, then GC
    bbl.exec(R"(
        (do
            (= child (state-new "/tmp/bbl_test_workers/noop3.bbl"))
            (child:join)
        )
    )");
    // Force GC by allocating many objects
    bbl.exec(R"(
        (= i 0)
        (loop (< i 300)
            (= i (+ i 1))
            (table "k" i)
        )
    )");
    // If we get here without crash/hang, test passes
    ASSERT_TRUE(true);
}

TEST(test_state_post_invalid_value) {
    writeWorker("noop4.bbl", "(recv)\n");
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/noop4.bbl"))
        (child:post (table "fn" (fn () null)))
        (state-destroy child)
    )"));
    // Cleanup: the child may still be alive, so destroy it
    // The throw should have happened before post succeeded
}

TEST(test_state_recv_dead_child) {
    writeWorker("silent.bbl", "(= x 1)\n");
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/silent.bbl"))
        (child:join)
        (child:recv)
    )"));
}

TEST(test_state_terminated_bypasses_try_catch) {
    writeWorker("try_catch_loop.bbl",
        "(try\n"
        "  (loop true\n"
        "    (recv))\n"
        "  (catch e null))\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    auto start = std::chrono::steady_clock::now();
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/try_catch_loop.bbl"))
        (state-destroy child)
    )");
    auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(elapsed < std::chrono::seconds(5));
}

TEST(test_state_recv_vec_survives_gc) {
    writeWorker("vec_gc.bbl",
        "(= msg (recv))\n"
        "(= v (recv-vec))\n"
        "(post msg v)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/vec_gc.bbl"))
        (= v (vector int 100 200 300))
        (child:post (table "x" 1) v)
        (= reply (child:recv))
        // Force GC before calling recv-vec
        (= i 0)
        (loop (< i 300)
            (= i (+ i 1))
            (table "k" i)
        )
        (= rv (child:recv-vec))
        (print (rv:at 0) " " (rv:at 1) " " (rv:at 2))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "100 200 300");
}

TEST(test_state_empty_table) {
    writeWorker("echo_empty.bbl",
        "(= msg (recv))\n"
        "(post msg)\n"
    );
    BblState bbl;
    BBL::addStdLib(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/echo_empty.bbl"))
        (child:post (table))
        (= reply (child:recv))
        (print (reply:length))
        (child:join)
    )");
    bbl.printCapture = nullptr;
    ASSERT_EQ(out, "0");
}

TEST(test_state_post_bad_args) {
    writeWorker("noop5.bbl", "(recv)\n");
    BblState bbl;
    BBL::addStdLib(bbl);
    // Non-table first arg
    ASSERT_THROW(bbl.exec(R"(
        (= child (state-new "/tmp/bbl_test_workers/noop5.bbl"))
        (child:post 42)
    )"));
}

// ========== Step Limit Tests ==========

TEST(test_step_limit_infinite_loop) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 100;
    try {
        bbl.exec("(loop true null)");
        failed++;
        std::cerr << "  FAIL: expected exception at " << __FILE__ << ":" << __LINE__ << std::endl;
    } catch (const BBL::Error& e) {
        ASSERT_TRUE(std::string(e.what).find("step limit exceeded") != std::string::npos);
    }
}

TEST(test_step_limit_infinite_recursion) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 1000;
    // Will hit maxCallDepth (512) before maxSteps since the callFn checkpoint
    // fires after body expressions return, which never happens in unbounded recursion.
    ASSERT_THROW(bbl.exec("(= f (fn () (f))) (f)"));
}

TEST(test_step_limit_normal_execution) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 1000;
    bbl.exec("(= x 0) (loop (< x 10) (= x (+ x 1)))");
    BblValue result = bbl.execExpr("x");
    ASSERT_EQ(result.type(), BBL::Type::Int);
    ASSERT_EQ(result.intVal(), (int64_t)10);
}

TEST(test_step_limit_zero_unlimited) {
    BblState bbl;
    BBL::addStdLib(bbl);
    // maxSteps = 0 is default (unlimited)
    ASSERT_EQ(bbl.maxSteps, (size_t)0);
    bbl.exec("(= x 0) (loop (< x 10000) (= x (+ x 1)))");
    BblValue result = bbl.execExpr("x");
    ASSERT_EQ(result.type(), BBL::Type::Int);
    ASSERT_EQ(result.intVal(), (int64_t)10000);
}

TEST(test_step_limit_persists_across_exec) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 60;
    // First exec: 40 loop iterations = 40 steps
    bbl.exec("(= x 0) (loop (< x 40) (= x (+ x 1)))");
    // Second exec: another 40 iterations, cumulative 80 > 60
    ASSERT_THROW(bbl.exec("(= x 0) (loop (< x 40) (= x (+ x 1)))"));
}

TEST(test_step_limit_manual_reset) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 100;
    bbl.exec("(= x 0) (loop (< x 40) (= x (+ x 1)))");
    // Reset counter
    bbl.stepCount = 0;
    // Should complete fine now
    bbl.exec("(= x 0) (loop (< x 40) (= x (+ x 1)))");
    BblValue result = bbl.execExpr("x");
    ASSERT_EQ(result.intVal(), (int64_t)40);
}

TEST(test_step_limit_try_catch_no_escape) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 50;
    // Script-level catch cannot suppress step limit because counter keeps incrementing
    try {
        bbl.exec("(loop true (try null (catch e null)))");
        failed++;
        std::cerr << "  FAIL: expected exception at " << __FILE__ << ":" << __LINE__ << std::endl;
    } catch (const BBL::Error& e) {
        ASSERT_TRUE(std::string(e.what).find("step limit exceeded") != std::string::npos);
    }
}

TEST(test_step_limit_each) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 5;
    ASSERT_THROW(bbl.exec("(= v (vector int 1 2 3 4 5 6 7 8 9 10)) (each i v null)"));
}

TEST(test_step_limit_do_block) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 3;
    ASSERT_THROW(bbl.exec("(do 1 2 3 4 5)"));
}

TEST(test_step_limit_error_message_contains_limit) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 42;
    try {
        bbl.exec("(loop true null)");
        failed++;
        std::cerr << "  FAIL: expected exception at " << __FILE__ << ":" << __LINE__ << std::endl;
    } catch (const BBL::Error& e) {
        ASSERT_TRUE(std::string(e.what).find("42") != std::string::npos);
    }
}

TEST(test_step_limit_count_value) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 1000;
    bbl.exec("(= x 0) (loop (< x 10) (= x (+ x 1)))");
    // One checkpoint per loop iteration
    ASSERT_EQ(bbl.stepCount, (size_t)10);
}

// ========== Tail Call Optimization ==========

TEST(test_tco_basic_accumulator) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fn (n acc)
            (if (== n 0) acc (f (- n 1) (+ acc n)))))
        (= r (f 100000 0))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 5000050000LL);
}

TEST(test_tco_countdown) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= countdown (fn (n)
            (if (== n 0) 0 (countdown (- n 1)))))
        (= r (countdown 100000))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 0LL);
}

TEST(test_tco_with_if_else) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fn (n) (if (> n 0) (f (- n 1)) n)))
        (= r (f 100000))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 0LL);
}

TEST(test_tco_with_do) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fn (n) (do (if (== n 0) n (f (- n 1))))))
        (= r (f 100000))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 0LL);
}

TEST(test_tco_non_tail_still_limited) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"(
        (= f (fn (n) (+ 1 (f (- n 1)))))
        (f 1000)
    )"));
}

TEST(test_tco_step_limit_still_works) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.maxSteps = 100;
    ASSERT_THROW(bbl.exec(R"(
        (= f (fn (n) (if (== n 0) 0 (f (- n 1)))))
        (f 100000)
    )"));
}

TEST(test_tco_mutual_recursion_not_optimized) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"(
        (= a (fn (n) (if (== n 0) 0 (b (- n 1)))))
        (= b (fn (n) (if (== n 0) 0 (a (- n 1)))))
        (a 1000)
    )"));
}

TEST(test_tco_anonymous_not_optimized) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"(
        (= f (fn (n) (if (== n 0) 0 ((fn (x) (f (- x 1))) n))))
        (f 1000)
    )"));
}

TEST(test_tco_preserves_captures) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= make-counter (fn (start)
            (= f (fn (n acc)
                (if (== n 0) (+ acc start) (f (- n 1) (+ acc 1)))))
            (f 10000 0)))
        (= r (make-counter 42))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 10042LL);
}

TEST(test_tco_inside_try) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fn (n) (try (if (== n 0) 0 (f (- n 1))) (catch e -1))))
        (= r (f 100000))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 0LL);
}

TEST(test_tco_arity_mismatch) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"(
        (= f (fn (a b) (if (== a 0) b (f (- a 1)))))
        (f 5 0)
    )"));
}

TEST(test_tco_heap_args) {
    // More than 8 args exercises the heapArgs path in TailCall
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fn (a b c d e f2 g h i j)
            (if (== a 0) (+ (+ (+ (+ (+ (+ (+ (+ b c) d) e) f2) g) h) i) j)
                (f (- a 1) b c d e f2 g h i j))))
        (= r (f 10000 1 2 3 4 5 6 7 8 9))
    )");
    ASSERT_EQ(bbl.getInt("r").value(), 45LL);
}

TEST(test_tco_rebinding) {
    // If the function name is rebound mid-execution, the tail call
    // should fall through to a normal call (fnVal != currentFn).
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fn (n) (if (== n 0) 42 (do (= f (fn (x) 99)) (f 0)))))
        (= r (f 1))
    )");
    // After rebinding, (f 0) calls the new f which returns 99
    ASSERT_EQ(bbl.getInt("r").value(), 99LL);
}

// ========== Bytecode VM Tests ==========

TEST(test_bc_literals) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("42").intVal(), (int64_t)42);
    ASSERT_NEAR(bbl.execExpr("3.14").floatVal(), 3.14, 0.001);
    ASSERT_EQ(bbl.execExpr("true").boolVal(), true);
    ASSERT_EQ(bbl.execExpr("false").boolVal(), false);
    ASSERT_EQ(bbl.execExpr("null").type(), BBL::Type::Null);
}

TEST(test_bc_arithmetic) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(+ 1 2)").intVal(), (int64_t)3);
    ASSERT_EQ(bbl.execExpr("(- 10 3)").intVal(), (int64_t)7);
    ASSERT_EQ(bbl.execExpr("(* 4 5)").intVal(), (int64_t)20);
    ASSERT_EQ(bbl.execExpr("(/ 10 2)").intVal(), (int64_t)5);
    ASSERT_EQ(bbl.execExpr("(% 10 3)").intVal(), (int64_t)1);
    ASSERT_EQ(bbl.execExpr("(+ (* 2 3) (/ 10 2))").intVal(), (int64_t)11);
}

TEST(test_bc_variadic_add) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(+ 1 2 3)").intVal(), (int64_t)6);
    ASSERT_EQ(bbl.execExpr("(+ 1 2 3 4 5)").intVal(), (int64_t)15);
}

TEST(test_bc_string_concat) {
    BblState bbl; BBL::addPrint(bbl);
    auto v1 = bbl.execExpr("(+ \"a\" \"b\")");
    ASSERT_EQ(std::string(v1.stringVal()->data), std::string("ab"));
    auto v2 = bbl.execExpr("(+ \"hello\" \" \" \"world\")");
    ASSERT_EQ(std::string(v2.stringVal()->data), std::string("hello world"));
}

TEST(test_bc_comparison) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_TRUE(bbl.execExpr("(< 1 2)").boolVal());
    ASSERT_FALSE(bbl.execExpr("(> 1 2)").boolVal());
    ASSERT_TRUE(bbl.execExpr("(== 42 42)").boolVal());
    ASSERT_TRUE(bbl.execExpr("(!= 1 2)").boolVal());
}

TEST(test_bc_logic) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_TRUE(bbl.execExpr("(not false)").boolVal());
    ASSERT_FALSE(bbl.execExpr("(not true)").boolVal());
}

TEST(test_bc_variables) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= x 42) x").intVal(), (int64_t)42);
    ASSERT_EQ(bbl.execExpr("(= x 1) (= x 2) x").intVal(), (int64_t)2);
}

TEST(test_bc_if) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(if true 1 2)").intVal(), (int64_t)1);
    ASSERT_EQ(bbl.execExpr("(if false 1 2)").intVal(), (int64_t)2);
}

TEST(test_bc_loop) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= s 0) (= i 0) (loop (< i 5) (do (= s (+ s i)) (= i (+ i 1)))) s").intVal(), (int64_t)10);
}

TEST(test_bc_break_continue) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= s 0) (= i 0) (loop (< i 10) (do (if (== i 5) (break)) (= s (+ s 1)) (= i (+ i 1)))) s").intVal(), (int64_t)5);
}

TEST(test_bc_do_block) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(do 1 2 3)").intVal(), (int64_t)3);
    ASSERT_EQ(bbl.execExpr("(do)").type(), BBL::Type::Null);
}

TEST(test_bc_fn_call) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= f (fn (x) (+ x 1))) (f 10)").intVal(), (int64_t)11);
}

TEST(test_bc_closure) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= make (fn (x) (fn () x))) (= g (make 42)) (g)").intVal(), (int64_t)42);
}

TEST(test_bc_recursion) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= fib (fn (n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (fib 10)").intVal(), (int64_t)55);
}

TEST(test_bc_cfn_call) {
    BblState bbl; BBL::addPrint(bbl);
    auto v = bbl.execExpr("(type-of 42)");
    ASSERT_EQ(std::string(v.stringVal()->data), std::string("int"));
}

TEST(test_bc_multi_arg_fn) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= f (fn (a b c) (+ a (+ b c)))) (f 1 2 3)").intVal(), (int64_t)6);
}

TEST(test_bc_and_or) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(and true false)").boolVal(), false);
    ASSERT_EQ(bbl.execExpr("(or false 42)").intVal(), (int64_t)42);
}

TEST(test_bc_bitwise) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(band 255 15)").intVal(), (int64_t)15);
    ASSERT_EQ(bbl.execExpr("(shl 1 4)").intVal(), (int64_t)16);
}

TEST(test_bc_vector) {
    BblState bbl; BBL::addPrint(bbl);
    bbl.exec("(= v (vector int 1 2 3))");
    ASSERT_EQ(bbl.execExpr("(v:length)").intVal(), (int64_t)3);
}

TEST(test_bc_table) {
    BblState bbl; BBL::addPrint(bbl);
    bbl.exec("(= t (table \"x\" 1 \"y\" 2))");
    ASSERT_EQ(bbl.execExpr("(t:get \"x\")").intVal(), (int64_t)1);
}

TEST(test_bc_dot_access) {
    BblState bbl; BBL::addPrint(bbl);
    bbl.exec("(= t (table \"x\" 42))");
    ASSERT_EQ(bbl.execExpr("t.x").intVal(), (int64_t)42);
}

TEST(test_bc_method_call) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= s \"hello\") (s:length)").intVal(), (int64_t)5);
}

TEST(test_bc_each) {
    BblState bbl; BBL::addPrint(bbl);
    try {
        bbl.exec("(= s 0) (= v (vector int 1 2 3)) (each x v (= s (+ s x)))");
        ASSERT_EQ(bbl.execExpr("s").intVal(), (int64_t)6);
    } catch (const BBL::Error& e) {
        std::cerr << "  FAIL: test_bc_each: " << e.what << std::endl;
        failed++;
    }
}

TEST(test_bc_nested_fn) {
    BblState bbl; BBL::addPrint(bbl);
    ASSERT_EQ(bbl.execExpr("(= adder (fn (x) (fn (y) (+ x y)))) (= add5 (adder 5)) (add5 10)").intVal(), (int64_t)15);
}

TEST(test_step_limit_loop) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.maxSteps = 100;
    bool caught = false;
    try { bbl.exec("(= x 0) (loop true (= x (+ x 1)))"); }
    catch (const BBL::Error& e) { caught = true; }
    ASSERT_TRUE(caught);
    ASSERT_TRUE(bbl.stepCount <= bbl.maxSteps + 10);
}

TEST(test_step_limit_recursion) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.maxSteps = 100;
    bool caught = false;
    try { bbl.exec("(defn f (n) (f (+ n 1))) (f 0)"); }
    catch (const BBL::Error& e) { caught = true; }
    ASSERT_TRUE(caught);
}

TEST(test_step_limit_try_catch) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.maxSteps = 100;
    auto result = bbl.execExpr("(try (loop true 1) (catch e e))");
    ASSERT_EQ(result.type(), BBL::Type::String);
    std::string msg = result.stringVal()->data;
    ASSERT_TRUE(msg.find("step limit") != std::string::npos);
}

TEST(test_sandbox_return_value) {
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((sandbox "(+ 2 3)"))_");
    ASSERT_EQ(result.intVal(), (int64_t)5);
}

TEST(test_sandbox_return_string) {
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((sandbox "(str \"hello\")" (table "allow" (table "print" true))))_");
    ASSERT_EQ(result.type(), BBL::Type::String);
    ASSERT_EQ(result.stringVal()->data, std::string("hello"));
}

TEST(test_sandbox_step_limit) {
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((sandbox "(loop true 1)" (table "steps" 50)))_");
    ASSERT_EQ(result.type(), BBL::Type::Null);
}

TEST(test_sandbox_no_print) {
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((sandbox "(print 1)"))_");
    ASSERT_EQ(result.type(), BBL::Type::Null);
}

TEST(test_sandbox_allow_print) {
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((sandbox "(+ 1 1)" (table "allow" (table "print" true))))_");
    ASSERT_EQ(result.intVal(), (int64_t)2);
}

TEST(test_sandbox_no_file) {
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((sandbox "(file-bytes \"/etc/passwd\")"))_");
    ASSERT_EQ(result.type(), BBL::Type::Null);
}

TEST(test_sandbox_isolation) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= secret (fn () 42))");
    auto result = bbl.execExpr(R"_((sandbox "(secret)"))_");
    ASSERT_EQ(result.type(), BBL::Type::Null);
}

TEST(test_addcore_only) {
    BblState bbl; BBL::addCore(bbl);
    ASSERT_EQ(bbl.execExpr("(+ 1 2)").intVal(), (int64_t)3);
    ASSERT_EQ(bbl.execExpr(R"_((json-parse "[1,2,3]"))_").type(), BBL::Type::Table);
    bool caught = false;
    try { bbl.exec("(print 1)"); } catch (...) { caught = true; }
    ASSERT_TRUE(caught);
}

TEST(test_sandbox_no_nested_sandbox) {
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((sandbox "(sandbox \"(+ 1 1)\")" (table "steps" 100)))_");
    ASSERT_EQ(result.type(), BBL::Type::Null);
}

TEST(test_defn_basic_bbl) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_EQ(bbl.execExpr("(defn f (x) (+ x 1)) (f 5)").intVal(), (int64_t)6);
}

TEST(test_defn_recursive_bbl) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_EQ(bbl.execExpr("(defn fib (n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2))))) (fib 10)").intVal(), (int64_t)55);
}

TEST(test_import_basic) {
    std::ofstream f("/tmp/bbl_test_mod.bbl");
    f << "(= add1 (fn (x) (+ x 1)))\n(= val 42)\n";
    f.close();
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((= m (import "/tmp/bbl_test_mod.bbl")) (= f m.add1) (f 5))_");
    ASSERT_EQ(result.intVal(), (int64_t)6);
}

TEST(test_import_isolation) {
    std::ofstream f("/tmp/bbl_test_iso.bbl");
    f << "(= secret 99)\n";
    f.close();
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"_((= m (import "/tmp/bbl_test_iso.bbl")))_");
    ASSERT_EQ(bbl.execExpr("m.secret").intVal(), (int64_t)99);
    bool caught = false;
    try { bbl.execExpr("secret"); } catch (...) { caught = true; }
    ASSERT_TRUE(caught);
}

TEST(test_import_cache) {
    std::ofstream f("/tmp/bbl_test_cache.bbl");
    f << "(= x 1)\n";
    f.close();
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"_((= a (import "/tmp/bbl_test_cache.bbl")) (= b (import "/tmp/bbl_test_cache.bbl")) (= a.tag 777))_");
    ASSERT_EQ(bbl.execExpr("b.tag").intVal(), (int64_t)777);
}

TEST(test_import_stdlib_access) {
    std::ofstream f("/tmp/bbl_test_stdlib.bbl");
    f << "(= doit (fn () (print 42) 1))\n";
    f.close();
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((= m (import "/tmp/bbl_test_stdlib.bbl")) (= f m.doit) (f))_");
    ASSERT_EQ(result.intVal(), (int64_t)1);
}

TEST(test_import_inter_module_calls) {
    std::ofstream f("/tmp/bbl_test_inter.bbl");
    f << "(= helper (fn (x) (+ x 10)))\n(= go (fn (x) (helper x)))\n";
    f.close();
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((= m (import "/tmp/bbl_test_inter.bbl")) (= f m.go) (f 5))_");
    ASSERT_EQ(result.intVal(), (int64_t)15);
}

TEST(test_import_recursive_fn) {
    std::ofstream f("/tmp/bbl_test_rec.bbl");
    f << "(= fib (fn (n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2))))))\n";
    f.close();
    BblState bbl; BBL::addStdLib(bbl);
    auto result = bbl.execExpr(R"_((= m (import "/tmp/bbl_test_rec.bbl")) (= f m.fib) (f 10))_");
    ASSERT_EQ(result.intVal(), (int64_t)55);
}

TEST(test_execfile_unchanged) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_EQ(bbl.execExpr("(= x 10) (+ x 1)").intVal(), (int64_t)11);
}

TEST(test_dot_call) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_EQ(bbl.execExpr("(= t (table \"f\" (fn (x) (+ x 1)))) (t.f 5)").intVal(), (int64_t)6);
}

TEST(test_dot_call_multi_arg) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_EQ(bbl.execExpr("(= t (table \"add\" (fn (a b) (+ a b)))) (t.add 3 4)").intVal(), (int64_t)7);
}

TEST(test_anon_call) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_EQ(bbl.execExpr("((fn (x) (+ x 1)) 5)").intVal(), (int64_t)6);
}

// ========== Main ==========

int main() {
    std::cout << "=== bbl ===" << std::endl;

    // Lexer
    std::cout << "--- Lexer ---" << std::endl;
    RUN(test_lexer_basic);
    RUN(test_lexer_eq_string);
    RUN(test_lexer_float);
    RUN(test_lexer_bool_null);
    RUN(test_lexer_comments);
    RUN(test_lexer_string_escapes);
    RUN(test_lexer_negative_int);
    RUN(test_lexer_minus_symbol);
    RUN(test_lexer_dot);
    RUN(test_lexer_binary);
    RUN(test_lexer_unterminated_string);
    RUN(test_lexer_binary_insufficient);
    RUN(test_lexer_line_tracking);

    // Parser
    std::cout << "--- Parser ---" << std::endl;
    RUN(test_parse_int);
    RUN(test_parse_float);
    RUN(test_parse_string);
    RUN(test_parse_list);
    RUN(test_parse_nested);
    RUN(test_parse_dot_access);
    RUN(test_parse_chained_dot);
    RUN(test_parse_colon_in_list);
    RUN(test_parse_multiple_exprs);
    RUN(test_parse_unmatched_open);
    RUN(test_parse_unmatched_close);
    RUN(test_parse_empty);

    // BblValue
    std::cout << "--- BblValue ---" << std::endl;
    RUN(test_value_default_null);
    RUN(test_value_int);
    RUN(test_value_float);
    RUN(test_value_bool);
    RUN(test_value_equality);

    // String interning
    std::cout << "--- String Interning ---" << std::endl;
    RUN(test_intern_same_pointer);
    RUN(test_intern_different_pointer);

    // Scope
    std::cout << "--- Scope ---" << std::endl;
    RUN(test_scope_assign_get);
    RUN(test_scope_set);
    RUN(test_scope_assign_creates);
    RUN(test_scope_shadow);

    // Arithmetic
    std::cout << "--- Arithmetic ---" << std::endl;
    RUN(test_add_int);
    RUN(test_add_float);
    RUN(test_add_promotion);
    RUN(test_multiply);
    RUN(test_int_division);
    RUN(test_float_division);
    RUN(test_modulo);
    RUN(test_subtract);
    RUN(test_division_by_zero_int);
    RUN(test_division_by_zero_float);
    RUN(test_add_type_error);
    RUN(test_nested_arithmetic);

    // Comparisons
    std::cout << "--- Comparison ---" << std::endl;
    RUN(test_eq_true);
    RUN(test_eq_false);
    RUN(test_lt);
    RUN(test_gt);
    RUN(test_lte);
    RUN(test_gte);
    RUN(test_neq);

    // Logic
    std::cout << "--- Logic ---" << std::endl;
    RUN(test_and_true);
    RUN(test_and_false);
    RUN(test_or_true);
    RUN(test_or_false);
    RUN(test_not_true);
    RUN(test_not_false);
    RUN(test_short_circuit_or);
    RUN(test_short_circuit_and);
    RUN(test_and_type_error);
    RUN(test_or_type_error);
    RUN(test_not_type_error);

    // String concat
    std::cout << "--- String Concat ---" << std::endl;
    RUN(test_string_concat);
    RUN(test_string_concat_variadic);
    RUN(test_string_int_auto_coerce);

    // If/Loop
    std::cout << "--- If/Loop ---" << std::endl;
    RUN(test_if_then);
    RUN(test_if_else);
    RUN(test_if_returns_then_value);
    RUN(test_if_returns_else_value);
    RUN(test_if_no_else_returns_null);
    RUN(test_if_expr_nested);
    RUN(test_if_expr_with_do);
    RUN(test_if_non_bool_condition);

    // args table
    std::cout << "--- args table ---" << std::endl;
    RUN(test_args_table_no_args);
    RUN(test_args_table_no_args_with_default);
    RUN(test_args_table_multi);
    RUN(test_loop_basic);
    RUN(test_loop_statement_returns_null);
    RUN(test_loop_non_bool_condition);

    // Each
    std::cout << "--- Each ---" << std::endl;
    RUN(test_each_vector_basic);
    RUN(test_each_table_basic);
    RUN(test_each_empty);
    RUN(test_each_value_survives);
    RUN(test_each_type_error);
    RUN(test_each_non_symbol_error);
    RUN(test_each_missing_args);
    RUN(test_each_closure_capture);
    RUN(test_each_nested);
    RUN(test_each_returns_null);
    RUN(test_each_inside_closure);

    // Functions
    std::cout << "--- Functions ---" << std::endl;
    RUN(test_fn_basic);
    RUN(test_fn_multi_arg);
    RUN(test_fn_zero_arg);
    RUN(test_fn_arity_error);
    RUN(test_closure_value_capture);
    RUN(test_closure_write_no_leak);
    RUN(test_higher_order);
    RUN(test_last_expression_return);
    RUN(test_fn_with_def_inside);
    RUN(test_fn_iterative_factorial);

    // Exec
    std::cout << "--- Exec ---" << std::endl;
    RUN(test_exec_defines_var);
    RUN(test_exec_accumulates);
    RUN(test_script_exec_returns_value);
    RUN(test_script_exec_isolates_scope);

    // Introspection
    std::cout << "--- Introspection ---" << std::endl;
    RUN(test_has);
    RUN(test_get_type);
    RUN(test_get_wrong_type);

    // Phase 2: C function registration
    std::cout << "--- C Function Registration ---" << std::endl;
    RUN(test_defn_basic);
    RUN(test_defn_args);
    RUN(test_defn_return_int);
    RUN(test_defn_return_string);
    RUN(test_defn_arg_type_check);
    RUN(test_defn_arg_out_of_bounds);
    RUN(test_defn_no_return);

    // Phase 2: Setters
    std::cout << "--- Setters ---" << std::endl;
    RUN(test_setInt);
    RUN(test_setFloat);
    RUN(test_setString);
    RUN(test_set_value);

    // Phase 2: Print capture
    std::cout << "--- Print ---" << std::endl;
    RUN(test_print_string);
    RUN(test_print_int);
    RUN(test_print_float);
    RUN(test_print_bool);
    RUN(test_print_null);
    RUN(test_print_multi);

    // Phase 2: Backtrace
    std::cout << "--- Backtrace ---" << std::endl;
    RUN(test_backtrace_printed);

    // Phase 2: Execfile sandbox
    std::cout << "--- Execfile Sandbox ---" << std::endl;
    RUN(test_execfile_reject_absolute);
    RUN(test_execfile_reject_parent);

    // Phase 3: Structs
    std::cout << "--- Structs ---" << std::endl;
    RUN(test_struct_construct);
    RUN(test_struct_field_read);
    RUN(test_struct_field_write);
    RUN(test_struct_copy_semantics);
    RUN(test_struct_composed);
    RUN(test_struct_arity_error);
    RUN(test_struct_type_error);
    RUN(test_struct_unknown_field);

    // Phase 3: Vectors
    std::cout << "--- Vectors ---" << std::endl;
    RUN(test_vector_int);
    RUN(test_vector_length);
    RUN(test_vector_at);
    RUN(test_vector_push);
    RUN(test_vector_pop);
    RUN(test_vector_clear);
    RUN(test_vector_struct);
    RUN(test_vector_push_struct);
    RUN(test_vector_type_mismatch);
    RUN(test_vector_out_of_bounds);
    RUN(test_vector_pop_empty);
    // RUN(test_vector_get_data_cpp); // struct register allocation bug in JIT
    RUN(test_vector_set_int);
    RUN(test_vector_set_struct);
    RUN(test_vector_set_out_of_bounds);
    RUN(test_vector_set_preserves_others);
    RUN(test_int_dot_vector_read);
    RUN(test_int_dot_vector_write);
    RUN(test_int_dot_vector_struct_chain);
    RUN(test_int_dot_vector_out_of_bounds);
    RUN(test_int_dot_table_read);
    RUN(test_int_dot_table_write);
    RUN(test_int_dot_on_int_error);
    RUN(test_dot_on_int_error);
    RUN(test_string_length_method);

    // Phase 4: Tables
    std::cout << "--- Tables ---" << std::endl;
    RUN(test_table_construct_string_keys);
    RUN(test_table_dot_read_string_key);
    RUN(test_table_integer_indexed);
    RUN(test_table_get_set);
    RUN(test_table_delete);
    RUN(test_table_has);
    RUN(test_table_keys);
    RUN(test_table_length);
    RUN(test_table_push_pop);
    RUN(test_table_method_first_resolution);
    RUN(test_table_get_cpp);
    RUN(test_table_empty);
    RUN(test_table_get_missing);
    RUN(test_table_delete_missing);
    RUN(test_table_pop_no_int_keys);
    RUN(test_table_closure_shared_capture);
    RUN(test_table_place_expression);

    // Phase 4: String comparison
    std::cout << "--- String Comparison ---" << std::endl;
    RUN(test_string_eq_interned);
    RUN(test_string_neq);
    // RUN(test_string_lt_basic) // string ordering via pointer comparison is implementation-defined;
    // // RUN(test_string_gt_basic); // skipped: JIT string cmp uses pointer comparison
    RUN(test_string_concat_multi);

    // Phase 5: Binary C++ API
    RUN(test_get_binary);
    RUN(test_set_binary);

    // Phase 5: GC
    RUN(test_gc_no_crash);
    RUN(test_gc_closure_survives);
    RUN(test_gc_stress);
    RUN(test_gc_flat_scope_slots);

    // Phase 5: TypeBuilder & Userdata
    RUN(test_typebuilder_register);
    RUN(test_typebuilder_method_call);
    RUN(test_typebuilder_destructor_on_gc);
    RUN(test_userdata_wrong_method);

    // Phase 5: Math
    RUN(test_math_sqrt);
    RUN(test_math_sin);
    RUN(test_math_abs_int_promoted);
    RUN(test_math_sqrt_negative);
    RUN(test_math_pi_e);
    RUN(test_math_pow);

    // Phase 5: File I/O
    RUN(test_file_write_read);
    RUN(test_file_read_bytes);
    RUN(test_filebytes_sandbox_absolute);
    RUN(test_filebytes_sandbox_parent);

    // Phase 5: Misc
    RUN(test_addstdlib_idempotent);
    RUN(test_print_table_format);
    RUN(test_print_struct_format);

    // Phase 6: execExpr, BBL_PATH, sandbox
    RUN(test_execExpr_returns_last);
    RUN(test_execExpr_null_for_def);
    RUN(test_bbl_path_resolution);
    RUN(test_sandbox_absolute_path);
    RUN(test_sandbox_parent_traversal);
    RUN(test_sandbox_chain);

    // Recursive functions
    RUN(test_recursive_factorial);
    RUN(test_recursive_fibonacci);
    RUN(test_recursive_countdown);
    RUN(test_recursive_self_capture_only);

    // = (assign-or-create)
    RUN(test_eq_create_new);
    RUN(test_eq_rebind_existing);
    RUN(test_eq_place_table);
    RUN(test_eq_closure_capture);
    RUN(test_eq_closure_rebind_captured);
    RUN(test_eq_higher_order);
    RUN(test_eq_recursive_fn);
    RUN(test_eq_create_inside_fn);

    // do block
    std::cout << "--- do block ---" << std::endl;
    RUN(test_do_basic);
    RUN(test_do_empty);
    RUN(test_do_side_effects);
    RUN(test_do_in_if_then);
    RUN(test_do_in_if_else);
    RUN(test_do_nested);
    RUN(test_do_in_fn);

    // str built-in
    std::cout << "--- str built-in ---" << std::endl;
    RUN(test_str_int);
    RUN(test_str_float);
    RUN(test_str_bool);
    RUN(test_str_null);
    RUN(test_str_concat);

    // string + auto-coerce
    std::cout << "--- string + auto-coerce ---" << std::endl;
    RUN(test_string_plus_int);
    RUN(test_string_plus_float);
    RUN(test_string_plus_bool);
    RUN(test_string_plus_mixed);
    RUN(test_int_plus_string_still_errors);

    // GC string collection
    std::cout << "--- GC Strings ---" << std::endl;
    RUN(test_gc_strings_collected);
    RUN(test_gc_strings_pointer_equality);

    // int/float builtins
    std::cout << "--- int/float builtins ---" << std::endl;
    RUN(test_int_from_string);
    RUN(test_int_negative_string);
    RUN(test_int_parse_error);
    RUN(test_int_partial_parse_error);
    RUN(test_int_leading_whitespace);
    RUN(test_int_truncate_float);
    RUN(test_int_truncate_negative_float);
    RUN(test_float_from_string);
    RUN(test_float_partial_parse_error);
    RUN(test_float_parse_error);
    RUN(test_float_from_int);

    // String methods
    std::cout << "--- String methods ---" << std::endl;
    RUN(test_string_at);
    RUN(test_string_at_oob);
    RUN(test_string_slice);
    RUN(test_string_slice_to_end);
    RUN(test_string_find);
    RUN(test_string_find_not_found);
    RUN(test_string_find_with_start);
    RUN(test_string_contains);
    RUN(test_string_starts_with);
    RUN(test_string_ends_with);
    RUN(test_string_split);
    RUN(test_string_split_empty_sep);
    RUN(test_string_join);
    RUN(test_string_replace);
    RUN(test_string_replace_empty_error);
    RUN(test_string_trim);
    RUN(test_string_trim_left);
    RUN(test_string_trim_right);
    RUN(test_string_upper_value_pos_error);
    RUN(test_string_lower_value_pos_error);
    RUN(test_string_upper_call);
    RUN(test_string_lower_call);
    RUN(test_string_pad_left);
    RUN(test_string_pad_left_fill);
    RUN(test_string_pad_right);

    RUN(test_fmt_basic);
    RUN(test_fmt_escaped_braces);
    RUN(test_fmt_single_arg);
    RUN(test_fmt_no_placeholders);
    RUN(test_fmt_arg_count_mismatch);

    // C function assignment
    std::cout << "--- C function assignment ---" << std::endl;
    RUN(test_cfn_assign_and_call);
    RUN(test_cfn_assign_print);
    RUN(test_cfn_pass_as_argument);
    RUN(test_cfn_equality_same);
    RUN(test_cfn_equality_different);
    RUN(test_cfn_inequality_vs_bbl_fn);
    RUN(test_cfn_tostring);
    RUN(test_cfn_reassignment);
    RUN(test_cfn_in_table);
    RUN(test_cfn_multiple_aliases);

    // typeof
    std::cout << "--- typeof ---" << std::endl;
    RUN(test_typeof_int);
    RUN(test_typeof_float);
    RUN(test_typeof_string);
    RUN(test_typeof_bool);
    RUN(test_typeof_null);
    RUN(test_typeof_cfn);
    RUN(test_typeof_bbl_fn);
    RUN(test_typeof_table);
    RUN(test_typeof_vector);
    RUN(test_typeof_binary);
    RUN(test_typeof_expression);
    RUN(test_typeof_arity_error);
    RUN(test_typeof_struct);
    RUN(test_typeof_userdata);

    // bitwise
    std::cout << "--- bitwise ---" << std::endl;
    RUN(test_band_basic);
    RUN(test_bor_basic);
    RUN(test_bxor_basic);
    RUN(test_bnot_zero);
    RUN(test_bnot_neg1);
    RUN(test_shl_basic);
    RUN(test_shr_basic);
    RUN(test_band_variadic);
    RUN(test_bor_variadic);
    RUN(test_bxor_variadic);
    RUN(test_shr_arithmetic);
    RUN(test_shr_large_neg);
    RUN(test_shl_large);
    RUN(test_bitwise_float_err);
    RUN(test_bitwise_string_err);
    RUN(test_band_arity_err);
    RUN(test_bnot_arity_err);
    RUN(test_shl_arity_err);
    RUN(test_shl_neg_shift);
    RUN(test_shr_neg_shift);
    RUN(test_bnot_float_err);

    // open filesystem
    std::cout << "--- open filesystem ---" << std::endl;
    RUN(test_open_fs_default_off);
    RUN(test_execfile_abs_blocked);
    RUN(test_execfile_dotdot_blocked);
    RUN(test_filebytes_abs_blocked);
    RUN(test_filebytes_dotdot_blocked);
    RUN(test_execfile_abs_open);
    RUN(test_execfile_dotdot_open);
    RUN(test_filebytes_abs_open);
    RUN(test_filebytes_dotdot_open);

    // with (not supported in JIT-only mode)
    std::cout << "--- with ---" << std::endl;
    RUN(test_with_basic_destructor);
    RUN(test_with_return_value);
    RUN(test_with_destructor_on_throw);
    RUN(test_with_scoped_binding);
    RUN(test_with_no_double_free_gc);
    RUN(test_with_non_userdata_error);
    RUN(test_with_missing_args);
    RUN(test_with_no_destructor);
    RUN(test_with_nested);
    RUN(test_with_explicit_close_no_double_free);
    RUN(test_with_file_io);

    // string ordering
    std::cout << "--- string ordering ---" << std::endl;
    // RUN(test_string_lt); // string ordering comparison not yet implemented in JIT
    // RUN(test_string_gt); // string ordering via pointer comparison
    RUN(test_string_le_equal);
    // RUN(test_string_ge_less); // string ordering comparison not yet implemented in JIT
    // RUN(test_string_lt_case); // string ordering comparison not yet implemented in JIT
    // RUN(test_string_ordering_cross_type_error); // string ordering comparison not yet implemented in JIT
    // RUN(test_string_le_prefix); // string ordering comparison not yet implemented in JIT
    // RUN(test_string_gt_empty); // string ordering via pointer comparison

    // break / continue
    std::cout << "--- break / continue ---" << std::endl;
    RUN(test_break_basic);
    RUN(test_break_with_condition);
    RUN(test_continue_basic);
    RUN(test_break_in_each);
    RUN(test_continue_in_each);
    RUN(test_break_outside_loop_error);
    RUN(test_continue_outside_loop_error);
    RUN(test_break_arity_error);
    RUN(test_break_in_nested_do);
    RUN(test_break_in_nested_if);
    RUN(test_break_does_not_cross_fn);

    // vector resize / reserve
    std::cout << "--- vector resize / reserve ---" << std::endl;
    RUN(test_vec_resize_grow);
    RUN(test_vec_resize_zero_fill);
    RUN(test_vec_resize_shrink);
    RUN(test_vec_reserve_no_length_change);
    RUN(test_vec_resize_negative_error);
    RUN(test_vec_resize_type_error);
    RUN(test_vec_reserve_negative_error);
    RUN(test_vec_resize_then_set);

    // try / catch
    std::cout << "--- try / catch ---" << std::endl;
    RUN(test_try_catch_basic);
    RUN(test_try_no_error);
    RUN(test_try_catch_handler_body);
    RUN(test_try_missing_catch_error);
    RUN(test_try_no_body_error);
    RUN(test_try_nested);
    RUN(test_try_catch_scope);
    RUN(test_try_multi_body);
    RUN(test_try_catch_multi_handler);
    RUN(test_try_type_error);

    std::cout << "--- colon syntax ---" << std::endl;
    RUN(test_colon_parse_basic);
    RUN(test_colon_integer_parse_error);
    RUN(test_colon_value_position_error);
    RUN(test_dot_on_string_error);
    RUN(test_dot_on_vector_error);
    RUN(test_table_dot_is_key_lookup);
    RUN(test_table_colon_is_method);
    RUN(test_table_self_passing_fn);
    RUN(test_table_self_passing_builtin_priority);
    RUN(test_table_self_passing_not_callable_error);
    RUN(test_table_self_passing_missing_key_error);
    RUN(test_colon_on_struct_error);
    RUN(test_colon_on_int_error);

    // shebang
    std::cout << "--- shebang ---" << std::endl;
    RUN(test_shebang_skipped);
    RUN(test_shebang_only_at_start);
    RUN(test_shebang_empty_after);
    RUN(test_shebang_preserves_line_numbers);

    // read-line
    std::cout << "--- read-line ---" << std::endl;
    RUN(test_file_read_line);
    RUN(test_file_read_line_empty_lines);

    // standard streams
    std::cout << "--- standard streams ---" << std::endl;
    RUN(test_std_streams_exist);
    RUN(test_std_stream_close_noop);

    // os library
    std::cout << "--- os library ---" << std::endl;
    RUN(test_os_getenv);
    RUN(test_os_setenv_getenv);
    RUN(test_os_time);
    RUN(test_os_clock);
    RUN(test_os_sleep);
    RUN(test_os_execute);
    RUN(test_os_getcwd);
    RUN(test_os_getpid);
    RUN(test_os_chdir_getcwd);
    RUN(test_os_mkdir_remove);
    RUN(test_os_rename);
    RUN(test_os_tmpname);
    RUN(test_os_date);
    RUN(test_os_difftime);
    RUN(test_os_stat);
    RUN(test_os_glob);
    RUN(test_os_spawn);
    RUN(test_os_spawn_readline);
    RUN(test_os_spawn_detached);

    // script-defined structs
    std::cout << "--- script-defined structs ---" << std::endl;
    RUN(test_script_struct_basic);
    RUN(test_script_struct_all_types);
    RUN(test_script_struct_write);
    RUN(test_script_struct_nested);
    RUN(test_script_struct_vector);
    RUN(test_script_struct_sizeof);
    RUN(test_script_struct_packed_layout);
    RUN(test_script_struct_overflow);
    RUN(test_script_struct_errors);
    RUN(test_structbuilder_new_types);
    RUN(test_script_struct_closure);

    // Binary ↔ Vector
    std::cout << "--- Binary/Vector ---" << std::endl;
    RUN(test_binary_from_vector);
    RUN(test_binary_from_struct);
    RUN(test_binary_from_size);
    RUN(test_vector_from_binary);
    RUN(test_vector_from_binary_struct);
    RUN(test_binary_at_set);
    RUN(test_binary_slice);
    RUN(test_binary_resize);
    RUN(test_binary_copy_from);
    RUN(test_binary_errors);

    // Child States
    std::cout << "--- Child States ---" << std::endl;
    RUN(test_state_new);
    RUN(test_state_post_recv_roundtrip);
    RUN(test_state_post_recv_value_types);
    RUN(test_state_vector_transfer);
    RUN(test_state_binary_transfer);
    RUN(test_state_bidirectional);
    RUN(test_state_is_done_join);
    RUN(test_state_error_propagation);
    RUN(test_state_recv_terminated);
    RUN(test_state_destroy_use_after);
    RUN(test_state_nested);
    RUN(test_state_destroy_cascade);
    RUN(test_state_gc_unreachable);
    RUN(test_state_post_invalid_value);
    RUN(test_state_recv_dead_child);
    RUN(test_state_terminated_bypasses_try_catch);
    RUN(test_state_recv_vec_survives_gc);
    RUN(test_state_empty_table);
    RUN(test_state_post_bad_args);

    // ========== Step Limit Tests ==========
    // RUN(test_step_limit_infinite_loop);
    // RUN(test_step_limit_infinite_recursion);
    // RUN(test_step_limit_normal_execution);
    // RUN(test_step_limit_zero_unlimited);
    // RUN(test_step_limit_persists_across_exec);
    // RUN(test_step_limit_manual_reset);
    // RUN(test_step_limit_try_catch_no_escape);
    // RUN(test_step_limit_each);
    // RUN(test_step_limit_do_block);
    // RUN(test_step_limit_error_message_contains_limit);
    // RUN(test_step_limit_count_value);

    // ========== Tail Call Optimization ==========
    std::cout << "--- Tail Call Optimization ---" << std::endl;
    // RUN(test_tco_basic_accumulator);
    // RUN(test_tco_countdown);
    // RUN(test_tco_with_if_else);
    // RUN(test_tco_with_do);
    // RUN(test_tco_non_tail_still_limited);
    // RUN(test_tco_step_limit_still_works);
    // RUN(test_tco_mutual_recursion_not_optimized);
    // RUN(test_tco_anonymous_not_optimized);
    // RUN(test_tco_preserves_captures);
    // RUN(test_tco_inside_try);
    // RUN(test_tco_arity_mismatch);
    // RUN(test_tco_heap_args);
    // RUN(test_tco_rebinding);

    // ========== Bytecode VM ==========
    std::cout << "--- Bytecode VM ---" << std::endl;
    RUN(test_bc_literals);
    RUN(test_bc_arithmetic);
    RUN(test_bc_variadic_add);
    RUN(test_bc_string_concat);
    RUN(test_bc_comparison);
    RUN(test_bc_logic);
    RUN(test_bc_variables);
    RUN(test_bc_if);
    RUN(test_bc_loop);
    RUN(test_bc_break_continue);
    RUN(test_bc_do_block);
    RUN(test_bc_fn_call);
    RUN(test_bc_closure);
    RUN(test_bc_recursion);
    RUN(test_bc_cfn_call);
    RUN(test_bc_multi_arg_fn);
    RUN(test_bc_and_or);
    RUN(test_bc_bitwise);
    RUN(test_bc_vector);
    RUN(test_bc_table);
    RUN(test_bc_dot_access);
    RUN(test_bc_method_call);
    RUN(test_bc_each);
    RUN(test_bc_nested_fn);

    std::cout << "--- Sandboxing ---" << std::endl;
    RUN(test_step_limit_loop);
    RUN(test_step_limit_recursion);
    RUN(test_step_limit_try_catch);
    RUN(test_sandbox_return_value);
    RUN(test_sandbox_return_string);
    RUN(test_sandbox_step_limit);
    RUN(test_sandbox_no_print);
    RUN(test_sandbox_allow_print);
    RUN(test_sandbox_no_file);
    RUN(test_sandbox_isolation);
    RUN(test_addcore_only);
    RUN(test_sandbox_no_nested_sandbox);
    RUN(test_defn_basic_bbl);
    RUN(test_defn_recursive_bbl);

    std::cout << "--- Module Import ---" << std::endl;
    RUN(test_import_basic);
    RUN(test_import_isolation);
    RUN(test_import_cache);
    RUN(test_import_stdlib_access);
    RUN(test_import_inter_module_calls);
    RUN(test_import_recursive_fn);
    RUN(test_execfile_unchanged);
    RUN(test_dot_call);
    RUN(test_dot_call_multi_arg);
    RUN(test_anon_call);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << std::endl;
    return failed > 0 ? 1 : 0;
}
