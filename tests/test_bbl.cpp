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
    name(); \
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
    ASSERT_EQ(t.binaryData.size(), (size_t)5);
    ASSERT_EQ(t.binaryData[0], (uint8_t)'h');
    ASSERT_EQ(t.binaryData[4], (uint8_t)'o');
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

TEST(test_parse_dot_in_list) {
    BblLexer lex("(verts.push 1)");
    auto nodes = parse(lex);
    ASSERT_EQ(nodes.size(), (size_t)1);
    ASSERT_EQ(nodes[0].type, NodeType::List);
    ASSERT_EQ(nodes[0].children[0].type, NodeType::DotAccess);
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
    ASSERT_EQ(v.type, BBL::Type::Null);
}

TEST(test_value_int) {
    auto v = BblValue::makeInt(42);
    ASSERT_EQ(v.type, BBL::Type::Int);
    ASSERT_EQ(v.intVal, (int64_t)42);
}

TEST(test_value_float) {
    auto v = BblValue::makeFloat(3.14);
    ASSERT_EQ(v.type, BBL::Type::Float);
    ASSERT_NEAR(v.floatVal, 3.14, 0.001);
}

TEST(test_value_bool) {
    auto v = BblValue::makeBool(true);
    ASSERT_EQ(v.type, BBL::Type::Bool);
    ASSERT_TRUE(v.boolVal);
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
    ASSERT_EQ(bbl.getInt("x"), (int64_t)10);
}

