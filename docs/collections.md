# Collections

## Arrays

`array<T>` and `T[]` name the same growable array type. Arrays have shared mutable identity: assigning an array to another variable aliases it, and mutation is visible through both bindings. Indexes are checked.

```flywheel
int[] values = [5, 3, 9]
values.push(1)
values.sort()                         # [1, 3, 5, 9]
values.sort(function(int a, int b) => a > b)
print values.slice(1, 3), values[0..1], values.index_of(9)
values.insert(0, 42)
print values.remove_at(0), values.remove(3), values.join(", ")
```

| Method | Behavior |
|---|---|
| `push(x)`, `pop()` | append; remove and return the last item (`IndexError` when empty) |
| `insert(i, x)` | insert before position `i` (0 to length) |
| `remove_at(i)` | remove and return the item at `i` |
| `remove(x)` | remove the first equal item; returns whether one was found |
| `clear()` | remove everything |
| `extend(other)` | append every item of another sequence of the same item type |
| `sort()` | stable in-place sort of numbers, chars, bools, or strings |
| `sort(less)` | stable sort with `function(T a, T b) returns bool` meaning "a goes before b" |
| `reverse()` | reverse in place |
| `slice(start)`, `slice(start, end)` | new array; negative indices count from the end; bounds are clamped |
| `index_of(x)`, `contains(x)` | first index or -1; membership (also `x in array`) |
| `copy()` | shallow copy |
| `join(separator = "")` | concatenate items; non-strings use their `string(...)` form |
| `map`, `filter`, `reduce(initial, f)`, `any`, `all`, `each` | higher-order helpers |

### Filtering with `which`

`items which condition` returns a new array of the items for which the condition is true. Inside the condition, `.name` reads a member of the item and a condition that starts with an operator applies it to the item:

```flywheel
var adults = people which .age >= 18
var dannys = people which .name == "Danny"
print numbers which > 10
print numbers which > 5 and < 12, numbers which % 2 == 0
print people which not .admin, words which .length > 3, ids which in allowed
```

`which` works on arrays, tuples, and sets, binds looser than every other operator (so `and`/`or` stay inside the condition), and chains: `xs which > 1 which < 9`. Parenthesize it to use the result in a larger expression: `(people which .admin).length`. The condition must be `bool`; it can use local variables and call methods (`people which .greet() == "hi"`).

`a[i..j]` and `a[i..<j]` slice like `slice` with an inclusive or exclusive end. Tuples (`tuple<T>` and `[A, B]`) support the read-only methods (`slice`, `index_of`, `contains`, `copy`, `join`); mutating methods require an array.

A `for` expression is evaluated once and captures the initial length; shrinking the array during iteration can raise `IndexError`. `for index, item in values` also binds the index.

### Frozen collections

`freeze()` makes an array, map, or set immutable in place and returns it; `is_frozen()` reports whether it is. Every later change (`push`, `pop`, `xs[i] = v`, `m[k] = v`, `remove`, `sort`, `clear`, and so on) raises `ValueError`, while reads, iteration, copies, and slices still work (a copy is not frozen).

```flywheel
var primes = [2, 3, 5, 7].freeze()
try
    primes.push(11)
catch ValueError e
    print e.message     # cannot modify a frozen array
end
```

Combine it with a `readonly` field for a value that cannot change at all. Freezing is shallow: a frozen array of arrays keeps its elements mutable.

## Maps

Maps are homogeneous associative values with shared mutable identity, insertion-ordered and hash-indexed:

```flywheel
map<string, int> scores = {"Danny": 15}
scores["Danny"] += 1
scores["Alice"] = 20
print "Alice" in scores, scores.remove("Danny"), scores.keys(), scores.values()
for name, score in scores do
    print "{name}: {score}"
end
```

Keys may be integers, `bool`, `char`, enums, `string`, tuples of these (structural equality and hashing), or classes that define `hash() returns int` and `equals(T other) returns bool`. Missing-key reads raise `KeyError`; plain assignment inserts or updates. `remove(key)` returns whether a key existed, `keys()` and `values()` return array snapshots, and `len(map)` / `.length` count entries. Iteration uses a snapshot in insertion order. Lookup, insertion, and membership take expected constant time. Empty maps need a declared or contextual `map<K,V>` type.

```flywheel
class Point
    int x
    int y
    function hash() returns int
        return this.x * 31 + this.y
    end
    function equals(Point other) returns bool
        return this.x == other.x and this.y == other.y
    end
end
map<Point, string> labels = {Point(1, 2): "a"}
map<[int, string], int> pairs = {[1, "a"]: 10}
print Point(1, 2) in labels, labels[Point(1, 2)]
```

A class with `equals` also uses it for `==` and `!=` between related class values. Class keys call `hash` and `equals` during lookup; an error raised by either propagates normally.

