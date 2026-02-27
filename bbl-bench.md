# bbl-bench: LLM coding benchmark for BBL

Measure how well different models write code in a novel language they haven't been trained on.

## overview

Five programming tasks in BBL (a Lisp-like scripting language).  Each model gets one shot — read the language guide, write the script, scored pass/fail against deterministic expected output via SHA-256 checksums.

**What it tests:** ability to learn and apply an unfamiliar language from documentation alone, without prior training data.

## setup

```
bbl-bench/                     # home base (scoring, results)
├── bbl                        # compiled binary (copy from build/bbl)
├── doc/
│   └── bbl.md                 # language guide (the only documentation)
├── expected/                  # copied from bblbench/expected/
├── run.sh                     # copied from bblbench/run.sh
├── bench.sh                   # orchestrator (runs all 5 tasks for one model)
└── results/                   # output directory
    └── <model>-<timestamp>/
        ├── 1_file_gen.bbl
        ├── ...
        └── score.txt

/tmp/bbl-bench-<model>-<ts>/   # per-task isolation (auto-created, auto-cleaned)
├── 1_file_gen/                # each task gets its own directory containing:
│   ├── bbl                    #   - the binary
│   ├── doc/bbl.md             #   - the language guide
│   └── 1_file_gen.bbl         #   - ONLY the stub for this task
├── 2_primes/
│   └── ...
└── ...
```

**Isolation:** Each task runs in a separate `/tmp` directory outside the repo tree.
This prevents two contamination vectors:
1. **Cross-task:** copilot can't read solutions from earlier tasks (no other `.bbl` files present)
2. **Repo access:** copilot can't walk up to `bblbench/` reference solutions (workspace is under `/tmp`)

### preparation

```sh
mkdir -p bbl-bench/doc bbl-bench/expected bbl-bench/results
cp build/bbl bbl-bench/bbl
cp doc/bbl.md bbl-bench/doc/bbl.md
cp bblbench/expected/* bbl-bench/expected/
cp bblbench/run.sh bbl-bench/run.sh
# bench.sh creates per-task /tmp workspaces automatically — no stubs needed here
```

## available models

From `copilot --help --model`:

| CLI string | Display name |
|-----------|-------------|
| `claude-sonnet-4.6` | Claude Sonnet 4.6 |
| `claude-sonnet-4.5` | Claude Sonnet 4.5 |
| `claude-haiku-4.5` | Claude Haiku 4.5 |
| `claude-opus-4.6` | Claude Opus 4.6 |
| `claude-opus-4.6-fast` | Claude Opus 4.6 (fast mode) |
| `claude-opus-4.5` | Claude Opus 4.5 |
| `claude-sonnet-4` | Claude Sonnet 4 |
| `gemini-3-pro-preview` | Gemini 3 Pro |
| `gpt-5.3-codex` | GPT-5.3-Codex |
| `gpt-5.2-codex` | GPT-5.2-Codex |
| `gpt-5.2` | GPT-5.2 |
| `gpt-5.1-codex-max` | GPT-5.1-Codex-Max |
| `gpt-5.1-codex` | GPT-5.1-Codex |
| `gpt-5.1` | GPT-5.1 |
| `gpt-5.1-codex-mini` | GPT-5.1-Codex-Mini (Preview) |
| `gpt-5-mini` | GPT-5 mini |
| `gpt-4.1` | GPT-4.1 |

## task prompts

Each task is run as a single `copilot -p` invocation from the bbl-bench directory.
The model sees `doc/bbl.md` plus whatever other files it reads from the workspace.

### task 1: file_gen

```
Read doc/bbl.md to learn the BBL language. Write 1_file_gen.bbl that generates a
15x15 multiplication table and writes it to /tmp/bblbench_mult.txt. Header row:
4-char right-aligned column numbers preceded by four spaces then a pipe character.
Separator: four dashes then a plus sign, followed by four dashes per column. Data
rows: 4-char right-aligned row label then pipe then 4-char right-aligned products.
Print the byte count followed by ' bytes written' and a newline to stdout, where
byte count is the string length of the full table text. Use fopen/write/close for
file I/O. Test with: ./bbl 1_file_gen.bbl
```