TEST(test_scope_set) {
    BblState bbl;
    bbl.exec("(= x 10) (= x 20)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)20);
}

TEST(test_scope_assign_creates) {
    BblState bbl;
    bbl.exec("(= y 5)");
    ASSERT_EQ(bbl.getInt("y"), (int64_t)5);
}

TEST(test_scope_shadow) {
    BblState bbl;
    bbl.exec("(= x 1) (= x 2)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)2);
}

// ========== Arithmetic Tests ==========

TEST(test_add_int) {
    BblState bbl;
    bbl.exec("(= x (+ 1 2))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)3);
}

TEST(test_add_float) {
    BblState bbl;
    bbl.exec("(= x (+ 1.0 2.0))");
    ASSERT_NEAR(bbl.getFloat("x"), 3.0, 0.001);
}

TEST(test_add_promotion) {
    BblState bbl;
    bbl.exec("(= x (+ 1 2.0))");
    ASSERT_NEAR(bbl.getFloat("x"), 3.0, 0.001);
}

TEST(test_multiply) {
    BblState bbl;
    bbl.exec("(= x (* 3 4))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)12);
}

TEST(test_int_division) {
    BblState bbl;
    bbl.exec("(= x (/ 10 3))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)3);
}

TEST(test_float_division) {
    BblState bbl;
    bbl.exec("(= x (/ 10.0 3.0))");
    ASSERT_NEAR(bbl.getFloat("x"), 3.333, 0.01);
}

TEST(test_modulo) {
    BblState bbl;
    bbl.exec("(= x (% 10 3))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)1);
}

TEST(test_subtract) {
    BblState bbl;
    bbl.exec("(= x (- 10 3))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)7);
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
    ASSERT_EQ(bbl.getInt("x"), (int64_t)11);
}

// ========== Comparison Tests ==========

TEST(test_eq_true) {
    BblState bbl;
    bbl.exec("(= x (== 1 1))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_eq_false) {
    BblState bbl;
    bbl.exec("(= x (== 1 2))");
    ASSERT_FALSE(bbl.getBool("x"));
}

TEST(test_lt) {
    BblState bbl;
    bbl.exec("(= x (< 1 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_gt) {
    BblState bbl;
    bbl.exec("(= x (> 2 1))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_lte) {
    BblState bbl;
    bbl.exec("(= x (<= 2 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_gte) {
    BblState bbl;
    bbl.exec("(= x (>= 2 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_neq) {
    BblState bbl;
    bbl.exec("(= x (!= 1 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

// ========== Logic Tests ==========

TEST(test_and_true) {
    BblState bbl;
    bbl.exec("(= b (and true true))");
    ASSERT_TRUE(bbl.getBool("b"));
}

TEST(test_and_false) {
    BblState bbl;
    bbl.exec("(= b (and true false))");
    ASSERT_FALSE(bbl.getBool("b"));
}

TEST(test_or_true) {
    BblState bbl;
    bbl.exec("(= b (or false true))");
    ASSERT_TRUE(bbl.getBool("b"));
}

TEST(test_or_false) {
    BblState bbl;
    bbl.exec("(= b (or false false))");
    ASSERT_FALSE(bbl.getBool("b"));
}

TEST(test_not_true) {
    BblState bbl;
    bbl.exec("(= b (not true))");
    ASSERT_FALSE(bbl.getBool("b"));
}

TEST(test_not_false) {
    BblState bbl;
    bbl.exec("(= b (not false))");
    ASSERT_TRUE(bbl.getBool("b"));
}

TEST(test_short_circuit_or) {
    BblState bbl;
    bbl.exec("(= x 0) (or true (= x 1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)0);
}

TEST(test_short_circuit_and) {
    BblState bbl;
    bbl.exec("(= x 0) (and false (= x 1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)0);
}

TEST(test_and_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(and 1 true)"));
}

TEST(test_or_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(or 0 1)"));
}

TEST(test_not_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(not 42)"));
}

// ========== String Concat Tests ==========

TEST(test_string_concat) {
    BblState bbl;
    bbl.exec("(= s (+ \"a\" \"b\"))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("ab"));
}

TEST(test_string_concat_variadic) {
    BblState bbl;
    bbl.exec("(= s (+ \"hello\" \" \" \"world\"))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("hello world"));
}

TEST(test_string_int_auto_coerce) {
    BblState bbl;
    bbl.exec("(= s (+ \"a\" 1))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("a1"));
}

// ========== If/Loop Tests ==========

TEST(test_if_then) {
    BblState bbl;
    bbl.exec("(= x 0) (if true (= x 1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)1);
}

TEST(test_if_else) {
    BblState bbl;
    bbl.exec("(= x 0) (if false (= x 1) (= x 2))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)2);
}

TEST(test_if_statement_returns_null) {
    BblState bbl;
    bbl.exec("(= x (if true 42))");
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Null);
}

TEST(test_if_non_bool_condition) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(if 42 (= x 1))"));
}

TEST(test_loop_basic) {
    BblState bbl;
    bbl.exec("(= i 0) (loop (< i 5) (= i (+ i 1)))");
    ASSERT_EQ(bbl.getInt("i"), (int64_t)5);
}

TEST(test_loop_statement_returns_null) {
    BblState bbl;
    bbl.exec("(= x (loop false 1))");
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Null);
}

TEST(test_loop_non_bool_condition) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(loop 1 (= x 0))"));
}

// ========== Each Tests ==========

TEST(test_each_vector_basic) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(= v (vector int 10 20 30)) (each i v (print (v.at i) \" \"))");
    ASSERT_EQ(out, std::string("10 20 30 "));
}

TEST(test_each_table_basic) {
    BblState bbl;
    BBL::addPrint(bbl);
    std::string out;
    bbl.printCapture = &out;
    bbl.exec("(= t (table)) (t.push \"a\") (t.push \"b\") (t.push \"c\") (each i t (print (t.at i) \" \"))");
    ASSERT_EQ(out, std::string("a b c "));
}

TEST(test_each_empty) {
    BblState bbl;
    bbl.exec("(= v (vector int)) (each i v (= x 999))");
    ASSERT_EQ(bbl.getInt("i"), (int64_t)0);
}

TEST(test_each_index_survives) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (each i v (= x 0))");
    ASSERT_EQ(bbl.getInt("i"), (int64_t)3);
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
        "    (fns.push (fn () i)))"
        "(= r0 ((fns.at 0)))"
        "(= r1 ((fns.at 1)))"
        "(= r2 ((fns.at 2)))"
    );
    ASSERT_EQ(bbl.getInt("r0"), (int64_t)0);
    ASSERT_EQ(bbl.getInt("r1"), (int64_t)1);
    ASSERT_EQ(bbl.getInt("r2"), (int64_t)2);
}

TEST(test_each_nested) {
    BblState bbl;
    bbl.exec(
        "(= v1 (vector int 1 2))"
        "(= v2 (vector int 10 20 30))"
        "(= sum 0)"
        "(each i v1"
        "    (each j v2"
        "        (= sum (+ sum (+ (v1.at i) (v2.at j))))))"
    );
    // v1 has 2 elements (1,2), v2 has 3 (10,20,30)
    // Each pair: (1+10)+(1+20)+(1+30)+(2+10)+(2+20)+(2+30) = 11+21+31+12+22+32 = 129
    ASSERT_EQ(bbl.getInt("sum"), (int64_t)129);
}

TEST(test_each_returns_null) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2 3)) (= x (each i v 1))");
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Null);
}

TEST(test_each_inside_closure) {
    BblState bbl;
    bbl.exec(
        "(= data (vector int 1 2 3))"
        "(= f (fn () (= sum 0) (each i data (= sum (+ sum (data.at i)))) sum))"
        "(= r (f))"
    );
    ASSERT_EQ(bbl.getInt("r"), (int64_t)6);
}

// ========== Function Tests ==========

TEST(test_fn_basic) {
    BblState bbl;
    bbl.exec("(= f (fn (x) (* x 2))) (= r (f 5))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)10);
}

TEST(test_fn_multi_arg) {
    BblState bbl;
    bbl.exec("(= f (fn (x y) (+ x y))) (= r (f 3 4))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)7);
}

TEST(test_fn_zero_arg) {
    BblState bbl;
    bbl.exec("(= f (fn () 42)) (= r (f))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)42);
}

TEST(test_fn_arity_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= f (fn (x) x)) (f 1 2)"));
}

TEST(test_closure_value_capture) {
    BblState bbl;
    bbl.exec("(= x 10) (= f (fn () x)) (= x 99) (= r (f))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)10);
}

TEST(test_closure_write_no_leak) {
    BblState bbl;
    bbl.exec("(= x 10) (= f (fn () (= x 20))) (f)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)10);
}

TEST(test_higher_order) {
    BblState bbl;
    bbl.exec("(= make (fn (n) (fn (x) (+ x n)))) (= add5 (make 5)) (= r (add5 3))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)8);
}

TEST(test_last_expression_return) {
    BblState bbl;
    bbl.exec("(= f (fn () 1 2 3)) (= r (f))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)3);
}

TEST(test_fn_with_def_inside) {
    BblState bbl;
    bbl.exec("(= f (fn (x) (= y (* x 2)) (+ y 1))) (= r (f 5))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)11);
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
    ASSERT_EQ(bbl.getInt("r"), (int64_t)120);
}

// ========== Exec Tests ==========

TEST(test_exec_defines_var) {
    BblState bbl;
    bbl.exec("(= x 10)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)10);
}

TEST(test_exec_accumulates) {
    BblState bbl;
    bbl.exec("(= x 10)");
    bbl.exec("(= y (+ x 5))");
    ASSERT_EQ(bbl.getInt("y"), (int64_t)15);
}

TEST(test_script_exec_returns_value) {
    BblState bbl;
    bbl.exec("(= r (exec \"(+ 1 2)\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)3);
}

TEST(test_script_exec_isolates_scope) {
    BblState bbl;
    bbl.exec("(= x 99) (= r (exec \"(= y 5) y\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)5);
    ASSERT_EQ(bbl.getInt("x"), (int64_t)99);
    ASSERT_FALSE(bbl.has("y"));
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
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Int);
    ASSERT_EQ(bbl.getType("s"), BBL::Type::String);
    ASSERT_EQ(bbl.getType("b"), BBL::Type::Bool);
    ASSERT_EQ(bbl.getType("n"), BBL::Type::Null);
}

TEST(test_get_wrong_type) {
    BblState bbl;
    bbl.exec("(= x 1)");
    ASSERT_THROW(bbl.getString("x"));
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
    ASSERT_EQ(bbl.getInt("r"), (int64_t)30);
}

TEST(test_defn_args) {
    BblState bbl;
    bbl.defn("count", testGetArgCount);
    bbl.exec("(= c (count 1 2 3))");
    ASSERT_EQ(bbl.getInt("c"), (int64_t)3);
}

TEST(test_defn_return_int) {
    BblState bbl;
    bbl.defn("myadd", testAdd);
    bbl.exec("(= x (myadd 3 4))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)7);
}

static int testReturnString(BblState* bbl) {
    bbl->pushString("hello");
    return 1;
}

TEST(test_defn_return_string) {
    BblState bbl;
    bbl.defn("greet", testReturnString);
    bbl.exec("(= s (greet))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("hello"));
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
    ASSERT_EQ(bbl.getType("r"), BBL::Type::Null);
}

// ========== Phase 2: Setters ==========

TEST(test_setInt) {
    BblState bbl;
    bbl.setInt("x", 99);
    ASSERT_EQ(bbl.getInt("x"), (int64_t)99);
}

TEST(test_setFloat) {
    BblState bbl;
    bbl.setFloat("f", 2.5);
    ASSERT_NEAR(bbl.getFloat("f"), 2.5, 0.001);
}

TEST(test_setString) {
    BblState bbl;
    bbl.setString("s", "hello");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("hello"));
}

TEST(test_set_value) {
    BblState bbl;
    bbl.set("v", BblValue::makeBool(true));
    ASSERT_TRUE(bbl.getBool("v"));
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
    ASSERT_EQ(bbl.getType("v"), BBL::Type::Struct);
}

TEST(test_struct_field_read) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vertex 1.0 2.0 3.0)) (= rx v.x) (= ry v.y) (= rz v.z)");
    ASSERT_NEAR(bbl.getFloat("rx"), 1.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("ry"), 2.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("rz"), 3.0, 0.001);
}

TEST(test_struct_field_write) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vertex 1.0 2.0 3.0)) (= v.x 5.0) (= rx v.x)");
    ASSERT_NEAR(bbl.getFloat("rx"), 5.0, 0.001);
}

TEST(test_struct_copy_semantics) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= a (vertex 1.0 2.0 3.0)) (= b a) (= b.x 99.0) (= ax a.x) (= bx b.x)");
    ASSERT_NEAR(bbl.getFloat("ax"), 1.0, 0.001);
    ASSERT_NEAR(bbl.getFloat("bx"), 99.0, 0.001);
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
    ASSERT_NEAR(bbl.getFloat("ax"), 0.0, 0.001);
    bbl.exec("(= by tri.b.y)");
    ASSERT_NEAR(bbl.getFloat("by"), 0.0, 0.001);
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
    ASSERT_EQ(bbl.getType("v"), BBL::Type::Vector);
}

TEST(test_vector_length) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2 3)) (= n (v.length))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)3);
}

TEST(test_vector_at) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= x (v.at 1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)20);
}

TEST(test_vector_push) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2)) (v.push 3) (= n (v.length))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)3);
}

TEST(test_vector_pop) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= last (v.pop)) (= n (v.length))");
    ASSERT_EQ(bbl.getInt("last"), (int64_t)30);
    ASSERT_EQ(bbl.getInt("n"), (int64_t)2);
}