## Sets

`set<T>` holds unique elements in insertion order, with the same element rules as map keys:

```flywheel
set<int> seen = {3, 1, 3}         # {3, 1}
seen.add(5)
print 1 in seen, seen.remove(3), seen.length
var letters = set("hello")        # set<char>
print seen.union({9}), seen.intersection({1}), seen.difference({1}), {1}.is_subset(seen)
for x in seen do
    print x
end
```

`set(sequence)` builds a set from an array, tuple, string, map (its keys), or another set; `set<T>()` or `{}` with an expected type makes an empty one. Methods: `add`, `remove` (returns bool), `contains` (also `in`), `clear`, `items()` (array snapshot), `union`, `intersection`, `difference`, and `is_subset`.

## Iterator protocol

A class becomes iterable by providing `next() returns T?`, or `iterator()` returning such an object. `for x in object` calls `next()` until it returns null:

```flywheel
class Countdown
    int n
    function next() returns int?
        if this.n == 0 do
            return null
        end
        this.n -= 1
        return this.n + 1
    end
end
for n in Countdown(3) do
    print n    # 3, 2, 1
end
```

## Slices

`a:b` is a `slice`: the integers from `a` up to (not including) `b`. A third part is the step, `a:b:step`, which may be negative. Slices are values of the built-in `slice` type, a [struct](types.md#structs) with `start`, `stop`, and `step` fields:

```flywheel
slice s = 1:4
print s, s.length(), s.to_array()        # 1:4 3 [1, 2, 3]
for i in 10:0:-3 do
    print i                               # 10, 7, 4, 1
end
print 5 in 0:10, (0:10:2).contains(3)     # true false
```

In a subscript, a slice picks part of a string or an array, as in Python. Either end may be left out (it then reaches the start or the end), negative ends count from the end, ends past either side are clamped, and the step may be negative:

```flywheel
var text = "hello world"
print text[0:5], text[6:], text[-5:], text[::-1]     # hello world world dlrow olleh
int[] xs = [1, 2, 3, 4, 5, 6]
print xs[1:3], xs[:-1], xs[::2], xs[5:1:-1]          # [2, 3] [1, 2, 3, 4, 5] [1, 3, 5] [6, 5, 4, 3]
print xs[s]                                          # a slice value works too: [2, 3, 4]
```

Slicing makes a new string or array (strings slice by character); it never raises for out-of-range ends, and a step of 0 raises `ValueError`. Inside the brackets `:` separates the parts, so a computed end is written as is: `xs[i + 1:n - 1]`, `xs[lo otherwise 0:]`.

| `slice` member | |
|---|---|
| `start`, `stop`, `step` | the parts (`int`); a part left out in a subscript is a marker that `positions` resolves |
| `length()`, `contains(n)` (also `n in s`), `to_array()` | the numbers of the slice; `for i in s` walks them |
| `positions(length)` | the indices the slice picks from a sequence of `length` items, Python-style |
| `bounds(length)` | `[first, past]`: the first index and the one past the last, for a sequence of `length` |
| `to_string()` | `1:4`, `0:10:2`, `:3` |

A class makes its own subscript slices by taking a `slice` in `operator []`; `positions` does the arithmetic:

```flywheel
class Rows
    string[] rows = ["r0", "r1", "r2", "r3"]
    function operator [](slice part) returns string[]
        string[] out = []
        for i in part.positions(this.rows.length) do
            out.push(this.rows[i])
        end
        return out
    end
end
print Rows()[1:3], Rows()[::-1]       # ["r1", "r2"] ["r3", "r2", "r1", "r0"]
```

A class or struct can also give `:` its own meaning with `operator :(T other)`, which `a:b` calls when `a` is an instance: the built-in [vectors](types.md#built-in-vectors) use it to list every cell between two points. `a:b:c` is `(a:b):c`, so a `slice` takes its step through its own `operator :`.

## Tuples and ranges

Tuples have fixed length and statically known types at each position:

```flywheel
[int, string] pair = [42, "answer"]
print pair[0]
pair[1] = "updated"
var inferred = [true, "heterogeneous"]
```

With a tuple context, a bracket literal constructs that tuple. Otherwise, items with a lossless common type infer an array; heterogeneous items infer a tuple. Fixed heterogeneous tuple indices must be compile-time integer expressions. Homogeneous `tuple<T>` values additionally support dynamic checked indices, membership and iteration, with runtime-determined fixed arity. The `..` and `..<` operators produce `tuple<int>`.

`collections.range(end)` and `collections.range(start,end)` create an `int[]` for the half-open interval, with step 1. See [formatting, casts, repetition, and ranges](value-conversions.md) for constructors and operators.
