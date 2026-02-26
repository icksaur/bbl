# BBL Pain Points

Findings from writing 5 non-trivial programs in BBL (bblbench suite).  Each pain point includes: what hurts, how it manifests, whether weaker models will hit it, and a suggested fix with complexity estimate.

---

## Pain Point 1: `if` allows only one expression per branch

**Severity: Critical**

`(if cond then-expr else-expr)` — each branch is a single expression.  This is the single biggest usability problem in BBL.  Every non-trivial conditional requires either:

- Extracting the body into a helper function (noise, indirection)
- Using multiple sequential `if` statements with the same condition (wasteful, error-prone)

```bbl
// BROKEN — looks right but the 3rd expression is the else branch
(if (> x 10)
    (= y 1)
    (= z 2))     // ← this is the ELSE, not part of the then

// WORKAROUND 1: extract to function
(= do-thing (fn ()
    (= y 1)
    (= z 2)))
(if (> x 10) (do-thing))

// WORKAROUND 2: repeat the condition
(if (> x 10) (= y 1))
(if (> x 10) (= z 2))
```

**Will weaker models hit this?**  100%.  Every model trained on Lisp will write multi-statement if bodies.  This is the #1 failure mode I predict.

**Suggested fix: multi-expression `if`**

Change `if` to accept multiple body expressions with an explicit `else` keyword to separate branches:

```bbl
(if (> x 10)
    (= y 1)
    (= z 2)
  else
    (= y 0))
```

Or, keep current `if` syntax and add a `do` block form:

```bbl
(if (> x 10)
    (do (= y 1) (= z 2))
    (do (= y 0)))
```

**Complexity estimate:**

| Approach | Lines changed | Risk |
|----------|-------------|------|
| `do` block (new special form) | ~15 lines in evalList | Very low — just evals children in order, returns last |
| Multi-body `if` with `else` keyword | ~30 lines in If handler + parser awareness | Low — scan children for `else` symbol, split into two groups |

The `do` block is strictly better because it composes with everything (loop, fn, etc.) and requires no special parsing.

---

## Pain Point 2: No int-to-string conversion

**Severity: High**

`+` on strings requires all operands to be strings.  `(+ "score: " 42)` throws a type error.  To build a string containing a number, you must implement your own int-to-str conversion using modulo and a digit lookup table — ~15 lines of boilerplate in every program that formats output.

```bbl
// Required boilerplate for basic string formatting
(= digits (table 0 "0" 1 "1" 2 "2" 3 "3" 4 "4" 5 "5" 6 "6" 7 "7" 8 "8" 9 "9"))
(= int-to-str (fn (n)
    (= s "")
    (if (== n 0) (= s "0"))
    (if (< n 0) ...)  // handle negatives too
    (loop (> n 0)
        (= s (+ (digits.get (% n 10)) s))
        (= n (/ n 10)))
    s))
```

Note: `print` already handles mixed types internally — it calls `snprintf` for ints/floats.  The problem is only with string building via `+`.

**Will weaker models hit this?**  Yes, in any task that builds strings containing numbers (file generation, formatting).  Models will try `(+ "x=" x)` and fail.

**Suggested fix: `str` function**

Add a built-in `str` that converts any value to its string representation (same logic `print` already uses):

```bbl
(= msg (+ "score: " (str 42)))       // "score: 42"
(= label (+ "pi=" (str 3.14)))       // "pi=3.14"
```

**Complexity estimate:** ~30 lines.  Extract the per-type formatting from `bblPrint` into a shared `valueToString()` helper, register a `str` C function that calls it and pushes the result.

---

## Pain Point 3: No `for`/`range` loop

**Severity: Medium-High**

Every counted loop requires 3 lines of boilerplate:

```bbl
(= i 0)                    // init
(loop (< i 10)              // condition
    ...                     // body
    (= i (+ i 1)))          // increment
```

This is the most common pattern in every script.  The counter init and increment are ceremony that adds noise and creates off-by-one opportunities.

**Will weaker models hit this?**  Not directly — they'll adapt.  But the noise makes scripts harder to read and debug, increasing the chance of subtle errors in the increment placement.

**Suggested fix: `for` special form**

```bbl
(for i 0 10
    (print i "\n"))
```

Desugars to: bind `i` = start, loop while `i` < end, increment after body.

**Complexity estimate:** ~25 lines.  New `SpecialForm::For` case — create binding, loop with auto-increment.  Very low risk.

---

## Pain Point 4: No early return or break

**Severity: Medium**

Functions always run all body expressions and return the last one.  There's no way to return early.  Loops have no `break`.  This forces awkward flag-variable patterns:

```bbl
// Want: return early when found
// Must: loop through everything with a flag
(= found false)
(= result null)
(= i 0)
(loop (and (< i len) (not found))
    (if (== (data.at i) target)
        (= found true))
    (if (not found)
        (= i (+ i 1))))
```

**Will weaker models hit this?**  Models will try to write `return` or `break` and fail.  They'll need to restructure to flag-based flow control.