### task 2: primes

```
Read doc/bbl.md to learn the BBL language. Write 2_primes.bbl that finds all
primes up to 200 using the Sieve of Eratosthenes. Print each prime on its own
line (just the number). After all primes print the line: total: COUNT where COUNT
is the number of primes found. Use a table as a sparse boolean array. Use sqrt
and floor for the sieve limit. Test with: ./bbl 2_primes.bbl
```

### task 3: sort

```
Read doc/bbl.md to learn the BBL language. Write 3_sort.bbl that performs
insertion sort on this exact dataset of 20 numbers: 38 27 43 3 9 82 10 44 17 25
91 4 62 55 71 36 15 48 20 67. Build a table using push for each number (gives
0-based keys). Print 'unsorted: ' followed by space-separated values, then a
newline. Implement insertion sort (not bubble sort, not selection sort). Print
'sorted:   ' (note: 3 spaces after the colon) followed by space-separated values,
then a newline. Verify the sort and print 'sorted_ok: true' or 'sorted_ok: false'.
Print 'min: VALUE' and 'max: VALUE'. Use table.get and table.set for element
access (0-based keys from push). Test with: ./bbl 3_sort.bbl
```

### task 4: collatz

```
Read doc/bbl.md to learn the BBL language. Write 4_collatz.bbl analyzing the
Collatz conjecture for starting values 1 through 50. For each n compute steps
until n reaches 1 (even: n/2, odd: 3n+1). Print each line as the number then a
space then the step count. Print 'longest: n=N steps=STEPS' for the longest
chain. Print 'total_steps: SUM' for the sum of all step counts. Test with:
./bbl 4_collatz.bbl
```

### task 5: closure

```
Read doc/bbl.md to learn the BBL language. Write 5_closure.bbl implementing map,
filter, reduce as higher-order functions using closures. Build a dataset of
integers 1..20 via push. Use (each i container body...) to iterate tables and
vectors — it binds i from 0 to length-1.  Access elements via (container.at i).
Implement print-table that prints space-separated elements + newline. Compute and
print: 'squares: ' then squared values via print-table, 'even_squares: ' then
even squares via print-table, 'sum_even_squares: VALUE', 'sum_odd_squares: VALUE'
(filter odds from original, square, sum), 'shifted: ' then each value + 10 via a
make-adder closure factory and print-table, 'squares_over_100: COUNT' counting
squares > 100 using reduce. Test with: ./bbl 5_closure.bbl
```

## bench.sh — orchestrator script

