# The Lua module

`require('_youraddon')` returns a table with the functions below. They are the whole API; there is
no global state to configure and nothing else to call.

Everything here runs on Windower's Lua thread, which the game thread waits on. Nothing in this API
blocks on the render thread or on disk, with the exceptions called out below.

## Lifecycle

### `start([ffxi_dir [, flags]]) -> ok, msg [, retry_ms]`

Reads the game's art, then joins the hook.

- `ok == true`: you are in. Nothing is drawn until you send a layer and a frame runs.
- `ok == false` **with** `retry_ms`: no module has found the hook yet. This is normal at load time,
  before the client's renderer has been through `EndScene`. Call `start` again after `retry_ms`
  milliseconds. Most addons do this from `prerender` with a five second timer, say so once, and
  stay quiet after.
- `ok == false` **without** `retry_ms`: final. Print `msg` and do not retry.

`ffxi_dir` overrides where the install is looked for; leave it out in game. `flags` is a string
that may contain:

| Flag | Effect |
|---|---|
| `noshare` | keep this module's own textures instead of the shared cache's. The pixels are identical; it costs memory. An escape hatch, worth exposing as a setting. |
| `nosig` | search for a deliberately wrong signature. The fail-closed test: `start` must fail with a retry. |

### `stop() -> "stopped"`

Leave the hook and the cache, release every texture and layer. Call it from your addon's unload
handler, **before** you save anything, so the game is not drawing while your addon winds down.

### `version() -> string`

The version string from your `HostConfig`.

## Drawing

### `screen(w, h [, auto [, f]])`

The coordinate space your Lua draws in. Pass the client's resolution for 1:1 pixels.

### `layer(id, z, text)` and `layer(id)`

Set or remove one of 16 layers. `z` orders them back to front. `text` is the command string
documented in [layer-commands.md](layer-commands.md). Returns the command count, or `nil, err` with
the line number when a command is malformed.

### `layer_pos(id, ox, oy [, x0, y0, x1, y1])`

Move a layer by an offset, and optionally clip it, without re-sending its commands. This is how you
scroll: the commands are already parsed, so moving them is close to free.

## Textures

### `tex_load(path [, max_w, max_h]) -> handle, w, h`

Load a PNG or JPEG, downscaled to fit the bound you give. The file is read and hashed, then looked
for in the shared cache: if another addon already has those bytes, nothing is decoded at all.
Otherwise it decodes on a thread of this module's own and is drawn from the first frame after the
pixels arrive, so a load never stalls the game. A file that does not decode is simply never drawn;
`prof().decode_failed` counts those.

### `tex_free(handle)`

Drop a picture. The texture goes when the last holder lets go.

### `icon(item_id) -> boolean` and `icon_pixels(item_id) -> string`

Whether the client has an icon for that item, and its 32 by 32 BGRA pixels as a 4,096-byte string
if you want to do something else with them. Drawing one is the `I` command; you do not need either
of these for that.

## Layout

### `metrics() -> table`

| Field | What |
|---|---|
| `moji` | 96 advance widths, ASCII 0x20 to 0x7F, for the body font |
| `title` | per-glyph `{w, h, top}` for the nameplate serif |
| `wtitle` | advances for the window-title font |
| `from_game` | false when the client's own width table could not be read and fallbacks are in use |
| `title_font` | false when the nameplate serif is not available |

These are the client's own numbers, so a string you measure measures the same in game.

## The window style

### `wintype(n)`

Draw in window style `n`, 1 to 8.

### `wtread() -> type, source`

The style the player has chosen, read from the client's config table. `source` is `"config"` or
`"watch"`. Call it when you load and when the player might have changed it, and pass the result to
`wintype`.

### `wtfind(value [, restart])` and `wtwatch(rva, enc)`

A fallback scanner for a future client where the config table has moved: ask the player what style
they are using, call `wtfind` with it, and `wtread` uses what it learns. Most addons never need
these.

## Diagnostics

### `status() -> table`

The table worth putting behind a `//youraddon status` command.

| Field | What |
|---|---|
| `running`, `message` | whether it is drawing, and why not |
| `hook`, `hook_state`, `describe` | the shared hook: `live`, `waiting`, `blocked` or `no bus`, and one line a player can read out to you |
| `owner`, `owner_rev`, `rev`, `clients`, `slot`, `site` | who drives the hook, who else is on it, where it is patched |
| `drawing` | running, and not held back by a texture upload that failed and is being retried |
| `device_ok`, `calls`, `drawn`, `uploads` | the renderer |
| `layers`, `quads`, `batches` | what your last layer came to |
| `last_us`, `max_us`, `build_us` | time in the draw and in the build |
| `cache` | `shared`, `why`, `shared_textures`, `private_textures`, `hits`, `creates`, `live_kb`, `clients`, and more |
| `native_errors`, `pending_releases`, `final_render` | counters that should stay at zero or near it |

### `prof() -> table`

Counters since the last call, then reset. Render thread, Lua thread and decode thread are reported
separately: lock waits, build times, draws, uploads, file reads, decode failures. Reading it costs
nothing between calls, so it is safe to poll.

### `us() -> number`

Microseconds since the module was opened, from the performance counter. Useful because Windower's
own clock is not fine enough to measure a frame.
