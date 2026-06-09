# GPU Demo Editor Architecture

This project is a wxWidgets + OpenGL room editor/viewer for Landstalker data.

It renders:
- Isometric map and heightmap overlays
- Sprite instances (entities)
- Warp regions
- Tile swap regions and door overlays

It also supports editing all object types directly in the canvas.

## Build

From the existing build directory:

```bash
cd build
cmake --build . -j1
```

The executable is generated as `gpu_demo` in the build output.

## High-Level Design

The code is split into three layers:

1. **Canvas and rendering host**
   - `GLCanvas.cpp/.h`
   - Owns event routing, camera, render passes, persistent room state, and selection state.
2. **Object-specific editors**
   - `GLCanvasEntityEditor.cpp/.h`
   - `GLCanvasWarpEditor.cpp/.h`
   - `GLCanvasTileDoorEditor.cpp/.h`
   - Encapsulate edit operations for each object family.
3. **Cross-object coordinator + shared support**
   - `GLCanvasObjectCoordinator.cpp/.h`
   - `GLCanvasObjectSupport.cpp/.h`
   - Coordinator handles operations that span all object types (selection cycling, delete, nudge, reorder).
   - Support module holds reusable geometry/math/ordering algorithms used by multiple units.

## Main Components

### `MyGLCanvas` (`GLCanvas.cpp/.h`)

Primary responsibilities:
- OpenGL context initialization and frame loop (`OnTimer`, `OnPaint`)
- Camera and zoom controls
- Input handling (mouse + keyboard)
- Selection/hover/drag state for all editable object categories
- Room load/persist synchronization with `GameData`

`MyGLCanvas` keeps the canonical runtime state vectors:
- `m_instances` (render instances for entities)
- `m_room_entities` (entity data to persist)
- `m_warps`
- room-scoped tile swaps/doors accessed through `RoomData`

Thin forwarding methods on `MyGLCanvas` call the specialized editor/coordinator classes so input flow remains centralized while edit logic stays modular.

### `GLCanvasEntityEditor`

Entity-specific actions:
- Add, copy, paste, cut/cycle attributes
- Orientation and floor snapping updates
- Maintains consistency between `m_room_entities` and `m_instances`
- Re-sorts instance draw order via shared support sorting

### `GLCanvasWarpEditor`

Warp-specific actions:
- Two-step warp creation (`AddWarpHalf`)
- Warp resize/rotate/type cycling
- Warp floor recomputation and constraints

### `GLCanvasTileDoorEditor`

Tile swap + door actions:
- Add door, cycle door size
- Add tile swaps, cycle shape/id
- Resize selected tile swap region
- Toggle/clear preview overlays

Preview operations intentionally mutate temporary render state; `ClearTileSwapPreview()` restores canonical room data.

### `GLCanvasObjectCoordinator`

Cross-object behavior:
- Delete selected object for whichever type is active
- Reorder selected object
- Selection traversal (`SelectNextObject`) across all object types in one unified tab order
- Shared nudge behavior for entity/warp/door/tile-swap regions

### `GLCanvasObjectSupport`

Reusable algorithms shared across translation units:
- Tile swap region geometry generation
- Tile swap region metrics extraction
- Warp instance creation and size validation helpers
- Entity geometric sorting for stable draw order

This avoids duplicating math/geometry behavior in multiple editor files.

## Rendering and Edit Flow

1. `LoadRoom(...)` populates runtime vectors and renderer state from `GameData`.
2. User input is captured in `MyGLCanvas` event handlers.
3. Forwarded edit operations are executed by editor/coordinator classes.
4. Shared support utilities perform reusable calculations (region geometry, sorting, warp constraints).
5. `PersistCurrentRoomEdits()` writes edited state back into `GameData` structures.

## OpenGL Primer For This Project

This codebase uses a hybrid style:
- Modern-ish OpenGL objects (shader programs + texture objects)
- Legacy immediate mode for many debug/editor overlays (`glBegin`/`glEnd`)

That makes the rendering code easier to inspect while still using shaders for map/sprite decoding.

### Frame Pipeline (`MyGLCanvas::OnPaint`)

Each paint call does this, in order:

1. **Bind the context and configure 2D camera space**
  - `SetCurrent(...)` selects this window's OpenGL context.
  - `glViewport(...)` sets drawable pixel bounds.
  - `glOrtho(...)` creates a 2D projection in screen pixels.
  - `glTranslatef` + `glScalef` apply camera pan/zoom.
2. **Clear frame buffers**
  - `glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)` resets color and stencil.
3. **Render world layers**
  - Map -> optional heightmap -> tile swaps -> doors -> warps -> sprites.
4. **Render editor overlays**
  - Handles, tooltips, collision/occlusion diagnostics.

Rendering order matters because stencil and alpha blending are order-dependent.

### Shaders In This Repo