TEST(test_vector_clear) {
    BblState bbl;
    bbl.exec("(= v (vector int 1 2 3)) (v.clear) (= n (v.length))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)0);
}

TEST(test_vector_struct) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= verts (vector vertex (vertex 0 1 0) (vertex 1 0 0)))");
    bbl.exec("(= x (verts.at 0).x)");
    ASSERT_NEAR(bbl.getFloat("x"), 0.0, 0.001);
}

TEST(test_vector_push_struct) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= verts (vector vertex)) (verts.push (vertex 3 4 5)) (= n (verts.length))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)1);
}

TEST(test_vector_type_mismatch) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1 2)) (v.push \"bad\")"));
}

TEST(test_vector_out_of_bounds) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1)) (v.at 5)"));
}

TEST(test_vector_pop_empty) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int)) (v.pop)"));
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
    bbl.exec("(= v (vector int 10 20 30)) (v.set 1 99) (= r (v.at 1))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)99);
}

TEST(test_vector_set_struct) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vector vertex (vertex 1 2 3) (vertex 4 5 6)))"
             "(v.set 0 (vertex 7 8 9))"
             "(= r (v.at 0).x)");
    ASSERT_NEAR(bbl.getFloat("r"), 7.0f, 0.001f);
}

TEST(test_vector_set_out_of_bounds) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1 2)) (v.set 5 99)"));
}