```bash
#!/usr/bin/env bash
# bench.sh — run all 5 bbl-bench tasks for a single model
# Usage: ./bench.sh <model-name>
# Example: ./bench.sh claude-sonnet-4.6
#
# Each task runs in an isolated /tmp directory containing ONLY:
#   - bbl binary
#   - doc/bbl.md (language guide)
#   - the single stub .bbl file
#
# This prevents cross-task contamination (reading other solutions)
# and repo contamination (walking up to bblbench/ reference solutions).

set -euo pipefail

MODEL="${1:?Usage: ./bench.sh <model-name>}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
RUN_DIR="$BENCH_DIR/results/${MODEL}-${TIMESTAMP}"

mkdir -p "$RUN_DIR"

echo "bbl-bench — model: $MODEL"
echo "results:  $RUN_DIR"
echo "========================================="

TASKS=(
    "1_file_gen"
    "2_primes"
    "3_sort"
    "4_collatz"
    "5_closure"
)

PROMPTS=(
    "Read doc/bbl.md to learn the BBL language. Write 1_file_gen.bbl that generates a 15x15 multiplication table and writes it to /tmp/bblbench_mult.txt. Header row: 4-char right-aligned column numbers preceded by four spaces then a pipe character. Separator: four dashes then a plus sign, followed by four dashes per column. Data rows: 4-char right-aligned row label then pipe then 4-char right-aligned products. Print the byte count followed by ' bytes written' and a newline to stdout, where byte count is the string length of the full table text. Use fopen/write/close for file I/O. Test with: ./bbl 1_file_gen.bbl"
    "Read doc/bbl.md to learn the BBL language. Write 2_primes.bbl that finds all primes up to 200 using the Sieve of Eratosthenes. Print each prime on its own line (just the number). After all primes print the line: total: COUNT where COUNT is the number of primes found. Use a table as a sparse boolean array. Use sqrt and floor for the sieve limit. Test with: ./bbl 2_primes.bbl"
    "Read doc/bbl.md to learn the BBL language. Write 3_sort.bbl that performs insertion sort on this exact dataset of 20 numbers: 38 27 43 3 9 82 10 44 17 25 91 4 62 55 71 36 15 48 20 67. Build a table using push for each number (gives 0-based keys). Print 'unsorted: ' followed by space-separated values, then a newline. Implement insertion sort (not bubble sort, not selection sort). Print 'sorted:   ' (note: 3 spaces after the colon) followed by space-separated values, then a newline. Verify the sort and print 'sorted_ok: true' or 'sorted_ok: false'. Print 'min: VALUE' and 'max: VALUE'. Use table.get and table.set for element access (0-based keys from push). Test with: ./bbl 3_sort.bbl"
    "Read doc/bbl.md to learn the BBL language. Write 4_collatz.bbl analyzing the Collatz conjecture for starting values 1 through 50. For each n compute steps until n reaches 1 (even: n/2, odd: 3n+1). Print each line as the number then a space then the step count. Print 'longest: n=N steps=STEPS' for the longest chain. Print 'total_steps: SUM' for the sum of all step counts. Test with: ./bbl 4_collatz.bbl"
    "Read doc/bbl.md to learn the BBL language. Write 5_closure.bbl implementing map, filter, reduce as higher-order functions using closures. Build a dataset of integers 1..20 via push. Use (each i container body...) to iterate tables and vectors — it binds i from 0 to length-1. Access elements via (container.at i). Implement print-table that prints space-separated elements + newline. Compute and print: 'squares: ' then squared values via print-table, 'even_squares: ' then even squares via print-table, 'sum_even_squares: VALUE', 'sum_odd_squares: VALUE' (filter odds from original, square, sum), 'shifted: ' then each value + 10 via a make-adder closure factory and print-table, 'squares_over_100: COUNT' counting squares > 100 using reduce. Test with: ./bbl 5_closure.bbl"
)

# Run each task in an isolated temp directory
for idx in "${!TASKS[@]}"; do
    task="${TASKS[$idx]}"
    prompt="${PROMPTS[$idx]}"

    echo ""
    echo "--- task: $task ---"

    # Create isolated workspace under /tmp (outside repo tree)
    TASK_DIR="/tmp/bbl-bench-${MODEL}-${TIMESTAMP}/${task}"
    mkdir -p "$TASK_DIR/doc"

    # Copy ONLY what the model should see
    cp "$BENCH_DIR/bbl" "$TASK_DIR/bbl"
    cp "$BENCH_DIR/doc/bbl.md" "$TASK_DIR/doc/bbl.md"
    echo "; write your solution here" > "$TASK_DIR/$task.bbl"

    echo "  workspace: $TASK_DIR"
    echo "  files: bbl, doc/bbl.md, $task.bbl"

    # Time the copilot invocation from the isolated directory
    cd "$TASK_DIR"
    { time copilot \
        -p "$prompt" \
        --model "$MODEL" \
        --yolo \
        --no-ask-user \
        2>/dev/null ; } 2> "$RUN_DIR/${task}.time"

    # Check if copilot actually wrote something
    if [[ "$(cat "$TASK_DIR/$task.bbl")" == "; write your solution here" ]]; then
        echo "  WARNING: copilot did not modify $task.bbl — stub unchanged"
    fi

    # Capture the written file back to results
    cp "$TASK_DIR/$task.bbl" "$RUN_DIR/$task.bbl"

    # Print timing
    elapsed=$(grep real "$RUN_DIR/${task}.time" | awk '{print $2}')
    echo "  time: $elapsed"
done

# Score all 5 tasks
echo ""
echo "========================================="
echo "scoring..."
echo ""

cd "$BENCH_DIR"

# Copy results into bench dir for run.sh (it uses SCRIPT_DIR)
for f in 1_file_gen 2_primes 3_sort 4_collatz 5_closure; do
    cp "$RUN_DIR/$f.bbl" "$BENCH_DIR/$f.bbl"
done

rm -f /tmp/bblbench_mult.txt

# Re-run file_gen to produce the output file for scoring
"$BENCH_DIR/bbl" "$BENCH_DIR/1_file_gen.bbl" > /dev/null 2>&1 || true

bash "$BENCH_DIR/run.sh" "$BENCH_DIR/bbl" 2>&1 | tee "$RUN_DIR/score.txt"

# Extract timing summary
echo "" | tee -a "$RUN_DIR/score.txt"
echo "--- timing ---" | tee -a "$RUN_DIR/score.txt"
for task in "${TASKS[@]}"; do
    elapsed=$(grep real "$RUN_DIR/${task}.time" | awk '{print $2}')
    echo "$task: $elapsed" | tee -a "$RUN_DIR/score.txt"
done

echo "" | tee -a "$RUN_DIR/score.txt"
echo "model: $MODEL" | tee -a "$RUN_DIR/score.txt"
echo "timestamp: $TIMESTAMP" | tee -a "$RUN_DIR/score.txt"

# Cleanup temp directories
rm -rf "/tmp/bbl-bench-${MODEL}-${TIMESTAMP}"
echo ""
echo "cleaned up temp workspaces"
```

