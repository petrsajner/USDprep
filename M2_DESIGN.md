# M2 — the Prep panel, redesigned

**Date:** 2026-09-20 · **Status:** decided, being built

## Why the v0.5 panel was not it

usdtweak is an editor for USD engineers: many small panels, information
everywhere, every decision visible. Our user is a Nuke compositor who
wants one thing — find an object in a big scene and get it out as a
standalone file — and does not think in prims, kinds or layer stacks.
The v0.5 panel was a small floating window on top of all that, so it
inherited every problem: cramped, no help with orientation, and the
stock panels still did the real work.

## What we keep from usdtweak

The 3D viewport (Storm), the stage selection, the file menu, the undo
stack. Everything else is optional and hidden by default.

## The panel

One tab, docked as the right panel's first (and, by default, only)
content. From top to bottom:

1. **Scene** — file name and object count.
2. **Search** — narrows the tree to matching names *and their parents*,
   so a match is always seen in context.
3. **Tree** — larger type than the rest of the app, one row per object,
   the name and nothing else (type and full path on hover, a dim child
   count when collapsed). Click a name: that object and everything under
   it is selected. Click it again: deselected. Ctrl+click adds or removes
   without touching the rest. Selected rows turn yellow; everything
   underneath them turns a dimmer yellow, so "and all children" is
   visible, not implied. Arrow or double-click folds and unfolds.
4. **Export** — the export list, the preset, where the file goes, the
   button. The bottom of the panel, always visible.

## Selection, one truth

The editor's stage selection is the only selection. Clicking in the 3D
view, in the tree, or pressing a key all end in the same place, and the
viewport highlight, the tree colouring and the export all read from it.

When the selection changes from outside the tree (a click in 3D), the
tree unfolds exactly the parents needed to show the picked object and
scrolls it to the middle of the panel. The picked object itself stays
folded.

## Keys (global — no matter which panel has focus)

| key | does |
|---|---|
| **↑** | select the parent of the current object (with all its children) |
| **↓** | go back down to the child we came up from; else the first child; else nothing |
| **F** | frame the 3D view on the selection |

Keys are ignored while a text field has the keyboard.

## Export list (level 2)

"Add to export" puts the current selection's top-level objects on a list
that survives further browsing; each entry has a remove button. When the
list is empty, Export takes the current selection — so the one-object
case stays one click.

## Presets in the panel

A combo: **Nuke-ready** (default), **Raw copy**, and **Recipe file…**
for a studio JSON recipe. "Advanced" shows what the chosen recipe does,
and the three switches (de-instance, main object, copy textures) still
override it for one run.

## Simple mode

On first start the panel hides the Stage outliner, the property editor
and the timeline (Windows menu brings any of them back), docks itself
into the right panel and takes the front tab. This happens once; from
then on the user's layout is theirs.

## Level 3 — other formats (not now, and why)

- **OBJ**: we can write it ourselves (meshes, UVs, normals; materials
  only as far as MTL goes). Cheap, and useful for classic ReadGeo.
- **Alembic**: the conda USD build has no usdAbc plugin. It comes with
  our own USD build for the release (M4 already plans that build), not
  before.
- **FBX**: no free writer worth shipping. Assimp can, with caveats to
  evaluate. Decide when OBJ and Alembic are in.

## Not doing

- A second outliner-style panel with columns, kinds and visibility
  toggles. The stock one exists for that.
- Editing anything in the scene. This panel reads and exports.