TEST(test_vector_set_preserves_others) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (v.set 1 99)"
             "(= a (v.at 0)) (= b (v.at 2))");
    ASSERT_EQ(bbl.getInt("a"), (int64_t)10);
    ASSERT_EQ(bbl.getInt("b"), (int64_t)30);
}

// ========== Integer Dot Syntax ==========

TEST(test_int_dot_vector_read) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= r v.1)");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)20);
}

TEST(test_int_dot_vector_write) {
    BblState bbl;
    bbl.exec("(= v (vector int 10 20 30)) (= v.1 99) (= r v.1)");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)99);
}

TEST(test_int_dot_vector_struct_chain) {
    BblState bbl;
    addVertex(bbl);
    bbl.exec("(= v (vector vertex (vertex 1 2 3))) (= r v.0.x)");
    ASSERT_NEAR(bbl.getFloat("r"), 1.0f, 0.001f);
}

TEST(test_int_dot_vector_out_of_bounds) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(= v (vector int 1)) v.5"));
}

TEST(test_int_dot_table_read) {
    BblState bbl;
    bbl.exec(R"((= t (table)) (t.push "hello") (= r t.0))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("hello"));
}

TEST(test_int_dot_table_write) {
    BblState bbl;
    bbl.exec(R"((= t (table)) (t.push "hello") (= t.0 "world") (= r t.0))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("world"));
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
    bbl.exec("(= s \"hello\") (= n (s.length))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)5);
}

// ========== Phase 4: Tables ==========

TEST(test_table_construct_string_keys) {
    BblState bbl;
    bbl.exec(R"((= t (table "name" "hero" "hp" 100)))");
    ASSERT_EQ(bbl.getType("t"), BBL::Type::Table);
    auto* tbl = bbl.getTable("t");
    ASSERT_EQ(tbl->length(), (size_t)2);
}

TEST(test_table_dot_read_string_key) {
    BblState bbl;
    bbl.exec(R"((= t (table "name" "hero" "hp" 100)) (= n t.name) (= h t.hp))");
    ASSERT_EQ(std::string(bbl.getString("n")), std::string("hero"));
    ASSERT_EQ(bbl.getInt("h"), (int64_t)100);
}

TEST(test_table_integer_indexed) {
    BblState bbl;
    bbl.exec(R"((= t (table 1 "sword" 2 "shield" 3 "potion")) (= v (t.at 0)))");
    ASSERT_EQ(std::string(bbl.getString("v")), std::string("sword"));
}

TEST(test_table_get_set) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1))
        (t.set "b" 2)
        (= a (t.get "a"))
        (= b (t.get "b"))
    )");
    ASSERT_EQ(bbl.getInt("a"), (int64_t)1);
    ASSERT_EQ(bbl.getInt("b"), (int64_t)2);
}

TEST(test_table_delete) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1 "b" 2))
        (t.delete "a")
        (= len (t.length))
    )");
    ASSERT_EQ(bbl.getInt("len"), (int64_t)1);
}

TEST(test_table_has) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1))
        (= yes (t.has "a"))
        (= no (t.has "b"))
    )");
    ASSERT_TRUE(bbl.getBool("yes"));
    ASSERT_FALSE(bbl.getBool("no"));
}

TEST(test_table_keys) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "x" 1 "y" 2))
        (= ks (t.keys))
        (= n (ks.length))
    )");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)2);
}

TEST(test_table_length) {
    BblState bbl;
    bbl.exec(R"((= t (table "a" 1 "b" 2 "c" 3)) (= n (t.length)))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)3);
}

TEST(test_table_push_pop) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table))
        (t.push "first")
        (t.push "second")
        (= len (t.length))
        (= val (t.pop))
    )");
    ASSERT_EQ(bbl.getInt("len"), (int64_t)2);
    ASSERT_EQ(std::string(bbl.getString("val")), std::string("second"));
}

TEST(test_table_method_first_resolution) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "length" 42))
        (= mlen (t.length))
        (= klen (t.get "length"))
    )");
    ASSERT_EQ(bbl.getInt("mlen"), (int64_t)1);
    ASSERT_EQ(bbl.getInt("klen"), (int64_t)42);
}

TEST(test_table_get_cpp) {
    BblState bbl;
    bbl.exec(R"((= t (table "name" "hero" "hp" 100)))");
    BblTable* tbl = bbl.getTable("t");
    BblValue nameKey = BblValue::makeString(bbl.intern("name"));
    BblValue nameVal = tbl->get(nameKey);
    ASSERT_EQ(nameVal.type, BBL::Type::String);
    ASSERT_EQ(nameVal.stringVal->data, std::string("hero"));
    ASSERT_EQ(tbl->length(), (size_t)2);
    ASSERT_TRUE(tbl->has(nameKey));
}

TEST(test_table_empty) {
    BblState bbl;
    bbl.exec(R"((= t (table)) (= n (t.length)))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)0);
}

TEST(test_table_get_missing) {
    BblState bbl;
    bbl.exec(R"((= t (table "a" 1)) (= v (t.get "missing")))");
    ASSERT_EQ(bbl.getType("v"), BBL::Type::Null);
}

TEST(test_table_delete_missing) {
    BblState bbl;
    bbl.exec(R"((= t (table "a" 1)) (t.delete "missing") (= n (t.length)))");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)1);
}

TEST(test_table_pop_no_int_keys) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((= t (table "a" 1)) (t.pop))"));
}

TEST(test_table_closure_shared_capture) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t (table))
        (= f (fn () (t.push 1)))
        (f)
        (= n (t.length))
    )");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)1);
}