Shader programs are built in `CreateShader(...)` helpers in renderer classes.

OpenGL shader lifecycle used here:
1. `glCreateShader(...)`
2. `glShaderSource(...)`
3. `glCompileShader(...)`
4. `glCreateProgram(...)`
5. `glAttachShader(...)`
6. `glLinkProgram(...)`
7. `glUseProgram(...)` before drawing

The map fragment shader (`shaders/map.frag`) is the key decoding stage:
- Reads block IDs from `u_map`
- Uses `u_blockset` to choose tile index + flip flags
- Reads tile pixels from `u_tileset`
- Looks up final RGBA via `u_palette`
- Supports animated tiles with metadata textures (`u_anim_metadata1/2`)

The sprite fragment shader (`shaders/sprite.frag`) is simpler:
- Reads palette index from sprite atlas texture
- Discards index 0 (transparent)
- Looks up final color from palette texture row

### Texture Units And Samplers

For map rendering, multiple textures are bound to units (`GL_TEXTURE0`..`GL_TEXTURE5`) and connected to shader sampler uniforms with `glUniform1i`.

Think of each texture unit as an input slot. The uniform tells the shader which slot to read.

### Stencil Buffer Usage

Stencil is used as a lightweight per-pixel mask channel.

Common calls and meanings:
- `glStencilMask(mask)`: which stencil bits are writable
- `glStencilFunc(func, ref, mask)`: test rule for current fragment
- `glStencilOp(...)`: what to do on stencil/depth outcomes

Stencil conventions used by canvas/sprite flow:
- `0x01`: occluded by heightmap geometry
- `0x04`: covered by foreground-priority map tiles

Typical pattern in this code:
1. Build stencil mask in a color-disabled pass (`glColorMask(false, ...)`)
2. Render sprite/overlay with stencil test enabled
3. Disable stencil and continue normal rendering

### Occlusion Algorithm (High-Level)

For each sprite:
1. Build an occlusion stencil for the sprite's projected bounds (terrain + optional FG coverage).
2. Draw visible fragments where stencil does **not** indicate occlusion.
3. In transparent-occlusion mode, draw occluded fragments again at lower alpha.

That produces either:
- fully hidden occluded parts (`ObscuredHidden`)
- or "ghosted" occluded parts (`ObscuredTransparent`)

### Dragging And Selection Algorithm

Input handling is centralized in `MyGLCanvas`:
- Hover and click use prioritized hit-tests (resize/z handles first, then object bodies, then fallback targets).
- Active drag state is exclusive (`m_dragging_entity`, `m_dragging_warp`, etc.).
- During drag, mouse movement updates only the active object type.

Coordinate flow:
1. Screen pixel -> world position (`ScreenToWorldX/Y`)
2. World -> map/heightmap grid (`ScreenToMapPoint` / `ScreenToHeightmapPoint`)
3. Grid coordinates are clamped and written back to object state

This keeps interaction consistent regardless of camera pan/zoom.

### Why Some Rendering Uses `glUseProgram(0)`

`glUseProgram(0)` switches to fixed-function behavior.

In this project it is mainly used for editor overlays (lines/boxes/text markers) where shader decoding is unnecessary and immediate-mode drawing is convenient.

## Algorithm Notes

### Entity draw order

Entity ordering uses geometric heuristics and pairwise constraints:
- compute overlap/depth relationships
- build a dependency graph of "must draw before"
- topologically emit a stable order

This reduces visual popping compared with a single scalar sort key.

### Tile swap region geometry

Tile swap outlines are expanded into projected polygons/segments for both:
- tilemap source/destination regions
- heightmap source/destination regions

The generated geometry drives:
- drawing outlines and handles
- hit-testing
- resize/nudge operations

## File/Module Guide

- App shell:
  - `main.cpp`
  - `MainFrame.cpp/.h`
- Canvas + editor architecture:
  - `GLCanvas.cpp/.h`
  - `GLCanvasEntityEditor.cpp/.h`
  - `GLCanvasWarpEditor.cpp/.h`
  - `GLCanvasTileDoorEditor.cpp/.h`
  - `GLCanvasObjectCoordinator.cpp/.h`
  - `GLCanvasObjectSupport.cpp/.h`
- Renderers:
  - `MapRenderer.cpp/.h`
  - `HeightmapRenderer.cpp/.h`
  - `SpriteRenderer.cpp/.h`
- OpenGL loading/helpers:
  - `GLLoader.cpp/.h`
  - `PixelFont.h`
  - `SpriteInstance.h`

## Notes for Future Changes

- Prefer adding object-specific edit behavior in the relevant editor class, not in `MyGLCanvas`.
- Keep cross-object decisions in `GLCanvasObjectCoordinator`.
- Add new reusable math/geometry to `GLCanvasObjectSupport` when used by more than one module.
- Keep `MyGLCanvas` wrappers thin and focused on event/routing concerns.
