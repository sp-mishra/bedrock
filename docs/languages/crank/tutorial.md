# Crank Language Tutorial

**Location:** `src/examples/crank/example_crank_tutorial.hpp`
**Struct:** `crank_tutorial::CrankTutorial`

Loads Crank tutorial scripts from `src/examples/crank/resources/ex*.ck`, runs the full pipeline for each, and displays
results. No Crank source in the header.

## Pipeline (per script)

```
read file → parse → analyse → lower_to_hl (fn="Main") → execute_via_interpreter → display result
```

Each step propagates failure immediately via `testfw::Result`. Passes and failures are counted; the example fails if any
script fails.

## Resource Files

`src/examples/crank/resources/ex*.ck` — scripts processed in sorted filename order.

| File                      | What it demonstrates                                          |
|---------------------------|---------------------------------------------------------------|
| `ex1_1_basic_types.ck`    | Fixed-width types, function declaration                       |
| `ex1_2_numeric_types.ck`  | All numeric types (Int8/16/32/64, UInt8/16/32/64, Float32/64) |
| `ex1_3_control_flow.ck`   | if/else, for-range loops                                      |
| `ex2_1_collections.ck`    | Arrays `[N]T`, slices `[]T`, `len()`, iteration               |
| `ex2_2_strings.ck`        | String literals, length, concatenation                        |
| `ex3_1_structs.ck`        | Struct declaration, field access                              |
| `ex3_2_enums.ck`          | Enum sum types, exhaustive pattern matching                   |
| `ex4_1_basic_generics.ck` | Generic functions, type parameters                            |
| `ex4_2_trait_bounds.ck`   | Trait bounds `[T: Comparable]`, conformance                   |
| `ex5_1_option.ck`         | `Option[T]`, `Some`/`None`                                    |
| `ex5_2_result.ck`         | `Result[T,E]`, `Ok`/`Err`, error propagation                  |
| `ex8_1_stack.ck`          | Stack[T] LIFO structure                                       |
| `ex8_2_queue.ck`          | Queue[T] FIFO structure                                       |
| `ex8_3_linked_list.ck`    | Singly-linked list, recursive nodes                           |
| `ex8_4_bst.ck`            | BST, recursive tree structure                                 |
| `ex10_1_contracts.ck`     | Function contracts: `requires`/`ensures`                      |

## Running

```bash
cmake -S . -B build -DBEDROCK_BUILD_EXAMPLES=ON
cmake --build build --target bedrock_examples
./build/bedrock_examples crank_tutorial
```

## Resource Dir Resolution

The example finds its adjacent `resources/` directory with `std::source_location`; it does not depend on a project-root
compile definition and remains relocatable with its `.ck` files.

## Output

Each script logs:

```
crank tutorial [ex1_1_basic_types]: status=ok return=7
```

Final summary:

```
crank tutorial: 16/16 scripts passed
```

**Tags:** `crank`, `tutorial`, `beginner`, `intermediate`, `advanced`, `types`, `generics`, `data-structures`, `safety`,
`verification`

---

*Tutorial integrated with testfw::Registry for compile-time registration and runtime execution.*