## execution

```sh
cd bbl-bench

# Run a single model
./bench.sh claude-sonnet-4.6

# Run multiple models for comparison
for model in claude-sonnet-4.6 claude-opus-4.6 claude-sonnet-4.5 gpt-5.2-codex gpt-4.1 gemini-3-pro-preview claude-haiku-4.5; do
    ./bench.sh "$model"
done
```

## scoring

`run.sh` compares each script's stdout (and file output for task 1) against SHA-256 checksums of the expected output.  The score is simply pass/fail per task.

| Metric | Source |
|--------|--------|
| Pass/fail per task | run.sh output |
| Wall-clock time per task | bash `time` in bench.sh |
| Total time for 5 tasks | sum of per-task times |
| Model script saved | `results/<model>-<ts>/<task>.bbl` |

### result format (score.txt)

```
bblbench — BBL programming benchmark suite
============================================

  PASS  file_gen
  FAIL  primes
  PASS  sort
  PASS  collatz
  FAIL  closure

Passed: 3  Failed: 2  Total: 5

--- timing ---
1_file_gen: 0m12.345s
2_primes: 0m8.765s
3_sort: 0m10.123s
4_collatz: 0m7.890s
5_closure: 0m15.432s

model: claude-sonnet-4.5
timestamp: 20260225-143012
```

## known issues & lessons learned

Discovered during live runs with claude-sonnet-4.6 (2025-02-25):

### 1. Do NOT use `-s` (silent mode)

`-s` / `--silent` can cause copilot to exit immediately without writing any files.
Task 3 failed silently on the first attempt: 2.7s wallclock, no output, stub unchanged.
Retry without `-s` succeeded normally (47s).  **bench.sh omits `-s` for this reason.**

### 2. Cross-task contamination (fixed)

Copilot reads other `.bbl` files in the workspace.  In the initial (non-isolated) run,
task 3 read `2_primes.bbl` (the completed solution from task 2) before writing its own.
With `--yolo` (implies `--allow-all-paths`), copilot could also walk up to `bblbench/`
and read the reference solutions.

**Fix:** Each task now runs in a separate `/tmp` directory containing only the binary,
`doc/bbl.md`, and the single stub file.  No other `.bbl` files, no repo parent.
Sonnet 4.6 still scored 5/5 with full isolation, confirming it learns from docs alone.

### 3. "Path already exists" warning

Copilot tries to create the target `.bbl` file, gets blocked because the stub exists,
then reads and edits it instead.  This wastes one tool call but doesn't affect results.

### 4. Self-correction works (with `--yolo`)

On task 5, copilot discovered that BBL's `if` doesn't return branch values and adapted
using `do` blocks with explicit assignment.  This self-correction is only possible
because `--yolo` auto-approves running `./bbl` to test — without it, copilot would
need permission to execute each test.

### 5. bench.sh detects unchanged stubs

