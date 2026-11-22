# The avatar: default, or your own Muse's

The Muse screen shows an animated character on a 64x64 pixel grid. It idles,
listens and speaks during calls, thinks while Muse works, and hops when you pet
it. You choose what it is:

- **The default avatar:** the SDK's own character. No setup needed.
- **Your avatar:** your Muse's real avatar animation (the one in the Muse app),
  made from the video clips Muse keeps of it, so the gadget looks like *your* Muse.

Either way it's compiled into the firmware. Switching takes a rebuild and a
reflash, and you can always switch back.

```sh
scripts/05_avatar.sh status         # which avatar the next build uses
scripts/05_avatar.sh default        # use the SDK's default avatar
scripts/05_avatar.sh list           # your saved avatars
scripts/05_avatar.sh use my-avatar  # use a saved one again
```

Then `scripts/02_build.sh` and `scripts/04_flash_muse.sh --reflash`. The build
prints which avatar it's using.

Behind the scenes the SDK builds `muse-gadget-sdk/esp32/components/muse/avatar/muse_pixel.c`
in place of its default when that file exists. `default` moves it aside, and
`use` copies one of yours in. Your avatars are kept in `avatars/` (not committed),
so switching never loses one.

## Making your avatar from its clips

### 1. Ask Muse for its clips

Muse keeps its avatar as short MP4 loops on its cloud VM: idle, waking, working, a
celebration, and so on, each with a dark-mode version. Ask Muse to package them:

```sh
scripts/05_avatar.sh ask            # sends the request through the board (it must be paired)
scripts/05_avatar.sh ask --print    # or print it, and paste it into the Muse app yourself
```

The request is [`gif-avatar/prompts/01_export_clips.md`](../gif-avatar/prompts/01_export_clips.md).
Muse copies its dark-mode clips (the dark stage keys out cleanly on the black
screen), zips them as `avatar-clips.zip`, and **attaches the zip to your chat in
the Muse app**. Save it from your phone to the Mac (AirDrop, or Files then iCloud).

### 2. Optional: speaking and listening clips

Avatars usually have no clips of talking or listening. Without them, the call
screen shows the idle clip bouncing with Muse's voice plus a ring effect, which
looks fine. For the real thing, ask Muse to generate them:

```sh
scripts/05_avatar.sh ask --call-clips
```

Muse makes `speaking.mp4` and `listening.mp4` to match its other clips and attaches
`call-clips.zip` ([prompt](../gif-avatar/prompts/02_call_clips.md)).

### 3. Make the avatar

```sh
scripts/05_avatar.sh make ~/Downloads/avatar-clips.zip ~/Downloads/call-clips.zip \
    --id my-avatar --name "a short description of your avatar"
```

This:

1. Extracts the media files (only those, by name) into `avatar-clips/<id>-<time>/`.
2. Converts them with [`gif-avatar/gif2pixel.py`](../gif-avatar/gif2pixel.py): finds
   and removes the stage colour, crops, fits each clip to 64x64, builds one shared
   255-colour palette, and packs the frames (PackBits where it's smaller). If the
   frames are over the budget (512 KB by default), it lowers every clip's frame rate
   until they fit.
3. Compiles and runs the result on the Mac with sanitizers
   ([`gif-avatar/host_check.sh`](../gif-avatar/host_check.sh)), the same checks the
   SDK's avatar tool uses.
4. Saves `avatars/<id>.c`, plus 5x preview GIFs of every animation in
   `avatars/<id>-preview/`, and makes it the avatar for the next build.

Look at the previews, then build and reflash.

### Which clip plays when

| Board state | Clip (first found) | Without one |
|---|---|---|
| idle | `idle` (required) | |
| boot | `boot`, `waking`, `connecting` (plays once) | idle fading in |
| listening | `listening` | idle with expanding rings |
| speaking | `speaking` | idle bouncing with the voice |
| thinking / working | `thinking` and `working` (each stands in for the other) | idle |
| petted | `happy`, `milestone_level_up`, `making_something` | a hop |
| error | `error` | idle tinted red, shaking |
| off | `off` (plays once) | idle dimming |

### Tuning

- **Per-clip framing and frame rate:** [`gif-avatar/clips.json`](../gif-avatar/clips.json)
  (`crop`: `full`, `square`, `top-square`, `auto` or `X,Y,W,H`; `zoom`, `anchor`, `fps`,
  `range`).
- **Size vs smoothness:** `--budget-kb 768` gives smoother animation, `--budget-kb 384`
  smaller firmware. Every KB of avatar also costs a KB of PSRAM at run time
  ([memory.md](memory.md)), so 512 is a good balance with the apps.
- **Anything else:** options after `--` go to `gif2pixel.py`, for example
  `-- --crop auto --margin 2` or `-- --speak frames`. Run
  `toolchain/tools-venv/bin/python gif-avatar/gif2pixel.py --help` for all of them.

The same folder of clips always gives the same avatar, so keep your zips if you
want to rebuild it later.
