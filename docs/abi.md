# The two shared buses

Addons that use nui each compile their own copy of it, which means several copies of the library
can be alive in one game process at once, at different versions, loading and unloading in any
order. Two things cannot be done per copy: patching the client, and holding a texture. Those go
through named shared memory, and their layouts are frozen.

Frozen means the offsets never move. New fields are only ever appended into reserved space with a
revision number, and zero always means "the old behaviour", because the mapping starts zeroed. A
module built against revision 1 works beside a module built against revision 5, in either role.
`test/frozen/` holds the layouts as first released, and `test/frozen_check.py` fails any change to
them that is not an append. `test/layout_*.txt` is regenerated from the headers and compared
against it.

## UiHook 2

`Local\FFXiUiHook_v2_<pid>`, 4,096 bytes, mapped whole.

The client has a 13-byte wrapper around its renderer's `EndScene` call. There can only be one patch
of it, so there can only ever be one of these buses, which is why the ABI is 2 forever and
everything after is a revision within it.

- The first module to arrive finds the wrapper by byte signature and patches it to jump to a thunk
  in shared memory. Every module after that registers a draw callback with the bus already there.
- The thunk uses `pushad`/`popad`, so the client's `EndScene` runs once with its own `this` and its
  return value comes back untouched. With no dispatcher registered the thunk is a pass-through.
- The frame is driven by the live client with the **highest revision**. A newer module takes it
  over when it registers; when it leaves, the frame passes to the highest-revision survivor, or to
  nobody. Whichever module drives, it calls every live client's draw through the one frozen
  signature. That is the entire compatibility contract between versions.
- Joining needs only that somebody has patched. A module whose own signature search fails, because
  the client was updated and only a newer module knows the new shape, still joins and still draws.
- A module leaves in `DllMain(DLL_PROCESS_DETACH)`, which hands the frame on before its code is
  unmapped. It takes no lock and waits on nothing, because that runs under the loader lock.
- As defence in depth, a dispatcher skips and frees a client whose draw function is no longer inside
  its module's mapped image, and a register replaces an owner whose dispatcher has gone.
- If a pre-release ABI 1 patch is already on the wrapper, nui never patches over it. `start()`
  returns `blocked`, with no retry, and nothing is drawn.

The hook is modelled on BroGuyPal's SceneHook, which hooks a different function (inside
`draw_scene`, under the game's UI rather than over it) and uses a different mapping name. The two
never meet, and an addon can use both.

## NuiCache major 1

`Local\FFXiNuiCache_v1_<pid>`.

One `IDirect3DTexture8` per distinct piece of art per process, however many addons draw it.

Entries are keyed by what is in them, never by who asked:

| Kind | Key |
|---|---|
| `NC_ATLAS` | window style 1 to 8, plus a hash of the install folder |
| `NC_ITEM_ICON` | item id, plus a hash of the install folder |
| `NC_PICTURE` | the maximum width and height asked for, plus a 64-bit hash of the file's **contents** |

Each kind also carries a pixel-format version, bumped whenever a change to the library alters a
single output byte of that kind's decode. Two library versions that would decode the same source
differently therefore never share a texture, and `test/golden_fmt.json` fails a build that changes
the pixels without bumping the version.

A change that lets a load succeed where it used to fail also alters the output, even when a stock
install decodes exactly as before. The golden test sees only a stock install, so it cannot catch
that, and the version has to be bumped by hand. Reading HD texture packs is such a change. nui 1.2.10
cannot read their art, so the atlas moved to version 3 with it. Under one version, an addon on each
library would share one atlas, and the one that created it would decide whether the other drew any
window art.

- Textures are created on the render thread, in the managed pool.
- Holder bits track which modules still need an entry. An entry nobody holds survives ten seconds,
  so `//lua reload youraddon` finds its own atlas still there rather than decoding it again, and
  reloading one addon never throws away art another addon is still drawing. Past that, unheld
  entries go oldest first once they are over the budget.
- The `noshare` flag makes a module keep its own textures. Identical pixels, more memory. It exists
  so that a bug in the cache can never be the thing standing between a player and their interface.
- A new cache major would simply not share with this one. There is no negotiation and no fallback
  path to get wrong.

On a scene with 25 distinct textures, a second addon sharing them adds nothing measurable, against
roughly 12 MiB when it holds its own.
