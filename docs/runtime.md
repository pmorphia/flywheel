# Runtime and ownership

LLVM values hold primitive numbers, booleans, chars, and enum values directly. Immutable UTF-8 strings, dynamic arrays, tuples, maps, sets, class instances, Results, boxed optional primitives, JSON documents, errors, closures, markup nodes, and native resources (files, sockets, threads) are heap objects with reference counts (updated with atomic instructions only while other threads run). Object slots, map keys, and element children retain references. Assigning a collection or instance aliases it; mutations are visible through both bindings.

Generated functions establish an ownership frame. Reference locals and parameters have initialized owning roots. Stores retain the new reference before releasing the previous value. Lexical scope exit clears local roots, including exits through exceptions, break, and continue. Temporary references remain alive through their statement and are drained at safe boundaries. A conservative analysis skips temporary retains for reads in expressions containing only known non-mutating operations.

`for` evaluates the iterable once. Array iteration captures the initial length; map and set iteration uses a key snapshot. Return values and pending errors have protected roots while `finally` runs.

Native C++ failures are caught at runtime C ABI boundaries and recorded in a thread-local typed error channel. Generated code checks the channel after fallible native and generated calls. C++ exceptions do not unwind through generated LLVM functions. Uncaught errors report to stderr and return exit status 1.

## Cycle collection

Reference counting frees most objects immediately. Unreachable reference cycles are found by a synchronous cycle collector (trial deletion, as in Bacon and Rajan). Containers whose count is decremented without reaching zero become *possible roots*; the collector subtracts internal references among them and frees groups that nothing outside the group references. References held by variables, frames, and native owners (tasks, channels, web routes, windows) count as outside references, so the collector is conservative.

Collection runs automatically at statement boundaries once enough possible roots accumulate, after the program body finishes (so cycles held by globals are reclaimed at exit), and on demand with `gc.collect()`, which returns the number of objects freed. `gc.pending()` reports the current number of possible roots. Collection only runs while no spawned threads or web workers are active; it resumes when they finish. `weak<T>` remains available for back links that should not keep their target alive.

Instances that cannot be part of a cycle are never candidates and are never traced: struct values stored on the heap, and instances of classes whose fields are all numbers, bools, chars, enums, strings, and structs (or nullable ones). Arrays of such objects cost the collector nothing.

Programs that cannot afford a pause at an arbitrary statement (games, audio, animation) control when collection happens:

| Function | Effect |
|---|---|
| `gc.collect()` | collect now; returns the number of objects freed |
| `gc.step(seconds)` | collect for about that long at most (`gc.step(0.001)`, `gc.step(1ms)`), in small batches of possible roots; returns the number freed. Call it once per frame |
| `gc.pause()` / `gc.resume()` | stop and restart automatic collection (calls nest); `gc.collect` and `gc.step` still work while paused |
| `gc.paused()` | whether automatic collection is paused |
| `gc.pending()` | the number of possible roots waiting |
| `gc.threshold()` / `gc.threshold(n)` | how many possible roots trigger an automatic collection (default 20000); setting it returns the previous value |

A game typically sets a step budget each frame, or pauses during a match and collects between rounds:

```flywheel
import gc
gc.pause()          # the match: no collection pauses
play_round()
gc.resume()
gc.collect()        # between rounds, while the score screen shows
```

## Deep copies

Assigning a class instance or collection aliases it. `deep_copy(value)` instead copies everything the value reaches, in one call, keeping its shape: an object reached twice is copied once, so shared references stay shared and cycles stay cycles inside the copy. It is meant for saving a whole state and going back to it: rollback netcode, replays, undo, and training-mode resets.

```flywheel
var saved = deep_copy(match)   # match: a class holding fighters, projectiles, a random.Generator, ...
...
match = deep_copy(saved)       # back to that moment; `saved` stays as it was
```

Numbers, bools, chars, enums, and structs are values already and come back as they are. Arrays, maps, sets, tuples, class instances, Results, boxed optionals, errors, and `json.value` documents are copied; flat arrays of structs are copied as one block. Some things are shared rather than copied, because a second copy would be wrong: strings (immutable), closures, markup elements, windows, native resources (files, sockets, threads), and instances of classes with a `destructor` (they own something outside the program, such as a texture, that two instances would free twice). A `weak` reference to an object that was copied points at its copy. A frozen collection stays frozen in the copy. Copy while no other thread changes the state.

`weak<T>` holds a non-owning reference to a class instance, element, or other managed reference type. Assigning a `T` (or calling `weak(x)`) creates one; `get()` returns `T?` and `alive()` reports whether the target still exists. Each referenced object owns a shared cell that is cleared when the object is destroyed, under a lock, and `get()` only takes a reference while the target's count is above zero, so weak reads are safe across threads.

## Closures and captures

Closures are ARC-managed objects holding a code pointer and their captured values in ordinary slots. A named function used as a value is wrapped in a capture-free closure; a bound method (`obj.method`) captures its receiver. Locals that are both captured by a lambda and assigned live in a shared one-slot cell, so assignments are visible to the declaring scope and every closure; other captures are copied.

## Threads

`thread.spawn` runs a closure on a native thread with its own ownership frames and error channel; see [concurrency](concurrency.md). Reference counts and the runtime's internal tables are thread-safe. While spawned threads run, each array, map, set, field, and global operation takes a lock chosen by the object's address (skipped when only one thread runs); sequences of operations still need `thread.mutex` or `thread.channel`. See [shared state](concurrency.md#shared-state) for the exact guarantees.

## Markup, processes, and windows

Markup literals are constructed by compiled native calls; `html.parse` builds the same trees from HTML text at run time. Fragments preserve their sequence of children and serialize without a wrapper. Children keep a weak link to their most recent parent for `.parent`, siblings, and `closest`. Function-valued event attributes are stored on elements as retained closures.

Managed slots carry a compact value-kind tag for recursive formatting, JSON encoding, and checked collection conversion. Classes include compiler-generated field-name/type identity metadata; each class also has a unique identity used by dynamic dispatch, `is`, and `match`.

Child processes run synchronously. `os.run` uses `fork`/`execvp` with pipes and `poll` on POSIX and `CreateProcessW` with pipes and reader threads on Windows. Desktop windows keep their loaded tree and re-render it after callbacks; native GUI dependencies are separate from the core runtime and loaded only by `run`.

`testing.live_objects()` reports the current number of live managed objects, enabling lifetime regression checks. AddressSanitizer/UBSan tests exercise standalone programs, imported projects, threads, and cycle collection.
