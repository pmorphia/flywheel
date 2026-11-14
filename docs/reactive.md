# Reactive variables and negated aliases

## `reactive`, `when`, and `computed`

A `reactive` variable can be watched. `when condition do ... end` runs its body each time the condition **becomes** true (`when condition run call()` runs one call, with no `end`), an optional `else do` body runs each time it becomes false again, and `computed` declares a value derived from reactive variables:

```flywheel
reactive int score = 0
computed bool winning = score >= 10

when winning do
    print "winning with {score}"
end

score += 4          # nothing: winning is still false
score += 7          # prints "winning with 11"
score += 1          # nothing: winning stays true
score = 0           # winning becomes false, so the when re-arms
score = 12          # prints "winning with 12"
```

`reactive` and `computed` precede a declaration: `reactive bool alive = true`, `reactive var name = "a"`, `computed string label = name.upper()`.

A `when` behaves like this:

| Situation | Behavior |
|---|---|
| The condition is true when the `when` runs | the body runs immediately |
| The condition changes from false to true | the body runs |
| The condition stays true | the body does not run again |
| The condition becomes false | the `when` re-arms |
| The condition becomes true again | the body runs again |
| The condition becomes false after being true | the `else` body runs, if there is one |
| The enclosing block ends | the `when` stops watching |

Details:

- **Dependency tracking:** dependencies are found automatically from the variables and [fields](#fields) that the condition reads, including what the [functions it calls](#functions-the-condition-calls) read. A plain variable that a `when` condition or `computed` expression names becomes reactive, so `int[time] delay = 1s` followed by `when delay == 10s do` works without writing `reactive`. The condition must read at least one variable or field; parameters, loop variables, and constants are not watched.
- **`reactive` keeps its initial value:** `reactive bool done = count == 10` evaluates `count == 10` once, at the declaration. Use `computed bool done = count == 10` (or `when count == 10 do`) to follow `count`.
- **Synchronous evaluation:** checks run during the assignment that changes a dependency, before the next statement. Every change is seen; there is no batching.
  - A `when` whose body assigns its own dependencies does not re-run itself while its condition stays true.
- **Computed values update eagerly:** they are recomputed as soon as a dependency changes, so reading one is an ordinary variable read.
  - Assigning to a computed value is a compile error.
  - A computed value is itself reactive, so `when` and other `computed` declarations can depend on it.
- **Assignments that notify:** any assignment to a reactive variable notifies its watchers, including `+=` and the other compound forms, and assignments inside functions, methods, and lambdas.
  - Assigning into a reactive variable (`scores[2] = 5`, `point.x = 1`) notifies it too.
  - Changes made by method calls (`scores.push(5)`), and assignments made in another module (`other.x = 1`, or a field set by a library), are not tracked.
- **Unregistering:** a `when` or `computed` inside a function, loop, `if`, or `try` block unregisters when that block ends (like `defer`). At the top level of a file they stay active for the whole program.

```flywheel
function watch_until(int target) returns void
    reactive int x = 0
    when x == target do
        print "reached", x
    end
    for i in 1..target do
        x = i
    end
end                    # the when stops watching here
```

### `else`

`else do ... end` (or `else run call()`) runs on the opposite change: each time the condition becomes false after it was true. A condition that starts out false does not run it:

```flywheel
reactive int lives = 3
when lives == 0 do
    print "game over"
else do
    print "back in the game"
end

lives = 0          # prints "game over"
lives = 3          # prints "back in the game"

bool lit = false
when lit run lamp.on() else run lamp.off()
```

### Fields

A condition can read fields. `when player.health <= 0 do` is checked again after every assignment to a `health` field, whichever object it belongs to, including `this.health -= n` inside a method. Fields are matched by name, so an assignment to another object's `health` only re-checks the condition and finds it unchanged. Assigning a new object to `player` is seen as well, since `player` itself becomes reactive.

```flywheel
class Player
    int health = 10
    function hit(int n) returns void
        this.health -= n
    end
end

var player = Player()
when player.health <= 0 run print "down"
player.hit(4)
player.hit(7)      # prints "down"
```

### Functions the condition calls

A condition can call functions and methods. The variables and fields they read count as dependencies, following the functions they call in turn: `when touching(goal) do` is re-checked when a top-level variable or a field read by `touching` changes. Top-level variables those functions read become reactive. Methods are matched by name, as fields are. Values that change without an assignment, such as the time or the mouse position, are not seen, and neither are functions from other modules.

```flywheel
var x = 0
var y = 0
function far() returns bool
    return x * x + y * y > 25
end
when far() run print "far away"
x = 3
y = 5              # prints "far away"
```

### Deferred `when`

A `when` is checked during each assignment to a dependency, which can be in the middle of a calculation. `defer when` waits instead. An assignment only marks it pending, and its condition is checked after each statement of the code that follows it in the same block (including the statements in its loops, `if`s, and other blocks, but not inside the functions they call). A function that changes a dependency several times is checked once, when the statement that called it finishes:

```flywheel
var y = 0.0
defer when y < -10 do
    print "fell"
    y = 0
end

function step() returns void
    y = -20        # nothing yet
    y = -5         # back above -10: the dip is never seen
end

while running() do
    step()         # checked here
    move()         # and here
end
```

A game loop at the top level is the usual place for this: `defer when position.y < -12 run respawn()` reacts after the update that moved the player, not halfway through its collision checks. A deferred `when` inside a function is checked between that function's statements and stops watching when its block ends.

## Negated aliases

`bool name/negated = value` declares a bool and a second name for its negation. Both names refer to the same value:

```flywheel
bool enabled/disabled = true
print enabled, disabled     # true false
disabled = true             # stores enabled = false
print enabled, disabled     # false true
```

- **Reading:** reading `disabled` gives `not enabled`.
- **Assigning:** assigning `disabled = x` stores `enabled = not x`.
- **Scope:** both names share the declaration's scope and lifetime.
- **Types:** negated aliases are only for `bool`.
- **Passing to functions:** passing an alias to a function passes its current value.
- **Restrictions:**
  - Compound assignment (`disabled += ...`) is rejected.
  - Aliases cannot be fields or parameters.
  - A declaration has exactly two names.

Aliases combine with `reactive`. A `when` on the alias watches the underlying variable:

```flywheel
reactive bool playerAlive/playerDead = true

when playerDead do
    print "Game over!"
end

playerAlive = false    # prints "Game over!"
```

## Implementation

The compiler lowers these features before type checking (see `src/lower.cpp`):

- **`reactive` variables:** each one gets a hidden list of watcher closures. An assignment to the variable then calls each watcher.
- **`when`:** becomes a closure holding the condition, an armed flag, and the `else` body. It is added to the watcher list of each dependency and called once when the `when` statement runs.
- **Fields:** each field that a condition reads gets a watcher list of its own (`__fwr_field_health`), declared at the start of the program. Every assignment to a field with that name runs it.
- **Called functions:** the lowering reads the bodies of the functions and methods a condition calls (and of the functions those call) and adds the top-level variables and fields they read to the dependencies.
- **`defer when`:** the closure on the watcher lists only sets a pending flag. Each later statement of the block is followed by a check of the flag, which runs the `when` closure.
- **`computed` values:** a computed value is a reactive variable plus a watcher that reassigns it.
- **Name resolution:** names are resolved by lexical scope, so a local or parameter that shadows a reactive variable is an ordinary variable.
