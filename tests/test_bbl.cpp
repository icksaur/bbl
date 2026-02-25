#include "bbl.h"
#include <iostream>
#include <cstring>
#include <cmath>

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

TEST(test_lexer_def_string) {
    BblLexer lex("(def x \"hello\")");
    ASSERT_EQ(lex.nextToken().type, TokenType::LParen);
    auto d = lex.nextToken();
    ASSERT_EQ(d.type, TokenType::Symbol);
    ASSERT_EQ(d.stringVal, std::string("def"));
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
    BblLexer lex("(def x (+ 1 2))");
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

TEST(test_scope_def_get) {
    BblState bbl;
    bbl.exec("(def x 10)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)10);
}

TEST(test_scope_set) {
    BblState bbl;
    bbl.exec("(def x 10) (set x 20)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)20);
}

TEST(test_scope_set_undefined) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(set y 5)"));
}

TEST(test_scope_shadow) {
    BblState bbl;
    bbl.exec("(def x 1) (def x 2)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)2);
}

// ========== Arithmetic Tests ==========

TEST(test_add_int) {
    BblState bbl;
    bbl.exec("(def x (+ 1 2))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)3);
}

TEST(test_add_float) {
    BblState bbl;
    bbl.exec("(def x (+ 1.0 2.0))");
    ASSERT_NEAR(bbl.getFloat("x"), 3.0, 0.001);
}

TEST(test_add_promotion) {
    BblState bbl;
    bbl.exec("(def x (+ 1 2.0))");
    ASSERT_NEAR(bbl.getFloat("x"), 3.0, 0.001);
}

TEST(test_multiply) {
    BblState bbl;
    bbl.exec("(def x (* 3 4))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)12);
}

TEST(test_int_division) {
    BblState bbl;
    bbl.exec("(def x (/ 10 3))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)3);
}

TEST(test_float_division) {
    BblState bbl;
    bbl.exec("(def x (/ 10.0 3.0))");
    ASSERT_NEAR(bbl.getFloat("x"), 3.333, 0.01);
}

TEST(test_modulo) {
    BblState bbl;
    bbl.exec("(def x (% 10 3))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)1);
}

TEST(test_subtract) {
    BblState bbl;
    bbl.exec("(def x (- 10 3))");
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
    bbl.exec("(def x (+ (* 2 3) (/ 10 2)))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)11);
}

// ========== Comparison Tests ==========

TEST(test_eq_true) {
    BblState bbl;
    bbl.exec("(def x (== 1 1))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_eq_false) {
    BblState bbl;
    bbl.exec("(def x (== 1 2))");
    ASSERT_FALSE(bbl.getBool("x"));
}

TEST(test_lt) {
    BblState bbl;
    bbl.exec("(def x (< 1 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_gt) {
    BblState bbl;
    bbl.exec("(def x (> 2 1))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_lte) {
    BblState bbl;
    bbl.exec("(def x (<= 2 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_gte) {
    BblState bbl;
    bbl.exec("(def x (>= 2 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

TEST(test_neq) {
    BblState bbl;
    bbl.exec("(def x (!= 1 2))");
    ASSERT_TRUE(bbl.getBool("x"));
}

// ========== Logic Tests ==========

TEST(test_and_true) {
    BblState bbl;
    bbl.exec("(def b (and true true))");
    ASSERT_TRUE(bbl.getBool("b"));
}

TEST(test_and_false) {
    BblState bbl;
    bbl.exec("(def b (and true false))");
    ASSERT_FALSE(bbl.getBool("b"));
}

TEST(test_or_true) {
    BblState bbl;
    bbl.exec("(def b (or false true))");
    ASSERT_TRUE(bbl.getBool("b"));
}

TEST(test_or_false) {
    BblState bbl;
    bbl.exec("(def b (or false false))");
    ASSERT_FALSE(bbl.getBool("b"));
}

TEST(test_not_true) {
    BblState bbl;
    bbl.exec("(def b (not true))");
    ASSERT_FALSE(bbl.getBool("b"));
}

TEST(test_not_false) {
    BblState bbl;
    bbl.exec("(def b (not false))");
    ASSERT_TRUE(bbl.getBool("b"));
}

TEST(test_short_circuit_or) {
    BblState bbl;
    bbl.exec("(def x 0) (or true (set x 1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)0);
}

TEST(test_short_circuit_and) {
    BblState bbl;
    bbl.exec("(def x 0) (and false (set x 1))");
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
    bbl.exec("(def s (+ \"a\" \"b\"))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("ab"));
}

TEST(test_string_concat_variadic) {
    BblState bbl;
    bbl.exec("(def s (+ \"hello\" \" \" \"world\"))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("hello world"));
}

TEST(test_string_int_type_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(+ \"a\" 1)"));
}

// ========== If/Loop Tests ==========

TEST(test_if_then) {
    BblState bbl;
    bbl.exec("(def x 0) (if true (set x 1))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)1);
}

TEST(test_if_else) {
    BblState bbl;
    bbl.exec("(def x 0) (if false (set x 1) (set x 2))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)2);
}

TEST(test_if_statement_returns_null) {
    BblState bbl;
    bbl.exec("(def x (if true 42))");
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Null);
}

TEST(test_if_non_bool_condition) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(if 42 (def x 1))"));
}

TEST(test_loop_basic) {
    BblState bbl;
    bbl.exec("(def i 0) (loop (< i 5) (set i (+ i 1)))");
    ASSERT_EQ(bbl.getInt("i"), (int64_t)5);
}

TEST(test_loop_statement_returns_null) {
    BblState bbl;
    bbl.exec("(def x (loop false 1))");
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Null);
}

TEST(test_loop_non_bool_condition) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(loop 1 (def x 0))"));
}

// ========== Function Tests ==========

TEST(test_fn_basic) {
    BblState bbl;
    bbl.exec("(def f (fn (x) (* x 2))) (def r (f 5))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)10);
}

TEST(test_fn_multi_arg) {
    BblState bbl;
    bbl.exec("(def f (fn (x y) (+ x y))) (def r (f 3 4))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)7);
}

TEST(test_fn_zero_arg) {
    BblState bbl;
    bbl.exec("(def f (fn () 42)) (def r (f))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)42);
}

TEST(test_fn_arity_error) {
    BblState bbl;
    ASSERT_THROW(bbl.exec("(def f (fn (x) x)) (f 1 2)"));
}

TEST(test_closure_value_capture) {
    BblState bbl;
    bbl.exec("(def x 10) (def f (fn () x)) (set x 99) (def r (f))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)10);
}

TEST(test_closure_write_no_leak) {
    BblState bbl;
    bbl.exec("(def x 10) (def f (fn () (set x 20))) (f)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)10);
}

TEST(test_higher_order) {
    BblState bbl;
    bbl.exec("(def make (fn (n) (fn (x) (+ x n)))) (def add5 (make 5)) (def r (add5 3))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)8);
}

TEST(test_last_expression_return) {
    BblState bbl;
    bbl.exec("(def f (fn () 1 2 3)) (def r (f))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)3);
}

TEST(test_fn_with_def_inside) {
    BblState bbl;
    bbl.exec("(def f (fn (x) (def y (* x 2)) (+ y 1))) (def r (f 5))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)11);
}

TEST(test_fn_iterative_factorial) {
    BblState bbl;
    bbl.exec(R"(
        (def factorial (fn (n)
            (def result 1)
            (def i 1)
            (loop (<= i n)
                (set result (* result i))
                (set i (+ i 1))
            )
            result
        ))
        (def r (factorial 5))
    )");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)120);
}

// ========== Exec Tests ==========

TEST(test_exec_defines_var) {
    BblState bbl;
    bbl.exec("(def x 10)");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)10);
}

TEST(test_exec_accumulates) {
    BblState bbl;
    bbl.exec("(def x 10)");
    bbl.exec("(def y (+ x 5))");
    ASSERT_EQ(bbl.getInt("y"), (int64_t)15);
}

TEST(test_script_exec_returns_value) {
    BblState bbl;
    bbl.exec("(def r (exec \"(+ 1 2)\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)3);
}

TEST(test_script_exec_isolates_scope) {
    BblState bbl;
    bbl.exec("(def x 99) (def r (exec \"(def y 5) y\"))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)5);
    ASSERT_EQ(bbl.getInt("x"), (int64_t)99);
    ASSERT_FALSE(bbl.has("y"));
}

// ========== Introspection Tests ==========

TEST(test_has) {
    BblState bbl;
    bbl.exec("(def x 1)");
    ASSERT_TRUE(bbl.has("x"));
    ASSERT_FALSE(bbl.has("y"));
}

TEST(test_get_type) {
    BblState bbl;
    bbl.exec("(def x 1) (def s \"hi\") (def b true) (def n null)");
    ASSERT_EQ(bbl.getType("x"), BBL::Type::Int);
    ASSERT_EQ(bbl.getType("s"), BBL::Type::String);
    ASSERT_EQ(bbl.getType("b"), BBL::Type::Bool);
    ASSERT_EQ(bbl.getType("n"), BBL::Type::Null);
}

TEST(test_get_wrong_type) {
    BblState bbl;
    bbl.exec("(def x 1)");
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
    bbl.exec("(def r (myadd 10 20))");
    ASSERT_EQ(bbl.getInt("r"), (int64_t)30);
}

TEST(test_defn_args) {
    BblState bbl;
    bbl.defn("count", testGetArgCount);
    bbl.exec("(def c (count 1 2 3))");
    ASSERT_EQ(bbl.getInt("c"), (int64_t)3);
}

TEST(test_defn_return_int) {
    BblState bbl;
    bbl.defn("myadd", testAdd);
    bbl.exec("(def x (myadd 3 4))");
    ASSERT_EQ(bbl.getInt("x"), (int64_t)7);
}

static int testReturnString(BblState* bbl) {
    bbl->pushString("hello");
    return 1;
}

TEST(test_defn_return_string) {
    BblState bbl;
    bbl.defn("greet", testReturnString);
    bbl.exec("(def s (greet))");
    ASSERT_EQ(std::string(bbl.getString("s")), std::string("hello"));
}

TEST(test_defn_arg_type_check) {
    BblState bbl;
    bbl.defn("myadd", testAdd);
    bbl.exec("(def x 42)");
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
    bbl.exec("(def r (noop))");
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

// ========== Main ==========

int main() {
    std::cout << "=== bbl ===" << std::endl;

    // Lexer
    std::cout << "--- Lexer ---" << std::endl;
    RUN(test_lexer_basic);
    RUN(test_lexer_def_string);
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
    RUN(test_scope_def_get);
    RUN(test_scope_set);
    RUN(test_scope_set_undefined);
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
    RUN(test_string_int_type_error);

    // If/Loop
    std::cout << "--- If/Loop ---" << std::endl;
    RUN(test_if_then);
    RUN(test_if_else);
    RUN(test_if_statement_returns_null);
    RUN(test_if_non_bool_condition);
    RUN(test_loop_basic);
    RUN(test_loop_statement_returns_null);
    RUN(test_loop_non_bool_condition);

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

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << std::endl;
    return failed > 0 ? 1 : 0;
}
