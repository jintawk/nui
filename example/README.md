# The example

A complete, minimal nui module, and the offline renderer that drew every picture in the top-level
README.

| File | What |
|---|---|
| `DemoModule.cpp` | the whole native side of an addon: 20 lines |
| `exports.def` | names the one exported function |
| `build.sh` | the build, which is one compiler invocation |
| `layer.lua` | a small helper that turns drawing commands into method calls. Copy it into your addon and change it |
| `scenes.lua` | the pictures in the README, as drawing code |
| `render.lua` | draws the scenes offline and writes the back buffer out |
| `perf.lua` | what a layer costs to build and to draw, per thread |
| `raw_to_png.py`, `make_gif.py` | turn what `render.lua` wrote into PNGs and GIFs |
| `media/logo.png` | a picture to draw, so the picture demo has something to show |

## Building the module

```sh
sh example/build.sh                   # -> example/out/_nuidemo.dll
NUI_GXX=/path/to/i686-w64-mingw32-g++ sh example/build.sh
```

In a real addon, `NUI_SRC` points at your vendored `plugin/nui` instead of `../src`, and the output
goes to your addon's `libs/`. Nothing else about the build differs.

## Rendering offline

The renderer makes a real Direct3D 8 device on a hidden window, loads a copy of FFXiMain so that
its `EndScene` wrapper is there to be hooked, and pushes frames through it. The same library code
that runs in game runs here, so what comes back is what the game would show.

You need:

| Variable | Points at |
|---|---|
| `NUI_TEST_HOST` | the directory holding `cr_testhost.dll`, which `test/build_test.sh` builds |
| `NUI_FFXI_COPY` | an **unpacked** copy of `FFXiMain.dll`. The shipped DLL has its code section encrypted and only unpacks itself when loaded, so a straight copy will not do. Never point this at the DLL in your install; it gets loaded |
| `NUI_FFXI_DIR` | your Final Fantasy XI install directory, read only, for the fonts, window art and item icons |
| `NUI_DEMO_DLL` | the module, if it is not at `example/out/_nuidemo.dll` |
| `NUI_BG` | optional. A 3840 by 2160 raw BGRA image to draw over, so you can judge a window against a real scene instead of a flat colour |

Then:

```sh
lua example/render.lua out                    # every scene
lua example/render.lua out hud fonts          # just those two
python example/raw_to_png.py out
python example/make_gif.py out cycle 1000     # a frame sequence -> a GIF, 1000 ms a frame
python example/make_gif.py out live 90
```

`render.lua` prints the frame count and the worst draw time per scene, which is a quick way to see
what a layout is costing before you put it in front of the game.

## Measuring

```sh
lua example/perf.lua 600 hud
```

Builds the scene's layer six hundred times, then draws six hundred frames without changing it,
then times the Lua that produces the command text on its own. It reports each separately, because
only one of them runs on the thread the game waits on. The numbers in the top-level README's
"What it costs" came from exactly this.

## Writing your own scene

A scene is a name, the rectangle of the back buffer to write out, and a draw function:

```lua
shot {
    name = 'mywindow', rect = {24, 24, 624, 214},
    draw = function(b, ctx)
        b:window(40, 40, 560, 150, 'My addon')
        b:text(60, 78, 'Hello', 'FFFFFFFF')
    end,
}
```

Add `frames = 48` and the draw function is called with a frame number and each frame written out,
ready for `make_gif.py`. Add `wintype = 3`, or a function of the frame number, to draw in a
particular window style.

The draw function is handed the same builder your addon would use in game, so a scene file is a
real layout, not a mock-up of one. That is the point: the way to find out what a window looks like
is to look at it, and you should not have to log in to do that.