If copilot fails to write a file, bench.sh prints a WARNING.  The file is still copied
to the results directory (as the unchanged stub), and `run.sh` will score it as FAIL.

### 6. Scoring requires re-running file_gen

Task 1 writes to `/tmp/bblbench_mult.txt`, but copilot ran in an isolated `/tmp` dir.
The output file exists from the copilot run, but `run.sh` needs `1_file_gen.bbl` to
re-execute in the scoring directory.  bench.sh now re-runs file_gen before scoring.

## baseline results

### claude-sonnet-4.6 — 2025-02-25 (isolated)

```
  PASS  file_gen     1m17.448s
  PASS  primes       0m41.733s
  PASS  sort         ~1m00s
  PASS  collatz      ~0m40s
  PASS  closure      2m52.257s

Passed: 5/5
Total time: ~6m31s
```

Notes:
- Each task ran in an isolated `/tmp` directory — no cross-task or repo contamination
- Task 5 (closure) took ~3 minutes due to self-correction (if-return discovery)
- Task 1 (file_gen, rated "Hard") passed on first attempt with no prior BBL examples
- All tasks passed — Sonnet 4.6 can learn BBL from docs alone
- Times are ~1.5-2x slower than the initial non-isolated run (expected: no cached BBL examples)

### gpt-5.1-codex-mini — 2025-02-26 (isolated)

```
  PASS  file_gen     4m12.222s
  PASS  primes       1m43.716s
  PASS  sort         1m43.640s
  PASS  collatz      1m26.102s
  PASS  closure     16m33.180s

Passed: 5/5
Total time: ~25m39s
```

Notes:
- Each task ran in an isolated `/tmp` directory — no contamination
- Task 5 (closure) took 16+ minutes — massive iteration (delete-and-rewrite twice, ~20 edit/run cycles)
- Task 1 (file_gen) needed multiple debug-print iterations before converging (4m12s)
- Task 2 (primes) first attempt output 201 lines (included composites), self-corrected on second run
- Tasks 3 and 4 were one-shot passes — clean and fast
- 3-4x slower than Sonnet 4.6 overall, with the gap widest on the hardest task (closure: 16m vs 3m)
- Self-correction works but is expensive — many more iterations than Sonnet

### claude-sonnet-4.6 — 2025-02-25 (non-isolated, for reference)

```
  PASS  file_gen     0m40.672s
  PASS  primes       0m29.696s
  PASS  sort         0m47.000s
  PASS  collatz      0m32.431s
  PASS  closure      2m04.033s

Passed: 5/5
Total time: ~3m54s
```

Notes:
- Contaminated: later tasks could read earlier solutions
- Faster due to copilot caching + learning from prior BBL in workspace

## expected difficulty

| Task | Difficulty | Key trap |
|------|-----------|---------|
| 1_file_gen | Hard | Must use `str` for int→string, `pad-left` with string building, exact formatting |
| 2_primes | Medium | `do` block needed for multi-statement `if`, table-as-sparse-array pattern |
| 3_sort | Medium | 1-based keys from `push`, `table.get`/`table.set` for mutation, inner loop extraction |
| 4_collatz | Easy | Clean if/else, straightforward loop, `do` for best-tracking update |
| 5_closure | Hard | HOF patterns, `.at` (0-based) vs `.get` (key-based), `count-if` via reduce |

## key design decisions

1. **Oneshot, no iteration.** Each model gets exactly one `copilot -p` call per task.  No retries, no feedback loops.  This measures first-attempt capability.

2. **`--yolo --no-ask-user` (no `-s`)** — full auto-approve, no interactive questions.  The model reads docs, writes the file, tests it, and exits.  `-s` is intentionally omitted (see known issues).

3. **Same docs for all models.** Every model sees only `doc/bbl.md`.  No special hints, no examples of solutions.

4. **Deterministic scoring.** SHA-256 checksums on exact output.  No fuzzy matching — either the output is bit-identical or it fails.

5. **Isolation.** Each task runs in a fresh `/tmp` directory with only the binary, docs, and stub.  No other `.bbl` files, no repo tree.  Prevents cross-task learning and repo-walking contamination.

6. **Timing is informational.** The primary metric is pass/fail.  Time is tracked for curiosity but a slow pass beats a fast fail.
