# C Function Assignment Safety
Status: done

## Goal

Eliminate the segfault when assigning a C-registered function to a variable.  `(= s sqrt)` must work — storing a C function in a variable, passing it to other functions, comparing it, and calling it through the variable should all be safe.

## Bug

```bbl
(= s sqrt)
```

Segmentation fault (exit code 139).  Reproduces with any C function registered via `defn()`: `sqrt`, `print`, `str`, `int`, `float`, `fmt`, etc.

Calling C functions directly works fine: `(sqrt 9)` returns `3`.  The crash only occurs when assigning the function value to a variable.

## Root Cause

`BblValue` stores functions as `type == BBL::Type::Fn` with a discriminator `bool isCFn`.  The union holds either `BblFn* fnVal` (BBL closure) or `BblCFunction cfnVal` (C function pointer).  Both are pointer-sized and occupy the same union slot.

Two code paths read `fnVal` without checking `isCFn`:

### 1. Self-capture block in `=` handler (crash site)

`bbl.cpp` line 1081:

```cpp
if (val.type == BBL::Type::Fn && val.fnVal) {
    // dereferences val.fnVal->captures, ->params, ->body, etc.
}
```

For a C function, `fnVal` reads the bits of `cfnVal` — a non-null function pointer.  The condition passes.  The code then dereferences `fnVal` as a `BblFn*`, treating an arbitrary code address as a struct pointer.  Segfault.

### 2. Equality comparison

`bbl.cpp` line 74:

```cpp
case BBL::Type::Fn: return fnVal == o.fnVal;
```

For two C functions this produces a correct result by accident (same union bits), but comparing a C function to a BBL function gives a meaningless bitwise comparison.  No crash, but wrong semantics.

### Code paths that are already safe

- **GC mark** (line 593): guards with `!val.isCFn` before dereferencing `fnVal`.
- **Call dispatch** (lines 1890–1904): checks `headVal.isCFn` and calls `cfnVal` directly; the else-branch at line 1904 calls `callFn(headVal.fnVal, ...)` only when `isCFn` is false.
- **toString** (line 2526): returns `<fn>` without dereferencing.
- **typeName** (line 730): returns `"fn"` without dereferencing.

## Design

### Fix 1: Guard the self-capture block

Add `!val.isCFn` to the condition at line 1081.  C functions have no captures, params, or body — the self-capture logic does not apply.

```cpp
if (val.type == BBL::Type::Fn && !val.isCFn && val.fnVal) {
```

One-line change.  This eliminates the crash.

### Fix 2: Correct equality comparison

Replace the `Fn` case in `operator==` to handle the three comparison cases:

```cpp
case BBL::Type::Fn:
    if (isCFn != o.isCFn) return false;
    return isCFn ? (cfnVal == o.cfnVal) : (fnVal == o.fnVal);
```

Two C functions are equal if they point to the same function.  Two BBL functions are equal if they point to the same `BblFn` object.  A C function is never equal to a BBL function.

### Fix 3: Improve toString for C functions

Optional but useful for debugging.  Return `<cfn>` instead of `<fn>` for C functions so users can distinguish them.

```cpp
case BBL::Type::Fn:
    return val.isCFn ? "<cfn>" : "<fn>";
```

## Considerations

- The union layout is correct — `BblFn*` and `BblCFunction` are both pointer-sized on all target platforms.  The discriminator approach (`isCFn`) is sound; the bug is simply missing guards.
- No new fields or types are needed.  The fix is purely adding `isCFn` checks to the two code paths that lack them.
- C functions should be fully first-class: assignable, passable, comparable, callable through variables.  After the fix, `(= f sqrt) (f 9)` should return `3`.
- The self-capture block is BBL-function-specific (closures need self-references for recursion).  C functions are stateless pointers — self-capture is meaningless for them.

## Risks

None — the fix is purely additive guards on two existing code paths.  No behavioral change for BBL functions.

## Acceptance

1. `(= s sqrt) (s 9)` returns `3` — no crash.
2. `(= f print) (f "hello\n")` prints `hello` — no crash.
3. `(== sqrt sqrt)` returns `true`.
4. `(== sqrt print)` returns `false`.
5. `(== sqrt (fn (x) x))` returns `false`.
6. C function passed as argument: `(= apply (fn (f x) (f x))) (apply sqrt 16)` returns `4`.
7. Re-assignment: `(= s sqrt) (= s print) (s "hello\n")` prints `hello`.
8. C function stored in table: `(= t (table)) (set t "f" sqrt) ((get t "f") 9)` returns `3`.
9. Multiple aliases: `(= a sqrt) (= b sqrt) (== a b)` returns `true`.
10. `(str sqrt)` returns `<cfn>`.
11. All existing tests pass.
12. New tests cover all the above cases.