TEST(test_table_place_expression) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "hp" 100))
        (= t.hp 80)
        (= v t.hp)
    )");
    ASSERT_EQ(bbl.getInt("v"), (int64_t)80);
}

// ========== Phase 4: String comparison ==========

TEST(test_string_eq_interned) {
    BblState bbl;
    bbl.exec(R"((= r (== "hello" "hello")))");
    ASSERT_TRUE(bbl.getBool("r"));
}

TEST(test_string_neq) {
    BblState bbl;
    bbl.exec(R"((= r (== "a" "b")))");
    ASSERT_FALSE(bbl.getBool("r"));
}

TEST(test_string_lt_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((< "a" "b"))"));
}

TEST(test_string_gt_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec(R"((> "a" "b"))"));
}

TEST(test_string_concat_multi) {
    BblState bbl;
    bbl.exec(R"((= s (+ "a" "b" "c")))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("abc"));
}

// ========== Phase 5: Binary C++ API ==========

TEST(test_get_binary) {
    BblState bbl;
    bbl.exec("(= b 0b3:abc)");
    auto* b = bbl.getBinary("b");
    ASSERT_EQ(b->length(), (size_t)3);
    ASSERT_EQ(b->data[0], (uint8_t)'a');
}

TEST(test_set_binary) {
    BblState bbl;
    uint8_t data[] = {1, 2, 3, 4};
    bbl.setBinary("b", data, 4);
    auto* b = bbl.getBinary("b");
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
    ASSERT_EQ(bbl.getInt("r"), (int64_t)6);
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
    ASSERT_EQ(bbl.getInt("i"), (int64_t)200);
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
    BblValue last = bbl.get("last");
    ASSERT_EQ(last.type, BBL::Type::Table);
    // The table from the last iteration should have "val" = 49
    BblValue key = BblValue::makeString(bbl.intern("val"));
    BblValue val = last.tableVal->get(key);
    ASSERT_EQ(val.type, BBL::Type::Int);
    ASSERT_EQ(val.intVal, (int64_t)49);
}

// ========== Phase 5: TypeBuilder & Userdata ==========

static int counterValue = 0;
static bool counterDestructed = false;

static int udGetVal(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    int* ptr = static_cast<int*>(self.userdataVal->data);
    bbl->pushInt(*ptr);
    return 0;
}

static int udIncrement(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    int* ptr = static_cast<int*>(self.userdataVal->data);
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
    bbl.exec(R"((= v (c.value)))");
    ASSERT_EQ(bbl.getInt("v"), (int64_t)42);
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
    bbl.exec(R"((c.increment) (= v (c.value)))");
    ASSERT_EQ(bbl.getInt("v"), (int64_t)11);
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
    ASSERT_THROW(bbl.exec(R"((c.nonexistent))"));
}

// ========== Phase 5: Math ==========

TEST(test_math_sqrt) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (sqrt 4.0))");
    ASSERT_NEAR(bbl.getFloat("r"), 2.0, 0.001);
}

TEST(test_math_sin) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (sin 0.0))");
    ASSERT_NEAR(bbl.getFloat("r"), 0.0, 0.001);
}

TEST(test_math_abs_int_promoted) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (abs -5))");
    ASSERT_NEAR(bbl.getFloat("r"), 5.0, 0.001);
}

TEST(test_math_sqrt_negative) {
    BblState bbl;
    BBL::addMath(bbl);
    ASSERT_THROW(bbl.exec("(sqrt -1.0)"));
}

TEST(test_math_pi_e) {
    BblState bbl;
    BBL::addMath(bbl);
    ASSERT_NEAR(bbl.getFloat("pi"), 3.14159, 0.001);
    ASSERT_NEAR(bbl.getFloat("e"), 2.71828, 0.001);
}

TEST(test_math_pow) {
    BblState bbl;
    BBL::addMath(bbl);
    bbl.exec("(= r (pow 2.0 10.0))");
    ASSERT_NEAR(bbl.getFloat("r"), 1024.0, 0.001);
}

// ========== Phase 5: File I/O ==========

TEST(test_file_write_read) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fopen "/tmp/bbl_test_io.txt" "w"))
        (f.write "hello bbl")
        (f.close)
        (= f2 (fopen "/tmp/bbl_test_io.txt" "r"))
        (= contents (f2.read))
        (f2.close)
    )");
    ASSERT_EQ(std::string(bbl.getString("contents")), std::string("hello bbl"));
}

TEST(test_file_read_bytes) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= f (fopen "/tmp/bbl_test_io2.txt" "w"))
        (f.write "abcde")
        (f.close)
        (= f2 (fopen "/tmp/bbl_test_io2.txt" "rb"))
        (= b (f2.read-bytes 3))
        (f2.close)
        (= n (b.length))
    )");
    ASSERT_EQ(bbl.getInt("n"), (int64_t)3);
}

TEST(test_filebytes_sandbox_absolute) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"((filebytes "/etc/passwd"))"));
}

TEST(test_filebytes_sandbox_parent) {
    BblState bbl;
    BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec(R"((filebytes "../etc/passwd"))"));
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
    ASSERT_EQ(v.type, BBL::Type::Int);
    ASSERT_EQ(v.intVal, (int64_t)3);
}

