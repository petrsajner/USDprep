## What 0.9.2 is about

Alembic and FBX join OBJ as files USDprep opens directly. Like an OBJ, they are converted to a temporary USD file on opening - the original is only read - and from then on they are scenes like any other: pick objects, reduce them, export them for Nuke.

## New

- **Alembic (`.abc`)** - camera tracks, point caches, animated characters. Every mesh with its animation sample by sample, cameras with their focal length and film back in millimetres (lens squeeze included), UVs, normals, vertex colours, face sets and subdivision creases. The frames stay the ones the file has.
- **FBX (`.fbx`)** - models and characters from Maya, 3ds Max, Blender, Unreal or an online store. Meshes with their materials and textures (also the ones packed inside the file), cameras, lights and the animation of the first take on the file's own frames - a shot that starts at 1001 stays at 1001. A character moved by a skeleton or by blend shapes arrives in Nuke as a point cache: it simply plays.
- **File > Open** lists both formats, dropping a file on the window works, and `usdcut` takes them for every command: `usdcut extract character.fbx /character -o character.usdc --preset nuke`
- Everything needed is inside the installer; nothing is downloaded when the program runs.

## Also in the Nuke export

- **Repeating textures.** Nuke does not repeat a texture across a surface - bark wrapped six times round a log came out as one smeared colour. Such a texture is now laid out over its repeats in a copy, and the material reads that.
- **A texture that is not on this computer** no longer turns the object black: the object shows its material's colour, and the report names the missing file.

Both measured in Nuke 16.1 and 17.0 ([NUKE_COMPAT.md](https://github.com/petrsajner/USDprep/blob/main/NUKE_COMPAT.md), fifth measurement).

## Install

Download **`USDprep-0.9.2-setup.exe`**, run it, follow its pages - it installs over 0.9.1. Windows 10/11 64-bit, per user, no admin rights, no internet, nothing else to install. The updated user manual (section *Alembic, FBX and OBJ files*) is in the Start menu after installation and attached below.
