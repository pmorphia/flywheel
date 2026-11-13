# Desktop windows

The native `desktop` library displays Flywheel markup in a window, using GTK/WebKitGTK directly on Linux and WebView2 (through [webview/webview](https://github.com/webview/webview)) on Windows. It does not run a local HTTP server or require Python. Windows can run Flywheel event handlers, expose Flywheel functions to page JavaScript, update live, and run several at once.

```flywheel
import desktop
import html.*

int count = 0
var window = desktop.window("Counter", 480, 320)

function page() returns element
    return <html><body>
        <p id="count">Clicked {count} times</p>
        <button onclick={function()
            count += 1
            window.load(page())
        end}>Click me</button>
    </body></html>
end

window.load(page())
window.run()
```

| API | Behavior |
|---|---|
| `desktop.window()` | Create a window with title `Flywheel`, width `960`, height `640` |
| `desktop.window(string title, int width, int height)` | Customize title and size; trailing arguments are optional; `desktop.window "Title"` also works |
| `window.load(element_or_fragment)` | Show this tree; while running, the page shows it after the current callback |
| `window.bind(string name, function(string) returns string)` | Expose a Flywheel function to page JavaScript as `name(value)` |
| `window.eval(string script)` | Run JavaScript in the page: after loading when called before `run`, otherwise after the current callback |
| `window.on_frame(function(double elapsed))` | Call a function once per display frame while the window runs, with the seconds since the previous frame; `window.on_frame(null)` stops |
| `window.run()` | Show the window and block until it closes |
| `desktop.run(desktop.window[] windows)` | Show several windows with one event loop; returns when all are closed |
| `desktop.quit()` | Close every window and return from `run` after the current callback |

`desktop.window` without parentheses is the class itself (`var Window = desktop.window`, then `Window("Title")`); see [classes and constructors](types.md#classes-and-constructors). `desktop.Window` is an alias. Windows are reference-managed objects: assignments alias the same window.

For a single page, `flywheel desktop app.fw` shows the `element` or `fragment` the file returns at top level, without creating the window in code; see [pages](tooling.md#pages-desktop-and-serve).

## Events and live updates

Markup event attributes can hold Flywheel functions: `function() returns void`, or `function(string value) returns void`, which receives the key of a keyboard event (`"a"`, `"Enter"`, `"ArrowLeft"`) and otherwise the event target's `value` (for example the text of an `<input>`). Inline JavaScript strings (`onclick="..."`) still work too.

A window keeps the tree it was given. After every event handler, binding call, and frame callback, each running window re-renders its tree; when the HTML changed, the page is changed **in place**: elements that are still there are kept, with their focus, scroll position, text selection, and running CSS animations, and only the attributes, text, and children that differ are touched. Children are matched by their `data-key` attribute or `id` when they have one (so a reordered list moves its items instead of rewriting them), and by position and tag otherwise. Text a user typed into an input is kept unless the program changes that input's `value`. So a handler can mutate the tree in place (`node.text_content = ...`) or call `window.load(new_tree)`. Changes made before `run` are shown when the window opens.

## Animation

`window.on_frame` runs a function once per display frame, for animation and simple games drawn with markup, SVG, or a canvas:

```flywheel
import desktop
import svg.*
import html.*

double angle = 0.0
var window = desktop.window("Clock hand", 320, 320)
function page() returns element
    return <html><body>
        <svg width="300" height="300" viewBox="-150 -150 300 300">
            <circle r="140" fill="none" stroke="gray" />
            <line x1="0" y1="0" x2="0" y2="-120" stroke="tomato" stroke-width="6"
                  transform="rotate({angle})" />
        </svg>
    </body></html>
end
window.load(page())
window.on_frame(function(double elapsed)
    angle += 90 * elapsed          # a quarter turn per second
    window.load(page())
end)
window.run()
```

The next frame is requested when the callback returns, so slow frames never pile up, and the page changes in place. The callback may be set before `run` or while the window runs. For games that need a real renderer, sound, and gamepads, use the [Aperture](../aperture/README.md) package instead of a window of markup.

## JavaScript bindings

```flywheel
import desktop
var window = desktop.window()
window.bind("greet", function(string name) returns string
    return "Hello, " + name
end)
window.eval("greet('Ann').then(function (reply) { document.title = reply; })")
```

Page JavaScript calls a binding with one argument and receives a Promise. A single string argument arrives unchanged; other arguments arrive as their JSON text. The returned string resolves the promise. Bindings must be added before `run`. Braces with whitespace beside them, like the JavaScript block above, stay literal in Flywheel strings; `{name}` with no space interpolates.

Handlers and bindings run on the thread that called `run`, one at a time. An uncaught Flywheel error in a callback closes the windows and propagates from `run` as an ordinary error.

## Errors and lifetime

Dimensions must be positive signed 32-bit values. Titles and rendered HTML cannot contain NUL bytes. Invalid values, a missing backend, a missing display/WebView2 runtime, or native webview failures raise `DesktopError`. Backend modules stay loaded until process exit because GUI frameworks register process-wide types. On Linux, relative resource URLs such as `<img src="image.jpg">` resolve against the working directory, as the program's own file paths do; on Windows they have no filesystem base, so use absolute URLs there.

## Build and run

```sh
cmake -S . -B build -DFLYWHEEL_DESKTOP=ON
cmake --build build --parallel 2
build/flywheel run examples/desktop_demo.fw
```

The `FLYWHEEL_DESKTOP` CMake setting accepts `AUTO` (default), `ON`, or `OFF`. `AUTO` builds the backend when native development dependencies are available. `ON` makes missing build dependencies a configuration error. `OFF` retains the window API, but `run` raises `DesktopError` unless a compatible backend is provided. Command-line programs do not load the GUI backend.

- **Linux:** install GTK and WebKitGTK development packages; the backend (`runtime/desktop_gtk.cpp`) uses them directly and the build downloads nothing. The first of `webkit2gtk-4.1` (GTK 3), `webkitgtk-6.0` (GTK 4), or `webkit2gtk-4.0` found by pkg-config is used. Debian/Ubuntu: `libgtk-3-dev libwebkit2gtk-4.1-dev`; Arch Linux: `gtk3 webkit2gtk-4.1`. Running a window needs a graphical session.
- **Windows:** use the native C++ toolchain. The build downloads webview **0.12.0** from its official source archive, verifies its SHA-256, and its CMake obtains the WebView2 SDK; for an offline build, provide an existing checkout with `-DFETCHCONTENT_SOURCE_DIR_WEBVIEW=/path/to/webview`. End users need the WebView2 runtime installed. This path is untested.

`cmake --install build --prefix /chosen/prefix` installs the backend beside the runtime archive in `lib`. The compiler records its path in generated applications; `FLYWHEEL_DESKTOP_BACKEND` overrides it. The backend exports the versioned entry point `fw_desktop_run_v2`, separate from the core runtime ABI.

## Validation

The ordinary regression suite tests constructors, aliases, ownership, argument diagnostics, and failed-backend cleanup without opening a window. On Linux, CMake also registers a real GUI test when `xvfb-run`, `xdotool`, and `dbus-run-session` are installed (`ctest --test-dir build --output-on-failure`). It reads the rendered DOM, checks escaping and live trees, reruns a window, clicks buttons and types into inputs from JavaScript, checks that Flywheel handlers ran and the page re-rendered, calls bindings in two windows at once, and quits from Flywheel.
