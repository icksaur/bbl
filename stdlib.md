# BBL Standard Library

All functions are available after `BBL::addStdLib(bbl)` from C++ or when
running scripts via `bbl script.bbl`.

---

## Output

```bbl
(print "hello" " " "world")     // prints to stdout, no newline
(print "line\n")                 // \n \t \r \0 \\ \" supported
```

Print output can be captured from C++ via `bbl.printCapture = &string`.

### String Conversion

```bbl
(str 42)           // "42"
(str 3.14)         // "3.14"
(str true)         // "true"
```

`str` converts a single value to its string representation.
Use `+` for concatenation: `(+ "hello " (str 42))`.

---

## Math

Constants: `pi` (3.14159...), `e` (2.71828...)

```bbl
(sin x) (cos x) (tan x)
(asin x) (acos x) (atan x) (atan2 y x)
(sqrt x) (abs x)
(floor x) (ceil x)
(min a b) (max a b)
(pow base exp) (log x) (log2 x) (log10 x) (exp x)
```

### Random

```bbl
(random)                // float in [0, 1)
(random-int 1 6)        // int in [1, 6]
(random-seed 42)        // set seed for reproducibility
```

---

## String Methods

```bbl
(= s "hello world")
(s:length)                     // 11
(s:at 0)                       // "h"
(s:slice 0 5)                  // "hello"
(s:find "world")               // 6
(s:contains "world")           // true
(s:starts-with "hello")        // true
(s:ends-with "world")          // true
(s:upper)                      // "HELLO WORLD"
(s:lower)                      // "hello world"
(s:trim)                       // strip whitespace
(s:trim-left)                  // strip leading whitespace
(s:trim-right)                 // strip trailing whitespace
(s:replace "world" "bbl")      // "hello bbl"
(s:split " ")                  // table {0:"hello", 1:"world"}
(s:join items)                 // join table values with s as separator
// e.g. (",":join (table 0 "a" 1 "b"))  → "a,b"
(s:pad-left 15)                // pad with spaces to width 15
(s:pad-right 15 ".")           // pad with dots
```

---

## Table Methods

```bbl
(= t (table "name" "alice" "age" 30))
(t:get "name")                // "alice"
(t:set "name" "bob")
(t:has "name")                // true
(t:delete "age")
(t:length)                    // number of entries
(t:keys)                      // table of keys in insertion order
(t:push "item")               // append with auto-incrementing int key
(t:pop)                       // remove and return last entry
```

---

## Vector Methods

```bbl
(= v (vector int 1 2 3))
(v:length)                    // 3
(v:at 0)                      // 1
(v:set 0 99)
(v:push 4)
(v:pop)                       // removes and returns last
(v:clear)
(v:resize 10)                 // grow/shrink, zero-fill new slots
(v:reserve 1000)              // pre-allocate capacity
```

Element types: `int`, `float32`, `float64`, `bool`, `uint8`, `int8`, `int16`,
`int32`, and any registered struct type.

---

## Binary Methods

```bbl
(= b (file-bytes "data.bin"))
(b:length)                    // byte count
(b:at 5)                      // byte at index 5 (as int)
(b:set 5 0xFF)                // write byte
(b:slice 0 16)                // sub-binary
(b:resize 1024)               // grow/shrink
(b:copy-from src offset len)  // bulk copy from another binary
```

### Binary Literals

```bbl
(= raw 0b5:hello)             // 5-byte binary literal
(= compressed 0z128:<lz4>)    // LZ4-compressed, decompresses on access
```

---

## JSON

```bbl
(= obj (json-parse "{\"name\":\"alice\",\"age\":30}"))
(print obj.name)               // "alice"

(= json-str (json-encode (table "x" 1 "y" 2)))
(print json-str)               // {"x":1,"y":2}
```

---

## File I/O

```bbl
(= f (fopen "data.txt" "r"))
(= line (f:read-line))
(= content (f:read))           // read all
(f:close)

(= f (fopen "out.txt" "w"))
(f:write "hello\n")
(f:close)

(= bytes (file-bytes "image.png"))    // read entire file as binary
```

Pre-opened: `stdin`, `stdout`, `stderr`.

---

## OS Functions

```bbl
(get-env "HOME")              // environment variable
(set-env "KEY" "val")
(clock)                       // CPU time in seconds (float)
(time)                        // wall time (epoch seconds)
(sleep 0.5)                   // sleep 500ms
(exit 0)                      // terminate

(get-cwd)                     // current directory
(chdir "/tmp")
(mkdir "new-dir")
(remove "file.txt")
(rename "old" "new")
(exists "file.txt")           // true/false
(stat "file.txt")             // table {size, mtime, is-dir}
(glob "*.bbl")                // table of matching paths
(path-join "dir" "file.bbl")  // "dir/file.bbl"
(path-dir "/a/b.bbl")         // "/a"
(path-base "/a/b.bbl")        // "b.bbl"
(path-ext "/a/b.bbl")         // ".bbl"
(path-abs "relative.bbl")     // absolute path
```

### Process Execution

```bbl
(= p (spawn "ls" "-la"))       // start process
(= line (p:read-line))          // read stdout line
(= output (p:read))             // read all stdout
(= code (p:wait))               // wait and get exit code

(= result (execute "echo hi"))  // run and capture output
(print result.output)

(spawn-detached "server" "--port" "8080")  // fire and forget
```

---

## Networking

### TCP

```bbl
(= server (tcp-listen "0.0.0.0" 8080))
(= conn (server:accept))
(= line (conn:read-line))
(conn:write "HTTP/1.1 200 OK\r\n\r\nhello")
(conn:close)

(= sock (tcp-connect "example.com" 80))
(sock:write "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
(= response (sock:read))
(sock:close)
```

Socket methods: `read`, `read-line`, `read-bytes`, `read-string`, `write`,
`write-bytes`, `close`.

### UDP

```bbl
(= sock (udp-open))
(sock:bind 9000)
(sock:send-to "data" "127.0.0.1" 9001)
(= msg (sock:recv-from 1024))    // table {data, addr, port}
(sock:close)
```

---

## Compression

```bbl
(= compressed (compress raw-binary))
(= original (decompress compressed))
```

Uses LZ4 internally.

---

## Error Handling

```bbl
(error "something went wrong")    // throw error
(assert (> x 0) "x must be positive")

(try
    (= result (risky-operation))
    (catch err
        (print "Error: " err)))
```

---

## Modules

```bbl
(= physics (import "physics.bbl"))
(physics:step 0.016)
```

`import` executes the file in its own environment.  The module's top-level
definitions become fields on the returned table.  Subsequent imports of the
same file return the cached table (same object — mutations visible everywhere).

---

## Sandboxing

```bbl
(= result (sandbox "(+ 1 2)" 10000))
```

Evaluate untrusted code with a step limit.  No file I/O, no networking,
no process execution.  Returns the result or null on step limit exceeded.

---

## Sorting

```bbl
(= sorted (sort items))                 // sort vector
(= sorted (sort items (fn (a b) (< a.score b.score))))  // custom comparator
```
