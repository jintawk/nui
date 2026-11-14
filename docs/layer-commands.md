# Layer commands

A layer is a string. One command per line, fields separated by **tab** characters, lines separated
by `\n`. A line starting with `#` is a comment. A blank line is ignored.

`nui.layer(id, z, text)` hands one to the library. It is parsed once, turned into vertices, and
replayed every frame until you send a different one for that id. `nui.layer(id)` removes it. There
are 16 layers, drawn back to front by `z`.

Numbers are decimal, and whole pixels unless you want blurred art. Colours are `RRGGBBAA` hex, so
opaque white is `FFFFFFFF` and half-transparent white is `FFFFFF80`.

## The commands

| Command | Fields | What it draws |
|---|---|---|
| `W` | `x  y  w  h  title  [focus]` | A window frame in the current window style, assembled to any size. `title` may be empty. `focus` `1` draws the brighter variant the client uses for the window you are typing in. |
| `T` | `x  y  colour  text` | Body text in the client's chat and menu font, 1:1, drawn additively exactly as the client draws its own. |
| `H` | `x  y  px  colour  text` | The client's nameplate serif. `px` is cap height, and the glyphs scale smoothly, so this one is for headings. |
| `M` | `x  y  colour  text` | The client's window-title font. `y` is the **window's top edge**: the glyphs are placed on the frame the way the client places its own window titles. `W` already does this for its `title` field; `M` is for when you want one somewhere else. |
| `R` | `x0  y0  x1  y1  colour` | A filled rectangle. |
| `Q` | `h  dx0  dy0  dx1  dy1  sx0  sy0  sx1  sy1  colour  filter` | Part of texture handle `h`, from source texels `s*` to destination pixels `d*`. The colour multiplies the texels, so it tints, fades or both. |
| `I` | `x  y  size  item_id  colour  [2]` | An item icon, cut from the client's item DATs on first use. The client's own cell size is 32. A trailing `2` draws it greyed. |
| `G` | `x  y  w  h  frac  colour` | An HP/MP/TP gauge from the client's gauge art. `frac` is 0 to 1; the colour tints the fill, not the groove or the end caps. A height near 7 matches the client at 1:1. |
| `C` | `x0  y0  x1  y1` | Push a clip rectangle. Clips nest. |
| `c` | | Pop the innermost clip. |

### `Q` filter values

| | |
|---|---|
| `0` | point sampling, for pixel art and anything at 1:1 |
| `1` | linear, for anything scaled |
| `2` | point, desaturated |
| `3` | linear, desaturated |

Desaturation is Rec. 601 luma, done by the fixed-function pipeline, then multiplied by the vertex
colour, so you can grey something and tint it in one quad.

### Texture handles

| Handle | What |
|---|---|
| `0` | the atlas of game art: window frames, the three fonts, the gauge |
| `2` and up | pictures you loaded with `tex_load()`, in load order |
| anything else | item icons, which you should draw with `I` rather than by handle |

## An example

```
# the window, then its contents
W	40	40	560	150	Inventory
T	60	78	FFFFFFFF	Hi-Potion
T	520	78	A8B0BCFF	x12
I	540	74	24	4116	FFFFFFFF
G	60	100	300	9	0.82	7CD97CFF
C	56	120	544	180
T	60	130	FFFFFFFF	clipped to the rectangle above
c
```

Building that by hand gets old quickly. [`example/layer.lua`](../example/layer.lua) is a hundred
lines that turns it into method calls, and is meant to be copied into your addon and changed.

## Cost

A command costs roughly one quad, except `W`, which costs a handful plus one per 128 pixels of body
tile, and `T`/`H`/`M`, which cost one per character.

After parsing, nui reorders the quads into as few batches as their overlaps allow, so a layer that
alternates between text and rectangles does not alternate between draw calls. The HUD screenshot in
the README is 1,046 quads in 18 batches. `status().quads` and `status().batches` tell you what your
own layer came to.
