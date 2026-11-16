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

### Requirements

- Linux or Windows 10/11 with **WSL2 Ubuntu**.
- Python 3, Pillow, NumPy, FFmpeg, Git and a C compiler.
- **Docker** (recommended): `build.sh` uses the official `devkitpro/devkitarm` image, so local devkitPro is not required.
- Alternatively, install [devkitPro](https://devkitpro.org) with `nds-dev` and build with `make`.

The original game assets are required separately; this repository contains no assets or prebuilt ROM.

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
git clone https://github.com/RonyxDumb/funkinDS.git
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

Without the optional `funkin-art` checkout, skip the `--art` option below; Boyfriend's health icon is used.

### 5. Convert assets

```sh
python3 tools/convert.py funkin-assets --art=funkin-art
```

This creates `nitrofiles/`, `maxmod_data/`, `source/gen_assets.c` and `include/gen_assets.h`. Allow sufficient free disk space.

### 6. Build

**Docker (recommended):**

```sh
chmod +x build.sh
./build.sh
```

`./build.sh` without arguments **only builds**; it does not convert the assets again.

**With locally installed devkitPro / nds-dev:**

```sh
make
```

For local `make`, ensure `DEVKITARM` and the other devkitPro environment settings are configured.

The output is **`FunkinDS.nds`** at the root of the repository, approximately **200 MB** (IMA ADPCM audio and 256×192 cutscenes at 12 fps). Test it in **melonDS** or with compatible flashcart software such as TWiLight Menu++ / nds-bootstrap.

### Troubleshooting

- **`destination path 'funkinDS' already exists`**: enter the existing folder with `cd ~/funkinDS` and run `git pull`.
- **Docker socket permission denied**: check `docker info`, running Docker, group membership or Docker Desktop WSL integration as described above.
- **`Permission denied` under `build/`**: earlier root-owned output may be the cause. From the repository root, if `build/` exists, run `sudo chown -R "$USER:$(id -gn)" build` and retry without `sudo`.
- **`No module named PIL` or `numpy`**: `source .venv/bin/activate` then `python3 -m pip install pillow numpy`.
- **`ffmpeg: command not found`**: `sudo apt install ffmpeg`.
- **`Please set DEVKITARM`**: local `make` needs devkitPro configured; alternatively use Docker via `./build.sh`.
- **Missing or stale in-game assets**: rerun asset conversion before rebuilding.

### GitHub Actions

`.github/workflows/build.yml` builds the ROM on every push: it downloads Funkin.assets (and the icon from Funkin.art), converts them (the result is cached until the assets or the converter change, the first run takes a while) and uploads `FunkinDS.nds` as a workflow artifact. Since the game assets are not redistributable, the ROM is never attached to a public release.

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
