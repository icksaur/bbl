# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [code-quality.md](code-quality.md).

---

## quick-wins from bbl-pain.md

Three changes that make BBL dramatically more usable (see bbl-pain.md for rationale).  All are backward compatible.

### [*] 1. `do` block — group multiple expressions into one

**What:** New special form `(do expr1 expr2 ... exprN)` that evaluates each expression sequentially and returns the value of the last one.  Runs in the enclosing scope (like `loop`, `if`).

**Why:** Every major Lisp has a grouping form (`progn`, `begin`, `do`).  BBL's `if` only accepts one expression per branch.  `do` fixes that idiomatically:

```bbl
(if (> x 10)
    (do (= y 1) (= z 2))
    (do (= y 0) (= z 0)))
```

**Implementation (bbl.cpp):**

1. Add `Do` to `SpecialForm` enum (line ~890).
2. Add `{"do", SpecialForm::Do}` to the lookup table (line ~897).
3. Add `"do"` to the `specialForms` skip-list in `fn` capture logic (line ~1098).
4. Add handler in `evalList` switch:
   ```cpp
   case SpecialForm::Do: {
       BblValue result = BblValue::makeNull();
       for (size_t i = 1; i < node.children.size(); i++) {
           result = eval(node.children[i], scope);
       }
       return result;
   }
   ```

**Tests (tests/test_bbl.cpp):**

- `test_do_basic` — `(= x (do 1 2 3))` → x == 3
- `test_do_empty` — `(= x (do))` → x == null
- `test_do_side_effects` — `(= a 0) (do (= a 1) (= a 2))` → a == 2
- `test_do_in_if_then` — `(if true (do (= a 1) (= b 2)))` → a==1, b==2
- `test_do_in_if_else` — `(if false (= a 99) (do (= a 1) (= b 2)))` → a==1, b==2
- `test_do_nested` — `(= x (do (do 1 2) (do 3 4)))` → x == 4
- `test_do_in_fn` — `(= f (fn (x) (do (= y (* x 2)) y))) (= r (f 5))` → r == 10

**Functional test (tests/functional/do_block.bbl):**
```bbl
(= result (do (= a 10) (= b 20) (+ a b)))
(print result "\n")
(if true
    (do (print "then1\n") (print "then2\n"))
    (do (print "else1\n") (print "else2\n")))
```
Expected stdout: `30\nthen1\nthen2\n`

### [*] 2. `str` built-in — convert any value to string

**What:** `(str val)` returns the string representation of any value, using the same formatting logic as `print`.

```bbl
(= msg (+ "score: " (str 42)))       // "score: 42"
(= label (+ "pi=" (str 3.14)))       // "pi=3.14"
(= s (str true))                     // "true"
```

**Implementation (bbl.cpp):**

1. Extract per-type formatting from `bblPrint` into a shared `static std::string valueToString(const BblValue& val)` helper.
2. Refactor `bblPrint` to call `valueToString`.
3. Add `bblStr` C function — calls `valueToString`, pushes result via `pushString`.
4. Register in `addPrint`: `bbl.defn("str", bblStr);`

**Tests (tests/test_bbl.cpp):**

- `test_str_int` — `(= s (str 42))` → s == "42"
- `test_str_float` — `(= s (str 3.14))` → s == "3.14"
- `test_str_bool` — `(= s (str true))` → s == "true"
- `test_str_null` — `(= s (str null))` → s == "null"
- `test_str_concat` — `(= s (+ "val=" (str 99)))` → s == "val=99"

### [*] 3. String `+` auto-coerce non-string operands

**What:** When the left operand of `+` is a string, auto-convert remaining operands to strings instead of throwing a type error.

```bbl
(+ "x=" 42 " y=" 3.14)       // "x=42 y=3.14"
(+ "alive: " true)            // "alive: true"
```

**Implementation (bbl.cpp):**

In the `SpecialForm::Add` string branch (line ~1225), replace the type-error throw with a call to `valueToString`:
```cpp
if (right.type == BBL::Type::String) {
    result += right.stringVal->data;
} else {
    result += valueToString(right);
}
```

Depends on `valueToString` from item 2.

**Tests (tests/test_bbl.cpp):**

- `test_string_plus_int` — `(+ "val=" 42)` → "val=42"
- `test_string_plus_float` — `(+ "pi=" 3.14)` → "pi=3.14"
- `test_string_plus_bool` — `(+ "ok=" true)` → "ok=true"
- `test_string_plus_mixed` — `(+ "a" 1 " b" 2.5 " c" true)` → "a1 b2.5 ctrue"
- `test_int_plus_string_still_errors` — `(+ 1 "hello")` → throws (int + string is still an error)

### [*] 4. Update spec.md

Add `do` to the special forms table and control flow section.  Add `str` to the stdlib builtins.  Update the `+` operator description to mention auto-coerce.

### [*] 5. Update doc/bbl.md

Add `do` and `str` to the language guide with examples.
