# Friday Night Funkin' 3DS

An unofficial Nintendo 3DS port of [Friday Night Funkin'](https://github.com/FunkinCrew/Funkin) (V-Slice), based on the Nintendo DS port of this repository (the `3ds` branch; the DS version is on the main branch). The game is written in C with libctru and citro3d, following the V-Slice source (PlayState, Strumline, Conductor, BaseCharacter / Bopper, Stage, the song events and the stage scripts), and uses the original game data and art converted for the hardware.

## What is new on the 3DS

- **Graphics at the 3DS resolution**: the whole 16:9 game view on the 400x240 top screen, the real V-Slice HUD layout (both strumlines at full size, health bar, score). Characters, stages and HUD are converted at about twice the DS resolution, in true color with smooth (alpha blended, bilinear filtered) edges instead of 255 colors with 1-bit transparency.
- **Stereoscopic 3D**: every stage layer gets its depth from its scroll factor (the background behind the screen, the characters on it, the HUD in front). The strength can be set in the options (0-200%) and follows the 3D slider.
- **Blend-mode overlays**: the add / multiply lights of the Erect stages and Weekend 1 (stage lights, light maps, Blazin' sky and foreground) that the DS could not draw.
- **Audio**: 32 kHz stereo instrumentals (DS: 22 kHz mono), mixed by the DSP; a thread keeps the music streaming while the game loads.
- **Bottom screen** drawn by the GPU: score, accuracy, combo and judgements, a health bar with the icons, the song time bar, touch buttons in every menu (tap the screen to pause).
- **Cutscenes** at 400x240, 15 fps.
- New options: middlescroll, 3D depth. Extra keys: ZL / ZR for the left / right notes.
- New 3DS: runs at 804 MHz.

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

Not ported: the Week 7 and Week 5 in-engine cutscenes, stage shaders (rain, lighting, color adjustments), the animated freeplay capsules and the sticker transitions.

## Assets

The Friday Night Funkin' source code is Apache 2.0, but the game assets (art, music, charts, sounds) are not redistributable: see the [Funkin.assets license](https://github.com/FunkinCrew/funkin.assets/blob/main/LICENSE.md). **This repository therefore contains no game assets and no prebuilt build.** You build the game yourself from your own copy of the assets.

## Building

### Requirements

- Linux or Windows 10/11 with **WSL2 Ubuntu**.
- Python 3, Pillow, NumPy, FFmpeg, Git and a C compiler.
- **Docker** (recommended): `build.sh` uses the official `devkitpro/devkitarm` image (it includes `3ds-dev`, citro3d and the zlib portlib), so local devkitPro is not required.
- Alternatively, install [devkitPro](https://devkitpro.org) with `3ds-dev` and `3ds-zlib` and build with `make`.

The original game assets are required separately; this repository contains no assets or prebuilt build.

### 1. Install Ubuntu / WSL dependencies

```sh
sudo apt update
sudo apt install -y git python3 python3-pip python3-venv ffmpeg build-essential
```

Install Docker Engine, or Docker Desktop on Windows with **Settings → Resources → WSL Integration** enabled for Ubuntu. Start Docker and verify access:

```sh
docker info
```

If Docker Engine reports `permission denied ... docker.sock`, configure your user (the group must exist):

```sh
sudo usermod -aG docker "$USER"
newgrp docker
docker info
```

On Docker Desktop, first check that the app is running and WSL integration is enabled. Avoid `sudo ./build.sh`: it may create root-owned build artifacts. Docker group access grants elevated privileges.

### 2. Clone the repository

```sh
cd ~
git clone -b 3ds https://github.com/RonyxDumb/funkinDS.git
cd funkinDS
```

Already cloned? Use `cd ~/funkinDS && git pull` instead of cloning again. Back up any local changes before deleting an old checkout.

### 3. Set up Python

```sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install pillow numpy
```

Re-enable the environment with `source .venv/bin/activate` in later terminal sessions.

### 4. Download official game assets

Run these commands inside `~/funkinDS`:

```sh
git clone --depth 1 https://github.com/FunkinCrew/Funkin.assets funkin-assets

# Optional: official mobile app icon for the ROM
git clone --depth 1 --filter=blob:none --sparse https://github.com/FunkinCrew/Funkin.art funkin-art
git -C funkin-art sparse-checkout set icons
```

Without the optional `funkin-art` checkout, skip the `--art` option below; Boyfriend's health icon is used for the Home Menu icon.

### 5. Convert assets

```sh
python3 tools/convert.py funkin-assets --art=funkin-art
```

This creates `romfs/`, `source/gen_assets.c` and `include/gen_assets.h` (about 500 MB, mostly audio). Allow sufficient free disk space.

Options: `--mono` (mono audio, smaller), `--rate=22050` (audio sample rate), `--video-fps=12`, `--no-audio`.

### 6. Build

**Docker (recommended):**

```sh
chmod +x build.sh
./build.sh
```

`./build.sh` without arguments **only builds**; it does not convert the assets again.

**With locally installed devkitPro / 3ds-dev:**

```sh
make
```

For local `make`, ensure `DEVKITARM` and the other devkitPro environment settings are configured.

The output is **`Funkin3DS.3dsx`** at the root of the repository, approximately **500 MB** (the game data is embedded as RomFS). Test it in **Azahar** (or Citra / Lime3DS) or on a 3DS with custom firmware.

### Troubleshooting

- **`destination path 'funkinDS' already exists`**: enter the existing folder with `cd ~/funkinDS` and run `git pull`.
- **Docker socket permission denied**: check `docker info`, running Docker, group membership or Docker Desktop WSL integration as described above.
- **`Permission denied` under `build/`**: earlier root-owned output may be the cause. From the repository root, if `build/` exists, run `sudo chown -R "$USER:$(id -gn)" build` and retry without `sudo`.
- **`No module named PIL` or `numpy`**: `source .venv/bin/activate` then `python3 -m pip install pillow numpy`.
- **`ffmpeg: command not found`**: `sudo apt install ffmpeg`.
- **`Please set DEVKITARM`**: local `make` needs devkitPro configured; alternatively use Docker via `./build.sh`.
- **"Game data not found in RomFS"** on the console: the assets were not converted before building (step 5).
- **Missing or stale in-game assets**: rerun asset conversion before rebuilding.

### GitHub Actions

`.github/workflows/build.yml` builds the game on every push: it downloads Funkin.assets (and the icon from Funkin.art), converts them (the result is cached until the assets or the converter change, the first run takes a while) and uploads `Funkin3DS.3dsx` as a workflow artifact. Since the game assets are not redistributable, the build is never attached to a public release.

## Playing

Copy `Funkin3DS.3dsx` to `/3ds/` on the SD card and start it from the **Homebrew Launcher** (a 3DS with custom firmware, e.g. Luma3DS). Sound needs the DSP firmware dump (`/3ds/dspfirm.cdc`, made by DSP1 once); without it the game runs silently. In **Azahar**, open the `.3dsx` with *File > Load File*.

| Action | Buttons |
| --- | --- |
| Notes | D-pad / Circle Pad, Y / B / X / A, ZL / ZR (left, right) |
| Pause | START, or tap the bottom screen |
| Menus | D-pad, A / START = accept, B = back, L / R = difficulty, or the touch screen |
| Character select (freeplay) | SELECT |
| Dialogue / cutscenes | A = next, START = skip |
| Quit | START + SELECT + L + R, or the HOME menu |

The gameplay is on the top screen; the bottom screen shows the score, misses, accuracy, combo and judgements, the health bar and the time left, and the pause menu. Scores and options are saved to `sdmc:/3ds/funkin3ds/save.dat`.

## How it works on a 3DS

- **Rendering** (`source/gfx.c`): every sprite is a quad appended to a draw list per screen; the lists are drawn with citro3d at the end of the frame, the top screen twice in stereoscopic 3D with a per-vertex depth. Textures are premultiplied alpha, stored by the converter already in the GPU tile order and zlib compressed.
- **Characters** are stored as 8 bpp frames (256 premultiplied RGBA colors per character, LZ77 compressed); a frame is expanded into the character's texture when it changes.
- **Stages** are cropped to the area the camera can show, and their texture resolution is chosen per stage to fit a 9 MB budget (pixel stages keep one texel per pixel, nearest filtering).
- **Bottom screen** (`source/sub.c`): the menus write into a display list (like they drew into a bitmap on the DS) that the GPU redraws every frame.
- **Audio** (`source/audio.c`): IMA ADPCM streams decoded by a thread woken by the DSP, NDSP channels for the music, the voices and the sound effects.

## Project structure

```
source/        engine (gfx + render.v.pica shader, audio, tweens, bottom screen),
               PlayState (play.c), characters, stages and stage scripts (stage.c),
               menus, game over
include/       headers
tools/         convert.py (asset converter), animate.py (Animate atlas flattener),
               adpcm.c / lz10.c (encoders), emutest.sh (records a build running in
               the Azahar emulator, with -DAUTOTEST scripted inputs)
```

## Credits

Friday Night Funkin' by The Funkin' Crew (ninjamuffin99, PhantomArcade, evilsk8r, Kawai Sprite and contributors). This is an unofficial fan project, not affiliated with or endorsed by The Funkin' Crew.
