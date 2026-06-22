# Aether Type Inference Guide

## How Type Inference Works

Aether implements a type inference system that automatically deduces types from context, reducing the need for explicit type annotations in most cases.

## Basic Inference

### From Literals

```aether
x = 42              // inferred: int
pi = 3.14           // inferred: float
name = "Alice"      // inferred: string
flag = true         // inferred: bool
p = null            // inferred: ptr
```

A bare integer literal always infers as `int` — there is no `long`
literal or `long <value>` prefix. To get a `long` (64-bit) binding,
annotate the declaration explicitly:

```aether
big = 0             // inferred: int
long wide = 0       // explicit: long (64-bit)
```

### From Expressions

```aether
a = 10
b = 20
sum = a + b           // inferred: int (both operands are int)

x = 3.14
y = 2.0
result = x * y        // inferred: float
```

### From Functions

```aether
// Return type inferred from return statement
add(a, b) {
    return a + b      // If used with ints -> int, if with floats -> float
}

// Parameters inferred from usage
multiply(x, y) {
    return x * y
}

main() {
    n = add(10, 20)             // add inferred as: int -> int -> int
    f = multiply(3.14, 2.0)     // multiply inferred as: float -> float -> float
}
```

## Advanced Inference

### Struct Fields

```aether
struct Point {
    x,    // Type inferred from initialization
    y
}

struct Player {
    name,
    health,
    score
}

main() {
    p = Point{ x: 10, y: 20 }   // x, y inferred as int

    player = Player{
        name: "Alice",          // string
        health: 100,            // int
        score: 0                // int
    }
}
```

Struct fields infer their types from the initialisation site. A struct
declared with bare field names (no `: type`) and never initialised keeps
the fields at the parser's fallback type. Always initialise via a struct
literal at least once, or annotate the field explicitly.

### Array Elements

```aether
nums = [1, 2, 3, 4, 5]             // inferred: int[]
names = ["Alice", "Bob"]           // inferred: string[]
mixed_ok = [1.0, 2.0, 3.0]         // inferred: float[]
```

### Through Assignments

```aether
x = 42            // x: int
y = x             // y: int (inferred from x)
z = y + 10        // z: int (inferred from y + int)
```

## When to Use Explicit Types

While inference handles most cases, explicit types are useful for:

### 1. Function Signatures (Documentation)

```aether
// Explicit types make intent clear
calculate_damage(base: int, defense: int): int {
    return base - defense
}

// vs inferred (less clear to reader)
calculate_damage(base, defense) {
    return base - defense
}
```

### 2. Complex Scenarios

```aether
// When inference might be ambiguous
process_data(input: float[]) {
    // Explicit type ensures correct interpretation
}
```

### 3. Public APIs

```aether
// Export with explicit types for clarity
export calculate_score(kills: int, deaths: int, assists: int): float {
    return (kills + assists / 2.0) / (deaths + 1)
}
```

## Mixed Explicit/Inferred

You can mix explicit and inferred types:

```aether
// Some parameters explicit, some inferred
process(data, threshold: int) {
    return data > threshold   // data type inferred from usage
}

// Explicit return, inferred parameters
format_name(first, last): string {
    return "${first} ${last}"  // parameters inferred as string
}
```

## How Inference Works Internally

### Phase 1: Constraint Collection

The compiler walks the AST and collects type constraints:

```aether
x = 42
// Constraint: x must be int (from literal 42)

y = x + 10
// Constraint: y must be int (from x:int + 10:int)
```

### Phase 2: Constraint Solving

Constraints are propagated iteratively until all types are known:

```
Iteration 1: x = int (from literal)
Iteration 2: y = int (from x + int)
Done: All types resolved
```

### Phase 3: Validation

Once inferred, types are validated for consistency:

```aether
x = 42
x = "hello"   // ERROR: Can't assign string to int
```

## Null and Pointer Inference

The `null` keyword is typed as `ptr`:

```aether
conn = null                  // inferred: ptr
conn = tcp_connect_raw(...)  // still ptr — type is consistent
```

Integer `0` is compatible with `ptr` for null-initialization patterns:

```aether
server = 0                   // initially int
server = tcp_listen_raw(80)  // tcp_listen_raw returns ptr — type widens to ptr
```

### Constants

Top-level `const` declarations infer their type from the value:

```aether
const MAX = 100          // inferred: int
const NAME = "hello"     // inferred: string
```

## Edge Cases

### Ambiguous Types

```aether
// This FAILS - can't infer type without usage
x: int              // Declare with explicit type
x = get_value()     // OK - type is known
```

### Generic Functions

```aether
// Currently not supported - specify explicit types
identity(x: int): int {
    return x
}

// Future: Generic type parameters
// identity<T>(x: T): T {
//     return x
// }
```

## Performance Impact

Type inference happens at **compile-time only**:

- Zero runtime overhead
- Same generated C code as explicit types
- Full type safety maintained
- No performance difference

## Best Practices

### Do:
- Let inference handle obvious cases (literals, simple expressions)
- Use explicit types for public APIs and exports
- Use explicit types when it improves readability
- Mix explicit/inferred for balance

### Don't:
- Over-annotate everything (defeats the purpose)
- Rely on inference for complex logic (document with types)
- Leave ambiguous types un-annotated

## Examples

### Good: Minimal but Clear

```aether
struct Player {
    name: string,    // Explicit for documentation
    health,          // Inferred from init
    score            // Inferred from init
}

calculate_total(base, bonus) {  // Inferred from usage
    return base + bonus
}

main() {
    p = Player{ name: "Alice", health: 100, score: 0 }
    total = calculate_total(p.score, 10)
}
```

### Better: Strategic Explicit Types

```aether
struct Player {
    name: string,
    health: int,     // Explicit: important field
    score: int       // Explicit: important field
}

// Explicit return: public function
calculate_total(base, bonus): int {
    return base + bonus
}

main() {
    p = Player{ name: "Alice", health: 100, score: 0 }
    total = calculate_total(p.score, 10)
}
```

## Error Messages

When inference fails, Aether provides helpful errors:

```aether
x = unknown_value()   // Type cannot be inferred

// Error: Type inference failed
//   Line 1: Variable 'x' has ambiguous type
//   help: Add type annotation: x: int = unknown_value()
```

## Summary

- **Inference is powerful** - Handles most common cases automatically
- **Explicit types are documentation** - Use strategically for public APIs
- **No runtime cost** - Type inference is compile-time only
- **Fully type-safe** - Same guarantees as explicit types

Type inference gives Aether the expressiveness of ML/Haskell while maintaining C-level type safety and performance.