TEST(test_execExpr_null_for_def) {
    BblState bbl;
    BblValue v = bbl.execExpr("(= x 5)");
    // def returns null
    ASSERT_TRUE(true); // just don't crash
    ASSERT_EQ(bbl.getInt("x"), (int64_t)5);
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
    ASSERT_EQ(bbl.getBool("loaded"), true);
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
    // helper.bbl tries (execfile "../main.bbl") -> should fail
    fs::create_directories("/tmp/bbl_sandbox/subdir");
    {
        std::ofstream f("/tmp/bbl_sandbox/subdir/helper.bbl");
        f << R"((execfile "../main.bbl"))";
    }
    bbl.currentFile = "/tmp/bbl_sandbox/main.bbl";
    bbl.scriptDir = "/tmp/bbl_sandbox";
    ASSERT_THROW(bbl.exec(R"((execfile "subdir/helper.bbl"))"));
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
    ASSERT_EQ(bbl.getInt("r"), 3628800LL);
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
    ASSERT_EQ(bbl.getInt("r"), 55LL);
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
    ASSERT_EQ(bbl.getInt("r"), 0LL);
}

TEST(test_recursive_self_capture_only) {
    // Ensure that a non-recursive fn doesn't get a self-capture injected
    BblState bbl;
    bbl.exec(R"(
        (= add1 (fn (x) (+ x 1)))
        (= r (add1 9))
    )");
    ASSERT_EQ(bbl.getInt("r"), 10LL);
}

// ========== = (assign-or-create) ==========

TEST(test_eq_create_new) {
    BblState bbl;
    bbl.exec(R"((= x 42))");
    ASSERT_EQ(bbl.getInt("x"), 42LL);
}

TEST(test_eq_rebind_existing) {
    BblState bbl;
    bbl.exec(R"(
        (= x 10)
        (= x 20)
    )");
    ASSERT_EQ(bbl.getInt("x"), 20LL);
}

TEST(test_eq_place_table) {
    BblState bbl;
    bbl.exec(R"(
        (= t (table "a" 1 "b" 2))
        (= t.a 99)
        (= r t.a)
    )");
    ASSERT_EQ(bbl.getInt("r"), 99LL);
}

TEST(test_eq_closure_capture) {
    BblState bbl;
    bbl.exec(R"(
        (= x 10)
        (= f (fn () x))
        (= x 99)
        (= r (f))
    )");
    ASSERT_EQ(bbl.getInt("r"), 10LL);
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
    ASSERT_EQ(bbl.getInt("r"), 20LL);
    // Outer x unchanged
    ASSERT_EQ(bbl.getInt("x"), 10LL);
}

TEST(test_eq_higher_order) {
    BblState bbl;
    bbl.exec(R"(
        (= make-adder (fn (n) (fn (x) (+ x n))))
        (= add5 (make-adder 5))
        (= r (add5 3))
    )");
    ASSERT_EQ(bbl.getInt("r"), 8LL);
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
    ASSERT_EQ(bbl.getInt("r"), 3628800LL);
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
    ASSERT_EQ(bbl.getInt("r"), 42LL);
}

// ========== do block tests ==========

TEST(test_do_basic) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= x (do 1 2 3))");
    ASSERT_EQ(bbl.getInt("x"), 3LL);
}
TEST(test_do_empty) {
    BblState bbl;
    bbl.exec("(= x (do))");
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Null);
}
TEST(test_do_side_effects) {
    BblState bbl;
    bbl.exec("(= a 0) (do (= a 1) (= a 2))");
    ASSERT_EQ(bbl.getInt("a"), 2LL);
}
TEST(test_do_in_if_then) {
    BblState bbl;
    bbl.exec("(= a 0) (= b 0) (if true (do (= a 1) (= b 2)))");
    ASSERT_EQ(bbl.getInt("a"), 1LL);
    ASSERT_EQ(bbl.getInt("b"), 2LL);
}
TEST(test_do_in_if_else) {
    BblState bbl;
    bbl.exec("(= a 0) (= b 0) (if false (= a 99) (do (= a 1) (= b 2)))");
    ASSERT_EQ(bbl.getInt("a"), 1LL);
    ASSERT_EQ(bbl.getInt("b"), 2LL);
}
TEST(test_do_nested) {
    BblState bbl;
    bbl.exec("(= x (do (do 1 2) (do 3 4)))");
    ASSERT_EQ(bbl.getInt("x"), 4LL);
}
TEST(test_do_in_fn) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.exec("(= f (fn (x) (do (= y (* x 2)) y))) (= r (f 5))");
    ASSERT_EQ(bbl.getInt("r"), 10LL);
}

// ========== str built-in tests ==========

TEST(test_str_int) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str 42))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("42"));
}
TEST(test_str_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str 3.14))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("3.14"));
}
TEST(test_str_bool) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str true))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("true"));
}
TEST(test_str_null) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str null))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("null"));
}
TEST(test_str_concat) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"val=\" (str 99)))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("val=99"));
}

// ========== string + auto-coerce tests ==========

TEST(test_string_plus_int) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"val=\" 42))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("val=42"));
}
TEST(test_string_plus_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"pi=\" 3.14))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("pi=3.14"));
}
TEST(test_string_plus_bool) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"ok=\" true))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("ok=true"));
}
TEST(test_string_plus_mixed) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (+ \"a\" 1 \" b\" 2.5 \" c\" true))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("a1 b2.5 ctrue"));
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
    size_t stringCount = bbl.allocatedStrings.size();
    // Allow generous headroom (literals, stdlib strings, etc.), but far less than 1000
    ASSERT_TRUE(stringCount < 500);
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
    BblValue r = bbl.get("result");
    ASSERT_EQ(r.type, BBL::Type::Bool);
    ASSERT_TRUE(r.boolVal);
}

// ========== int/float builtins ==========

TEST(test_int_from_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int \"42\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)42);
}

TEST(test_int_negative_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int \"-7\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)-7);
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
    ASSERT_EQ(bbl.getInt("r"), (int64_t)42);
}

