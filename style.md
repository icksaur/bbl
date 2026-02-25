# C++ Style Guide

## naming

| element | convention | example |
|---------|-----------|---------|
| type / class / struct / enum | UpperCamelCase | `BblState`, `AstNode`, `TokenType` |
| variable / parameter / member | lowerCamelCase | `tokenType`, `lineNumber`, `parentScope` |
| function / method | lowerCamelCase | `nextToken()`, `getInt()`, `evalExpr()` |
| enum value | UpperCamelCase | `TokenType::LParen`, `Type::Int` |
| namespace | UpperCamelCase | `BBL` |
| constant / constexpr | lowerCamelCase | `maxArgs`, `defaultCapacity` |
| file | lowercase, no separators | `bbl.h`, `bbl.cpp` |

## formatting

K&R braces.  Opening brace on same line.  Always braces, even for single-statement bodies.

```cpp
if (x > 0) {
    doThing();
} else {
    doOtherThing();
}

for (int i = 0; i < n; i++) {
    process(i);
}

while (running) {
    step();
}
```

4-space indent.  No tabs.

## class layout

Private members first, then public.  No `private:` label needed (it's the default for `class`).

```cpp
class BblLexer {
    const char* src;
    int pos;
    int line;

    Token readString();
    Token readNumber();

public:
    BblLexer(const char* source);
    Token nextToken();
    int currentLine() const;
};
```

## comments

Almost none.  Names explain **what**.  Comments explain **why** — only when the reason isn't obvious.

```cpp
// bad
int count = 0; // initialize count to zero

// bad
// Returns the next token from the source
Token nextToken();

// good — why is non-obvious
// Intern table uses pool lifetime (freed by ~BblState) to avoid
// coupling string deallocation to the GC sweep cycle.
std::unordered_map<std::string_view, BblString*> internTable;
```

No commented-out code.  No TODO comments in committed code (use backlog.md).

## general

- Prefer `const` and `constexpr` where possible.
- Prefer value semantics; use pointers only when ownership or nullability is needed.
- Use `std::string_view` for non-owning string references.
- Use `std::variant`, `std::optional`, `std::span` where they reduce code.
- Avoid raw `new`/`delete` outside of the GC allocation path.
- One class, one purpose.
- No unnecessary getters/setters — public members are fine for POD and simple data.
