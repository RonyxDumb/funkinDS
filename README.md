# Friday Night Funkin' DS

An unofficial Nintendo DS port of [Friday Night Funkin'](https://github.com/FunkinCrew/Funkin) (V-Slice), built with the same approach as [twiddleDS](https://github.com/RonyxDumb/twiddleDS) and [olbDS](https://github.com/RonyxDumb/olbDS): this is not HaxeFlixel running on a DS. The game was rewritten in C with libnds, following the V-Slice source (PlayState, Strumline, Conductor, BaseCharacter / Bopper, Stage, the song events and the stage scripts), and uses the original game data and art converted for the hardware.

## What is ported

- **Tutorial, Weeks 1–7 and Weekend 1** (Darnell, Lit Up, 2hot, Blazin'), plus the extra song Spaghetti.
- **Erect remixes** (Erect and Nightmare difficulties), **Pico Mixes** and the **Boyfriend mixes** of Weekend 1: every song variation in the game, with its own chart, audio, characters and stage.
- **Story mode** (week characters, title and difficulty, week scores), **Freeplay** with the **Boyfriend / Pico DJ**, **character select** (Boyfriend or Pico, each with their own songs), **results screen** with the V-Slice ranks, **options**, **pause** and **game over**.
- **V-Slice gameplay**: PBOT1 scoring and judgements, health and hold-note rules, the countdown, rating and combo popups, note splashes, the health bar with icons, and the `FocusCamera` / `ZoomCamera` / `SetCameraBop` / `PlayAnimation` / `ScrollSpeed` events.
- **Note kinds** are read from the character scripts: Mom's `-alt` lines, Tankman's "ugh" and "heh, pretty good", Darnell's cans and Pico's gun in 2hot, and the whole Blazin' fight (punches, blocks, dodges, uppercuts) with its centered strumline.
- **Characters and stages** come from the game JSON data. Adobe Animate texture atlases (most V-Slice characters, the DJs, the character select sprites, Nene's A-Bot speaker) are flattened into frames by `tools/animate.py`, which follows flixel-animate.
- **Stage scripts**: lightning in the Spooky Mansion, the Philly train and window lights, the speeding car on the limo, and the dancing props.
- **Week 6 dialogue** (Senpai, Roses, Thorns) and the **Weekend 1 video cutscenes** in story mode.
- **Pixel note style** for Week 6.

Not ported: the Week 7 and Week 5 in-engine cutscenes, stage shaders (rain, lighting) and blend-mode overlays, the animated freeplay capsules and the sticker transitions.

## Assets

The Friday Night Funkin' source code is Apache 2.0, but the game assets (art, music, charts, sounds) are not redistributable: see the [Funkin.assets license](https://github.com/FunkinCrew/funkin.assets/blob/main/LICENSE.md). **This repository therefore contains no game assets and no prebuilt ROM.** You build the ROM yourself from your own copy of the assets.

## Building

You need Python 3 with Pillow and NumPy, ffmpeg, a C compiler, and [devkitPro](https://devkitpro.org) with `nds-dev` (or Docker).

```sh
git clone --depth 1 https://github.com/FunkinCrew/Funkin.assets funkin-assets
./build.sh funkin-assets            # converts the assets, then builds FunkinDS.nds
```

`tools/convert.py <Funkin.assets>` writes `nitrofiles/`, `maxmod_data/`, `source/gen_assets.c` and `include/gen_assets.h`; `make` (or `./build.sh` with no arguments) builds the ROM. The ROM is about 200 MB (songs are IMA ADPCM streams, the cutscenes 256x192 at 12 fps), so it needs a flashcart or melonDS.

## Playing

Copy `FunkinDS.nds` to your flashcart (TWiLight Menu++ / nds-bootstrap) or open it in **melonDS**.

| Action | Buttons |
| --- | --- |
| Notes | D-pad or Y / B / X / A (left, down, up, right) |
| Pause | START |
| Menus | D-pad, A / START = accept, B = back, L / R = difficulty, or the touch screen |
| Character select (freeplay) | SELECT |
| Dialogue / cutscenes | A = next, START = skip |

The gameplay is on the top screen; the bottom screen shows the score, misses, accuracy and judgements, and the pause menu. Scores and options are saved to `/data/funkinds.sav` on the SD card.

## How it fits on a DS

- **Characters** are stored as 8 bpp frames, LZ77-compressed in RAM and decompressed into a VRAM slot when the frame changes.
- **Stages** are cropped to the area the camera can show, and their texture resolution is chosen per stage to fit the VRAM budget (pixel stages keep one texel per pixel).
- The top screen shows the 16:9 view at 80% of a 4:3 crop; the strumlines are laid out for the narrower screen.

## Project structure

```
source/        engine (gfx, audio, tweens, bottom screen), PlayState (play.c),
               characters, stages and stage scripts (stage.c), menus, game over
include/       headers
tools/         convert.py (asset converter), animate.py (Animate atlas flattener),
               adpcm.c / lz10.c (encoders), melonheadless (headless melonDS test frontend)
```

## Credits

Friday Night Funkin' by The Funkin' Crew (ninjamuffin99, PhantomArcade, evilsk8r, Kawai Sprite and contributors). This is an unofficial fan project, not affiliated with or endorsed by The Funkin' Crew.