TEST(test_int_truncate_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int 3.9))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)3);
}

TEST(test_int_truncate_negative_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (int -2.7))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)-2);
}

TEST(test_float_from_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (float \"3.14\"))");
    ASSERT_NEAR(bbl.getFloat("r"), 3.14, 0.001);
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
    ASSERT_NEAR(bbl.getFloat("r"), 42.0, 0.001);
}

// ========== String methods ==========

// at
TEST(test_string_at) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\".at 0))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("h"));
}

TEST(test_string_at_oob) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(\"hello\".at 5)"));
}

// slice
TEST(test_string_slice) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\".slice 0 5))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("hello"));
}

TEST(test_string_slice_to_end) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\".slice 6))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("world"));
}

// find
TEST(test_string_find) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\".find \"world\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)6);
}

TEST(test_string_find_not_found) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\".find \"xyz\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)-1);
}

TEST(test_string_find_with_start) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\".find \"l\" 3))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)3);
}

// contains
TEST(test_string_contains) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello world\".contains \"world\"))");
    ASSERT_TRUE(bbl.getBool("r"));
}

// starts-with
TEST(test_string_starts_with) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\".starts-with \"hel\"))");
    ASSERT_TRUE(bbl.getBool("r"));
}

// ends-with
TEST(test_string_ends_with) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\".ends-with \"llo\"))");
    ASSERT_TRUE(bbl.getBool("r"));
}

// split
TEST(test_string_split) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t ("a,b,c".split ","))
        (= r0 (t.get 0))
        (= r1 (t.get 1))
        (= r2 (t.get 2))
    )");
    ASSERT_EQ(std::string(bbl.getString("r0")), std::string("a"));
    ASSERT_EQ(std::string(bbl.getString("r1")), std::string("b"));
    ASSERT_EQ(std::string(bbl.getString("r2")), std::string("c"));
}

TEST(test_string_split_empty_sep) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(\"abc\".split \"\")"));
}

// join
TEST(test_string_join) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t (table))
        (t.push "x")
        (t.push "y")
        (= r (",".join t))
    )");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("x,y"));
}

// replace
TEST(test_string_replace) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"aXbXc\".replace \"X\" \"-\"))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("a-b-c"));
}

TEST(test_string_replace_empty_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(\"abc\".replace \"\" \"x\")"));
}

// trim
TEST(test_string_trim) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"  hi  \".trim))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("hi"));
}

TEST(test_string_trim_left) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"  hi  \".trim-left))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("hi  "));
}

TEST(test_string_trim_right) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"  hi  \".trim-right))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("  hi"));
}

// upper/lower
TEST(test_string_upper) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r \"hello\".upper)");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("HELLO"));
}

TEST(test_string_lower) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r \"HELLO\".lower)");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("hello"));
}

// upper/lower call form
TEST(test_string_upper_call) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"hello\".upper))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("HELLO"));
}

TEST(test_string_lower_call) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (\"HELLO\".lower))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("hello"));
}

// pad-left
TEST(test_string_pad_left) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r ((str 42).pad-left 6))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("    42"));
}

TEST(test_string_pad_left_fill) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r ((str 42).pad-left 6 \"0\"))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("000042"));
}

// pad-right
TEST(test_string_pad_right) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r ((str 42).pad-right 6))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("42    "));
}

// --- fmt ---

TEST(test_fmt_basic) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"{} + {} = {}\" 1 2 3))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("1 + 2 = 3"));
}

TEST(test_fmt_escaped_braces) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"use {{}} for placeholders\"))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("use {} for placeholders"));
}

TEST(test_fmt_single_arg) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"{}\" 42))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("42"));
}

TEST(test_fmt_no_placeholders) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (fmt \"no args\"))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("no args"));
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
    ASSERT_EQ(bbl.getInt("r"), 3LL);
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
    ASSERT_EQ(bbl.getInt("r"), 4LL);
}

TEST(test_cfn_equality_same) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (== sqrt sqrt))");
    ASSERT_EQ(bbl.getBool("r"), true);
}

TEST(test_cfn_equality_different) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (== sqrt print))");
    ASSERT_EQ(bbl.getBool("r"), false);
}

TEST(test_cfn_inequality_vs_bbl_fn) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (== sqrt (fn (x) x)))");
    ASSERT_EQ(bbl.getBool("r"), false);
}

TEST(test_cfn_tostring) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s (str sqrt))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("<cfn>"));
}

TEST(test_cfn_reassignment) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= s sqrt) (= s abs) (= r (int (s -7)))");
    ASSERT_EQ(bbl.getInt("r"), 7LL);
}

TEST(test_cfn_in_table) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec(R"(
        (= t (table "f" sqrt))
        (= g t.f)
        (= r (int (g 9)))
    )");
    ASSERT_EQ(bbl.getInt("r"), 3LL);
}

TEST(test_cfn_multiple_aliases) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= a sqrt) (= b sqrt) (= r (== a b))");
    ASSERT_EQ(bbl.getBool("r"), true);
}

// ========== typeof tests ==========

TEST(test_typeof_int) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof 42))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("int"));
}

TEST(test_typeof_float) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof 3.14))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("float"));
}

TEST(test_typeof_string) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof \"hello\"))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("string"));
}

TEST(test_typeof_bool) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof true))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("bool"));
}

TEST(test_typeof_null) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof null))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("null"));
}

TEST(test_typeof_cfn) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof sqrt))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("fn"));
}

TEST(test_typeof_bbl_fn) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof (fn (x) x)))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("fn"));
}

TEST(test_typeof_table) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof (table)))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("table"));
}

