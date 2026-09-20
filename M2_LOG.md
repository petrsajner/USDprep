# M2 — GUI panel: working log

**Started:** 2026-09-17 (v0.2–v0.5, floating window) · **redesigned:** 2026-09-20 (see `M2_DESIGN.md`)

## Slice 1 delivered (2026-09-20)

The panel is one tab, "Prep for Nuke", docked as the right panel; the
stock usdtweak panels are hidden by default (Windows menu brings them
back). Verified live on Kitchen_set (2,742 prims):

| priority | state |
|---|---|
| 3D view kept, rest of the environment out of the way | ✅ simple mode: viewport + our tab, nothing else |
| our tab in the right panel, front after start | ✅ docked into the outliner's node via DockBuilder, focused |
| tree: bigger type, readable, easy fold/unfold, click = select object + children, click again = deselect, selected = yellow text | ✅ 1.3× type, name only (type + path on hover), arrow or double-click folds, dim yellow for everything under a selected object, ctrl+click adds/removes |
| two-way sync; a 3D pick unfolds exactly the path and scrolls the object to the middle, children stay folded | ✅ |
| global ↑/↓: parent / remembered child (else first child) | ✅ plus F = frame the 3D view; ignored while a text field has the keyboard |
| level 2: export list | ✅ "Add to export", per-entry remove, Export takes the list or, when empty, the selection |
| presets in the panel | ✅ Nuke-ready / Raw copy / Recipe file…; Advanced shows what the recipe removes; switches override for one run |
| search | ✅ narrows to matches *and their parents*, unfolded |

Also: the suggested file name follows the selected object until the
artist edits the path; the result line reads "Done in 0.6 s: 65
objects, 54 meshes, 210 KB" with the report behind "Details".

## How it is built (for whoever touches it next)

- `SceneTree` — reads the editor selection every frame; a change it did
  not make itself is treated as external and revealed. Its own changes
  go through `ExecuteAfterDraw` like everything else that mutates the
  editor. Keys are polled in `draw()`, which the Editor calls even when
  the tab is not in front — that is what makes them global.
- `ExportPanel` — export list, preset choice (persisted in the addon
  settings), destination, the run.
- Simple mode is applied once per `layoutVersion`; bump the constant to
  re-apply a changed layout on an existing installation.
- Reveal = `SetNextItemOpen(true)` on the ancestors and
  `SetNextItemOpen(false)` on the target, `SetScrollHereY(0.5)` on the
  target row, all one-shot in the frame the change is observed.

## Slice 2 (2026-09-20): smart pick, ALab

- **Smart pick.** A click on a mesh in 3D selects the object it belongs
  to: the nearest ancestor whose `kind` is a model (the component; an
  assembly where no component exists). Both Kitchen_set and ALab mark
  their artist-level objects `component`. A scene without kinds gives
  the mesh itself, and ↑ takes it from there. Done in the panel, not by
  switching the viewport's own pick mode, because that mode selects the
  pseudo-root on a scene without kinds.
- **Deselecting a row takes everything under it along**, ctrl+clicked
  children included. The rest of the selection stays.
- **ALab (47,401 prims):** loads, 60 FPS, search narrows instantly,
  a 3D click on the stoat lands on `stoat/outfit_M_hrc`; export of that
  component from the panel: 1.3 s, 91 MB usdz, 12 UDIM tiles inside.
- **A missing texture no longer sinks the export.** ALab's texture pack
  is not installed here, and USD's packager and localizer both give up
  on the first dependency they cannot find. Dead references are now
  taken out of the flattened layer and named in the report
  ("1 texture file(s) are not on this machine and were left out:
  stoat_outfit01.<UDIM>.exr"); fixture `missing_textures.usda` and a
  test pin it.
- usdtweak switches its content browser on every time a stage is
  opened; simple mode flips it back when the flag turns on in the same
  frame the stage changes, and leaves a Windows-menu choice alone.

## Slice 3 (2026-09-20): the export section, after the first real use

Feedback after an hour with ALab, all of it about finding things at a
glance rather than reading:

- one type size for the whole panel (the tree's 1.3x), export section
  and header included
- "Add to export" and "Clear list" twice the size, coloured (blue / brown-
  red), side by side *above* the list
- the list sits in its own tinted frame headed "To export: N object(s)";
  while the list is empty the frame shows the current selection instead,
  so it always answers "what goes out if I press Export now"
- a click on a list entry selects that object in the tree and the 3D
  view (the entry that *is* the selection is yellow, like in the tree) -
  the quick visual check before pressing Export
- Export: full width, tall, green
- "Clear selection" grew with the panel's type size

USD's own warnings now reach the report's Details (coalescing delegate
around every operation, known noise folded into one line) - see the
core commits af49acb and 7850e95.

## Decisions taken along the way

- Plain click on a selected row deselects that row and everything under
  it; the others stay. The plain click only ever replaces the selection
  on the way in.
- The revealed object stays folded even if the user had opened it
  before — the spec said so, and it keeps a 3D pick from unfolding a
  hundred meshes.

## Open

- Level 3 formats (OBJ own writer; Alembic needs our own USD build with
  usdAbc; FBX undecided) — see the design doc.
- Type size is fixed at 1.3×; a setting can come when someone asks.
- USD's own warnings (the localizer's "failed to resolve" noise, and
  anything real hiding in it) still go to the console, not into the
  report's Details. A coalescing diagnostic delegate around the run
  would bring them in.
