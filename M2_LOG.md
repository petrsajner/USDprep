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

## Decisions taken along the way

- Viewport picking stays in **Prim** mode. Model mode would give an
  artist the whole asset on one click, but on a scene without `kind`
  metadata it climbs to the pseudo-root and selects nothing — ↑ does the
  same job safely.
- Plain click on a selected row deselects only that row; the others
  stay. The plain click only ever replaces the selection on the way in.
- The revealed object stays folded even if the user had opened it
  before — the spec said so, and it keeps a 3D pick from unfolding a
  hundred meshes.

## Open

- Level 3 formats (OBJ own writer; Alembic needs our own USD build with
  usdAbc; FBX undecided) — see the design doc.
- Type size is fixed at 1.3×; a setting can come when someone asks.
- The panel has not been tried on ALab (47k prims) yet — the tree draws
  only unfolded rows, the search walks the whole stage per keystroke.