TEST(test_typeof_vector) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof (vector int)))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("vector"));
}

TEST(test_typeof_binary) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof 0b4:test))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("binary"));
}

TEST(test_typeof_expression) {
    BblState bbl; BBL::addStdLib(bbl);
    bbl.exec("(= r (typeof (+ 1 2)))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("int"));
}

TEST(test_typeof_arity_error) {
    BblState bbl; BBL::addStdLib(bbl);
    ASSERT_THROW(bbl.exec("(typeof)"));
    ASSERT_THROW(bbl.exec("(typeof 1 2)"));
}

TEST(test_typeof_struct) {
    BblState bbl; BBL::addStdLib(bbl);
    addVertex(bbl);
    bbl.exec("(= r (typeof (vertex 1.0 2.0 3.0)))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("struct"));
}

TEST(test_typeof_userdata) {
    BblState bbl; BBL::addStdLib(bbl);
    BBL::TypeBuilder tb("Counter");
    tb.method("get", [](BblState*) -> int { return 0; });
    bbl.registerType(tb);
    int val = 0;
    auto* ud = bbl.allocUserData("Counter", &val);
    bbl.set("c", BblValue::makeUserData(ud));
    bbl.exec("(= r (typeof c))");
    ASSERT_EQ(std::string(bbl.getString("r")), std::string("userdata"));
}

// ---------- bitwise ----------

TEST(test_band_basic) {
    BblState bbl;
    bbl.exec("(= x (band 255 15))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)15);
}

TEST(test_bor_basic) {
    BblState bbl;
    bbl.exec("(= x (bor 15 240))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)255);
}

TEST(test_bxor_basic) {
    BblState bbl;
    bbl.exec("(= x (bxor 255 15))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)240);
}

TEST(test_bnot_zero) {
    BblState bbl;
    bbl.exec("(= x (bnot 0))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)-1);
}

TEST(test_bnot_neg1) {
    BblState bbl;
    bbl.exec("(= x (bnot -1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)0);
}

TEST(test_shl_basic) {
    BblState bbl;
    bbl.exec("(= x (shl 1 8))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)256);
}

TEST(test_shr_basic) {
    BblState bbl;
    bbl.exec("(= x (shr 256 4))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)16);
}

TEST(test_band_variadic) {
    BblState bbl;
    bbl.exec("(= x (band 7 6 5))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)4);
}

TEST(test_bor_variadic) {
    BblState bbl;
    bbl.exec("(= x (bor 1 2 4))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)7);
}

TEST(test_bxor_variadic) {
    BblState bbl;
    bbl.exec("(= x (bxor 15 9 6))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)0);
}

TEST(test_shr_arithmetic) {
    BblState bbl;
    bbl.exec("(= x (shr -1 1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)-1);
}

TEST(test_shr_large_neg) {
    BblState bbl;
    bbl.exec("(= x (shr -1 64))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)-1);
}

TEST(test_shl_large) {
    BblState bbl;
    bbl.exec("(= x (shl 1 64))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)0);
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
    BblState bbl;
    ASSERT_THROW(bbl.exec("(bnot 1 2)"));
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
    ASSERT_THROW(bbl.exec("(filebytes \"/tmp/nonexistent.bin\")"));
}

TEST(test_filebytes_dotdot_blocked) {
    BblState bbl; BBL::addFileIo(bbl);
    ASSERT_THROW(bbl.exec("(filebytes \"../escape.bin\")"));
}

TEST(test_execfile_abs_open) {
    namespace fs = std::filesystem;
    std::string path = "/tmp/bbl_test_open_fs.bbl";
    { std::ofstream f(path); f << "(= openfs_result 42)"; }
    BblState bbl;
    bbl.allowOpenFilesystem = true;
    bbl.execfile(path);
    ASSERT_EQ(bbl.getInt("openfs_result"), (int64_t)42);
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
    bbl.exec("(execfile \"../target.bbl\")");
    ASSERT_EQ(bbl.getInt("dotdot_result"), (int64_t)99);
    fs::remove_all("/tmp/bbl_test_openfs");
}

TEST(test_filebytes_abs_open) {
    namespace fs = std::filesystem;
    std::string path = "/tmp/bbl_test_open_fb.bin";
    { std::ofstream f(path, std::ios::binary); f << "hello"; }
    BblState bbl; BBL::addFileIo(bbl);
    bbl.allowOpenFilesystem = true;
    bbl.exec("(= data (filebytes \"/tmp/bbl_test_open_fb.bin\"))");
    ASSERT_EQ(bbl.getType("data"), BBL::Type::Binary);
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
    bbl.exec("(= data (filebytes \"../target.bin\"))");
    ASSERT_EQ(bbl.getType("data"), BBL::Type::Binary);
    fs::remove_all("/tmp/bbl_test_openfs2");
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
    RUN(test_parse_dot_in_list);
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
    RUN(test_if_statement_returns_null);
    RUN(test_if_non_bool_condition);
    RUN(test_loop_basic);
    RUN(test_loop_statement_returns_null);
    RUN(test_loop_non_bool_condition);

    // Each
    std::cout << "--- Each ---" << std::endl;
    RUN(test_each_vector_basic);
    RUN(test_each_table_basic);
    RUN(test_each_empty);
    RUN(test_each_index_survives);
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
    RUN(test_vector_get_data_cpp);
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
    RUN(test_string_lt_type_error);
    RUN(test_string_gt_type_error);
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
    RUN(test_string_upper);
    RUN(test_string_lower);
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

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << std::endl;
    return failed > 0 ? 1 : 0;
}
