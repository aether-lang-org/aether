# Aether Type Annotation Style Guide

## Philosophy

Aether supports **both type inference and explicit type annotations**, giving you flexibility to choose the right approach for your code. This guide provides best practices for when to use each.

## Type Inference (Recommended Default)

Aether's type inference can deduce types automatically in most cases.

### When to Use Type Inference

**Local variables**
```aether
// Type inference - clean and concise
x = 42              // Inferred as int
name = "Alice"      // Inferred as string
points = [1, 2, 3]  // Inferred as int array
```

**Simple computations**
```aether
sum = a + b
product = x * y
result = compute_value()
```

**Loop variables**
```aether
for i in 0..10 {
    total = total + i
}
```

**Struct field access**
```aether
point = Point{ x: 5, y: 10 }
x_value = point.x  // Inferred from struct definition
```

## Explicit Types (For Clarity)

Use explicit type annotations when they improve code clarity or are required.

### When to Use Explicit Types

**Function signatures** (highly recommended)
```aether
// Function parameters and return types
add(int a, int b): int {
    return a + b
}

greet(string name): void {
    println("Hello, ${name}")
}

find_user(int id): User {
    // ... implementation
}
```

**Why**: Makes function contracts clear, improves documentation, helps catch errors at call sites.

**Public APIs and library functions**
```aether
// Export functions should have explicit types
export calculate_distance(p1: Point, p2: Point): float {
    dx = p2.x - p1.x
    dy = p2.y - p1.y
    return sqrt(dx * dx + dy * dy)
}
```

**Complex types or ambiguous cases**
```aether
import std.map
import std.list

// When type inference might fail or be unclear
ptr my_map = map.new()
ptr users = list.new()

// Explicit type prevents ambiguity
int result = parse_number(input)  // Clarifies we want int, not float
```

**Actor state declarations**
```aether
actor Counter {
    state int count            // Explicit prefix form for clarity
    state string name
    state int[] values
    state total = 0            // ...or inferred from an initializer
    state users = null         //    (inferred: ptr)
    
    // ...
}
```

**Struct field definitions**
```aether
struct User {
    int id
    string name
    string email
    bool active
    string[] tags
}
```

## Style Consistency

### Within a Module

**Be consistent** within a single file or module:

```aether
// Option A: Pure inference (for small scripts)
x = 10
y = 20
sum = x + y

// Option B: Explicit types (for larger projects)
int x = 10
int y = 20
int sum = x + y
```

**Don't mix** styles randomly:
```aether
// Inconsistent - avoid
int x = 10
y = 20           // Sudden switch to inference
int sum = x + y  // Back to explicit
```

### Across a Project

Establish team conventions:
- **Scripts/prototypes**: Use type inference for speed
- **Libraries/APIs**: Use explicit types for clarity
- **Performance-critical code**: Use explicit types for control

## Type Annotations Syntax

### Variables

```aether
// Explicit type annotation
int x = 42
string name = "Alice"
float pi = 3.14159
bool active = true

// Type inference (no annotation)
x = 42
name = "Alice"
pi = 3.14159
active = true
```

### Functions

```aether
// Explicit parameter and return types
multiply(int a, int b): int {
    return a * b
}

// Mixed: explicit parameters, inferred return
add(int a, int b) {
    return a + b  // Return type inferred
}

// Full inference (not recommended for public functions)
subtract(a, b) {
    return a - b
}
```

### Arrays

```aether
// Explicit array type (suffix form)
int[] numbers = [1, 2, 3, 4, 5]
string[] names = ["Alice", "Bob"]

// Type inference from elements
numbers = [1, 2, 3, 4, 5]  // Inferred as int[]
names = ["Alice", "Bob"]   // Inferred as string[]
```

> **Note:** The array type syntax is the C-style suffix `T[]`, not the prefix
> `[T]`. Earlier drafts of this guide used the prefix form; the parser has
> never accepted it. If you see `[string]` in older code or docs, replace it
> with `string[]`.

### Complex Types

```aether
import std.map
import std.list

// Explicit for complex types
ptr user_map = map.new()
ptr points = list.new()

// Inference for simple cases
my_map = map.new()
my_points = list.new()
counter = spawn(Counter())
```

## Examples

### Example 1: Script-Style (Inference-Heavy)

```aether
// Quick prototype or script
fibonacci(n) {
    if n <= 1 {
        return n
    }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

main() {
    result = fibonacci(10)
    println("Fib(10) = ${result}")
}
```

### Example 2: Library-Style (Explicit Types)

```aether
// Public library with clear contracts
export struct Point {
    float x
    float y
}

export distance(p1: Point, p2: Point): float {
    float dx = p2.x - p1.x
    float dy = p2.y - p1.y
    return sqrt(dx * dx + dy * dy)
}

export midpoint(p1: Point, p2: Point): Point {
    return Point{
        x: (p1.x + p2.x) / 2.0,
        y: (p1.y + p2.y) / 2.0
    }
}
```

### Example 3: Balanced Approach (Recommended)

```aether
// Explicit where it matters, inference elsewhere
import std.file

process_file(string filename): int {
    int count = 0

    // Inference for simple local variables
    f = file.open(filename, "r")
    content = file.read_all(f)
    file.close(f)

    println("Read file: ${filename}")
    println(content)

    return count
}
```

## When explicit types are required

Some cases don't give the inferencer enough to work with; annotate explicitly:

```aether
// This fails - type can't be inferred
x = undefined()
y = get_value()  // If get_value() return type is unknown

// Solution: Provide explicit type
int x = undefined()
int y = get_value()
```

```aether
// Recursive functions need return type
fibonacci(n) {
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)  // ERROR
}

// Explicit return type
fibonacci(int n): int {
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)
}
```

## Summary

| Use Case | Recommendation | Example |
|----------|---------------|---------|
| Local variables | Inference | `x = 42` |
| Function parameters | Explicit | `add(int a, int b)` |
| Function returns | Explicit | `add(...): int` |
| Struct fields | Explicit | `struct User { int id }` |
| Actor state | Explicit (prefix) | `state int count` |
| Loop variables | Inference | `for i in 0..10` |
| Complex types | Explicit | `ptr my_map = map.new()` |
| Public APIs | Explicit | All types annotated |
| Scripts/prototypes | Inference | Minimal annotations |

**Golden Rule**: Use explicit types when they add **clarity** or are **required**. Use inference when types are **obvious** from context.

## Tooling Support

The Aether LSP provides:
- Type inference on hover
- Type error diagnostics
- Auto-completion for type annotations (planned)
- "Add type annotation" quick fix (planned)

---

*For more information on type inference, see [type-inference-guide.md](type-inference-guide.md)*
