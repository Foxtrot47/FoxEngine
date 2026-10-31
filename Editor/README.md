# FoxEditor viewport

The viewport follows Unreal-style navigation and toolbar grouping. It is not a
complete Unreal Editor clone. Engine coordinates remain **Y-up**; snap distances
are engine world units, not Unreal centimetres.

## Navigation

| Input | Perspective | Orthographic |
|---|---|---|
| RMB + drag | Look around | Pan |
| RMB + WASD / Q,E | Fly / global down, up | No fly movement |
| RMB + R,F | Local up, down | No fly movement |
| RMB + wheel | Adjust fly speed | Pan gesture; release RMB to wheel-zoom |
| LMB + drag | Move forward/back and turn | Reserved for future marquee selection |
| LMB + RMB + drag | Pan | Zoom |
| MMB / Alt+MMB + drag | Pan | Pan |
| Alt+LMB + drag | Orbit focus pivot | Orbit without changing projection |
| Alt+RMB + drag | Dolly | Zoom extent |
| Wheel | Dolly | Zoom extent |
| F | Frame selection | Frame selection |

Arrow/numpad movement and Page Up/Down also work during perspective RMB fly.
Shift boosts fly speed. RMB+Z/C adjusts perspective FOV. Mouse navigation begins
only on the viewport image, not on toolbar controls, and ends on release or focus
loss. Clicking selects on release; camera drags do not change the selection.

View changes preserve camera position and orthographic zoom. The first switch to
orthographic initializes its extent from the current perspective focus distance.
**F** is the explicit framing command. Axis presets use exact directions; the
Isometric preset uses the classic equal-axis orthographic orientation. Orbiting
an axis preset changes its label to Orthographic, not Perspective.

## Shortcuts

- `Alt+G`: Perspective
- `Alt+J` / `Alt+Shift+J`: Top / Bottom
- `Alt+H` / `Alt+Shift+H`: Front / Back
- `Alt+K` / `Alt+Shift+K`: Left / Right
- `Q/W/E/R`: Select / Move / Rotate / Scale
- `Space`: cycle Move / Rotate / Scale
- `G`: Game View (hide editor helpers)
- `Ctrl+R`: toggle realtime simulation preview
- `F11`: immersive viewport
- `Ctrl+Z` / `Ctrl+Y` (or `Ctrl+Shift+Z`): undo / redo
- `Ctrl+S` / `Ctrl+Shift+S`: save / save as
- `Ctrl+D`, `Del`, `F2`: duplicate, delete, rename the selection

Viewport commands require the pointer over the image and no active text entry,
popup, gizmo manipulation or camera drag. The camera menu also exposes every
view, including Isometric. Blender numpad view bindings have been removed.

## Toolbar and panels

- Transform tools, World/Local space, and independent move/rotation/scale snaps.
  Scale uses object-local axes as implemented by ImGuizmo; move/rotate honor the
  selected coordinate system. Narrow viewports put transform tools in a menu.
- Camera projection, sensitivity, FOV/orthographic height, and fly speed.
- Lit / Unlit / Wireframe visualization overrides do not edit scene materials.
- Show menu: light helpers, orientation axes, orthographic grid, optional stats,
  Game View, realtime preview, immersive mode.
- Outliner: actor-name/type search and column headers.
- Content Browser: current-folder name search, asset-type filter, thumbnail size.
- Details: property-group search. Matching groups remain intact so dependent
  enable toggles/settings stay accessible; this is not individual-row filtering.

All new viewport settings are session-only; the existing dock layout still uses
`editor.ini`. Scene camera near/far/FOV controls retain their existing semantics.

## Undo and scene files

Undo covers gizmo transforms, Details edits of every entity kind except the
World settings, outliner add/duplicate/delete/rename, and Content Browser mesh
imports. Deleted entities are kept alive on the stack (including particle GPU
buffers) and reinserted at their original index, so earlier entries stay
valid; the stack only resets when a scene loads. Visibility toggles in the
outliner are not recorded. The Edit menu shows the next undo/redo label.

File > Open Scene lists `Assets/Scenes`. Save As writes `<name>.json` there and
switches the editor to it. Opening or reloading a scene (menu or Content
Browser) and closing the window prompt Save / Don't Save / Cancel when the
serialized scene differs from the last save.

## Rendering limits

Orthographic views skip perspective-only SSAO, volumetric fog and FSR rather than
reconstructing incorrect rays/depth. Returning to perspective restores their
existing enabled settings. Shadow cascades fit parallel orthographic slices.
Unlit and Wireframe omit lighting post-effects; tone mapping/grading still apply.
Realtime-off holds simulation time (including fog drift) but continues rendering
so camera navigation and editing remain responsive.

Multi-selection, marquee selection, hierarchy, multiple viewports, asset
placement by drag-and-drop and customizable/persistent key bindings are not
part of this pass.

## Validation

`EditorCameraTests` covers projection changes without repositioning/zoom resets,
exact axis bases, viewport-scaled panning, portrait/landscape focus framing,
parallel ortho pick rays, dolly clamping and orthographic shadow-volume coverage.

Manual interaction checklist:

1. Switch every view, zoom, switch again: position/ortho extent must not reset.
2. In ortho, RMB/MMB pan; LMB+RMB zoom; Alt+LMB remains orthographic.
3. In perspective, fly, then add/remove LMB: chord changes must not jump.
4. Move the pointer into the viewport and click: no camera movement on press.
5. Manipulate each gizmo with World/Local and snap toggles; undo the changes.
6. Click toolbar controls, type in search, and use popups: no camera/picking leaks.
7. Toggle Lit/Unlit/Wireframe, G, Ctrl+R and F11; restore each mode.
8. Resize to a narrow viewport and check the collapsed transform menu.
9. Add, duplicate and delete each entity kind, edit Details sliders/combos, then
   undo everything and redo it: indices, names and selection must round-trip
   and the title's `*` must clear when fully undone.
10. Edit, then Open/Reload/close the window: Cancel keeps the scene, Don't Save
    discards, Save writes first. Save As creates a new file and retitles.