**Suggested fix: not recommended yet**

Early return requires unwinding the evaluation stack (exception or longjmp).  `break` is simpler but still needs a loop-exit mechanism.  Both add significant complexity to the interpreter.

**Complexity estimate:**

| Feature | Lines | Risk |
|---------|-------|------|
| `break` for loops | ~15 lines (throw/catch a BreakSignal) | Low |
| `return` for functions | ~25 lines (throw/catch a ReturnSignal with value) | Medium — must be caught at fn call boundary |

---

## Pain Point 5: `+` string concat requires all-string operands

**Severity: Medium**

Related to pain point 2, but distinct.  Even if you have `str`, the requirement that `+` checks types strictly means you always need explicit conversion.  Most scripting languages auto-coerce in string context.

**Will weaker models hit this?**  Yes — `(+ "hello " name " has " hp " hp")` is the natural way to write it, and it fails if `hp` is an int.

**Suggested fix: auto-coerce in `+` when left operand is string**

When the left operand of `+` is a string, auto-convert subsequent operands to strings using the same logic as `print`.

```bbl
(+ "score: " 42 " at " 3.14 "x")  // "score: 42 at 3.14x"
```

**Complexity estimate:** ~20 lines.  In the `SpecialForm::Add` string branch, replace the type-error throw with a call to `valueToString()` for non-string operands.

---

## Pain Point 6: Value-type capture semantics are confusing

**Severity: Medium**

Closures capture value types (int, float, bool) by snapshot.  GC types (table, string) are shared.  This split semantics means:

```bbl
(= count 0)
(= inc (fn () (= count (+ count 1))))
(inc) (inc) (inc)
(print count)  // 0 — not 3!  closure modified its own snapshot

// Workaround: wrap in a table
(= state (table "count" 0))
(= inc (fn () (= state.count (+ state.count 1))))
```

**Will weaker models hit this?**  Some will.  It's a subtle gotcha that only manifests when closures try to share mutable state with their defining scope.  Models may write code that "looks right" but silently produces wrong values.

**Suggested fix: no change recommended**

This is a fundamental design choice (value semantics for simple types).  Changing it would require making all values heap-allocated or adding indirection.  The table workaround is adequate.  Document it prominently in the language guide instead.

---

## Pain Point 7: No string indexing, slicing, or manipulation

**Severity: Low-Medium**

Strings only have `.length`.  No `charAt`, `substring`, `split`, `indexOf`, `replace`, `startsWith`, `trim`, etc.  Any string processing beyond concatenation is essentially impossible.

**Will weaker models hit this?**  Only if the task requires string manipulation beyond concat.  The current benchmarks mostly avoid this, but any real-world task (parsing, formatting, text processing) would be blocked.

**Suggested fix: string methods**

Add methods to strings, same dispatch mechanism as tables/vectors:

```bbl
(= s "hello world")
(s.at 0)          // "h"
(s.slice 0 5)     // "hello"
(s.find "world")  // 6
(s.split " ")     // table: {1: "hello", 2: "world"}
```

**Complexity estimate:** ~80–120 lines.  Add string method dispatch in the DotAccess evaluator (parallel to table/vector methods).  Each method is ~10 lines.

---

## Model failure predictions for bblbench

| Script | Weak model failure rate | Primary trap |
|--------|------------------------|-------------|
| 1_file_gen | 70–80% | int-to-str conversion required; multi-statement if needed for padding logic |
| 2_primes | 60–70% | Must extract sieve inner loop into helper fn due to single-statement if |
| 3_sort | 50–60% | Short-circuit `and` in loop condition; table 1-based indexing; shift-right extraction |
| 4_collatz | 30–40% | If/else is naturally single-statement here (just assignment); table-for-shared-state is the trap |
| 5_closure | 40–50% | HOF patterns are natural in Lisp; `count-if` with if-inside-reduce is tricky |

**Overall prediction:** A model that hasn't specifically been trained on BBL will fail 2–4 out of 5 tasks.  The if-single-statement constraint is the killer — it's counter to every Lisp the model has seen.

---

## Priority-ordered improvement roadmap

| Priority | Fix | Impact | Complexity |
|----------|-----|--------|-----------|
| **1** | `do` block — `(do expr1 expr2 ... exprN)` | Eliminates #1 pain, helps with everything | ~15 lines, trivial |
| **2** | `str` built-in | Eliminates #2 pain | ~30 lines, trivial |
| **3** | Auto-coerce in string `+` | Eliminates #5 pain | ~20 lines, trivial |
| **4** | `for` loop | Reduces noise in every script | ~25 lines, easy |
| **5** | `break` | Enables early loop exit | ~15 lines, easy |
| **6** | String methods | Enables string processing tasks | ~100 lines, moderate |
| **7** | `return` | Enables early function exit | ~25 lines, medium |

Items 1–3 are each under 30 lines and would make BBL dramatically more usable.  A model given BBL with those three fixes would likely pass 4–5 out of 5 benchmarks.
