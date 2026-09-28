# DarkHouse

DarkHouse combines four desktop imaging tools in one application: a
**non-destructive RAW developer**, a **high-throughput digital asset manager
(DAM)**, a **layered pixel compositor** and a **vector designer**. They share one
catalog, one GPU pipeline and one document model.

Stack: C++20 · CMake ≥ 3.22 · SQLite 3 (WAL) · Vulkan 1.3 / SPIR-V ·
GLFW · Dear ImGui (docking, Vulkan backend with dynamic rendering) · GLM ·
ONNX Runtime

![The Canvas workspace: a test chart rendered through the GPU develop graph, with the layer stack, adjustments, metadata and filmstrip](docs/images/workspace-canvas.png)

---

## Design principles

1. **Originals are never modified.** Every edit is data. A develop stack is an
   ordered list of `(node_type, serialized_params)` rows in `edit_nodes`, and
   layers reference catalog assets instead of copying pixels. Exporting renders
   the stack again.
2. **The catalog comes first and is the source of truth.** One SQLite file in
   WAL mode. Imports hash and parse metadata in parallel on a worker pool, and
   only the final `INSERT` is serialized. The UI reads through a separate
   read-only connection, so browsing never waits for an import.
3. **Content identity, not path identity.** Assets are deduplicated by an XXH64
   content hash, so importing the same file again returns the existing record.
   Asset IDs are UUIDs that stay the same when files move.
4. **Scene-referred, GPU-first pixels.** The pipeline works in linear light,
   RGBA16F end to end. Operators are Vulkan compute nodes in a DAG with
   synchronization2 barriers, recorded into one command buffer per evaluation.
5. **Sparse by default.** Raster layers are maps of 512×512 FP16 tiles that are
   only allocated when painted. Writes mark tiles dirty, and only dirty tiles
   are re-uploaded to the GPU.
6. **One document model.** Adjustments, pixels, vector shapes and smart objects
   are all `LayerNode`s in one tree, with isolated groups and non-destructive
   masks.
7. **Optional subsystems degrade on their own.** Only the catalog is required.
   If there is no Vulkan device, the app runs in catalog-only mode. If ONNX
   Runtime or the models are missing, AI selection is off. Failures are logged,
   not fatal.
8. **One device for pixels and pixels-on-screen.** The desktop UI renders with
   the same `VkDevice` and queue as the develop graph, so the viewport samples
   develop outputs directly: no copies, no cross-queue ownership transfers.

## Architecture

```
            ┌──────────────────────────── GuiEngine (FrontEnd) ─────────────────────────────┐
            │ PlatformWindow (GLFW) · Swapchain (FIFO/mailbox, 2 frames in flight)          │
            │ Dear ImGui: GLFW platform + Vulkan renderer (dynamic rendering), docking      │
            │ GuiLayer = DarkHouseShell: menu · workspace switcher · status bar ·           │
            │            WorkspaceLayoutManager · panels · LibraryModel (view model)        │
            └───────────────┬───────────────────────────────▲───────────────────────────────┘
               AppEvents    │                               │  FrameContext (mode, canvas texture)
                            ▼                               │
                         ┌──────────────────────────────────┴┐
                         │           DarkHouseApp            │  modes: CATALOG | CANVAS | HYBRID_SPLIT
                         │ event queue → frame loop → pacing │
                         └──┬─────────┬─────────┬────────────┘
                            │         │         │
              ┌─────────────▼──┐ ┌────▼──────┐ ┌▼──────────────────────┐
              │  AssetManager  │ │ LayerNode │ │  RenderPipelineGraph  │
              │ SQLite WAL,    │ │ tree +    │ │  ComputeNode DAG      │
              │ import pool,   │ │ Sparse-   │ │  (ExposureNode, ...)  │
              │ EXIF, XXH64    │ │ Raster-   │ │  on VulkanContext ◄───┼── shared with the UI
              └────────────────┘ │ Layer     │ └───────────▲───────────┘
                                 └────┬──────┘   dirty FP16 tiles → GPU upload
                                      └──────────────────────┘
                     AISegmentationEngine (ONNX Runtime) → masks
```

| Path | Responsibility |
| --- | --- |
| `schema/catalog.sql` | Catalog schema: `assets`, `metadata`, `edit_nodes`, indexes. Embedded into the binary at configure time. |
| `include/asset_manager.hpp` | `AssetRecord` / `AssetMetadata`, async `importFile()`, parameterized `queryAssets()`, ratings/flags/labels, edit stacks. |
| `include/import_scan.hpp` | Expands files and folders into importable image paths (CLI `--import`, Import dialog, drag and drop). |
| `include/vulkan_context.hpp` | `PixelFormat`, `GPUTexture`, `VulkanContext` (instance, device, queue, optional window surface, validation messenger, textures, uploads) and synchronization2 barrier helpers. |
| `include/swapchain.hpp` | `Swapchain`: present mode and format selection, transparent recreation, per-frame fences and semaphores. |
| `include/render_pipeline.hpp` | `ComputeNode`, `ExposureNode`, `RenderPipelineGraph`. |
| `shaders/exposure.comp` | Exposure (EV), highlights/shadows and contrast on RGBA16F storage images, in 16×16 workgroups. |
| `include/layer_stack.hpp` | `TILE_SIZE`, FP16 tiles, `SparseRasterLayer`, `LayerNode` tree, blend modes, CPU reference compositor. |
| `include/ai_segmentation.hpp` | `AISegmentationEngine` (subject/sky) and the ONNX Runtime backend. |
| `include/app_controller.hpp` | `DarkHouseApp`, `AppMode`, `AppEvent`, and the `FrontEnd` seam (with its GPU lifecycle) for the UI layer. |
| `include/platform_window.hpp` | `PlatformWindow`: the GLFW window, surface creation, resize/drop/input tracking. |
| `include/gui_engine.hpp` | `GuiEngine` (the desktop `FrontEnd`) and the `GuiLayer` interface. |
| `include/ui/shell.hpp` | `DarkHouseShell`: main menu, workspace switcher, status bar, Import dialog. |
| `include/ui/workspace_layout.hpp` | `WorkspaceLayoutManager`: one dockspace and default layout per workspace. |
| `include/ui/library_model.hpp` | `LibraryModel`: collections, search filter, sort and selection shared by the catalog panels. |
| `include/ui/panels.hpp` | The dockable panels (see [Desktop UI](#desktop-ui)). |
| `include/ui/widgets.hpp`, `include/ui/theme.hpp` | Shared widgets (ratings, labels, thumbnail cards, gradient sliders) and the DarkHouse look. |
| `src/main.cpp` | Command-line interface and front-end selection. |
| `tests/` | CTest programs (see [Testing](#testing)). |

### Frame loop

Each frame runs these steps in order:

1. `FrontEnd::pumpPlatformEvents` turns OS input into `AppEvent`s. The desktop
   UI polls while you interact and blocks for input when idle (below).
2. Queued events are drained. Events are thread-safe to post from anywhere.
3. Finished imports are collected.
4. If the canvas is visible (`CANVAS` / `HYBRID_SPLIT`), dirty document tiles
   are composited, converted to FP16 and uploaded in one submission, and the
   develop graph is re-evaluated only when something changed.
5. `FrontEnd::drawFrame` builds the UI and presents. The develop output is
   sampled straight from the graph's output image, in `GENERAL` layout.
6. Pacing: with vsync, presentation (FIFO) paces the loop. Without it, the loop
   sleeps to hold the target frame rate.

**Idle pacing.** A photo library stays open for hours, so the UI does not
redraw an unchanged screen at the display rate. A few frames after the last
input (with no widget held and no text entry active) the loop blocks in
`glfwWaitEventsTimeout`. It wakes immediately on input, at 4 Hz for delayed
tooltips, and at 10 Hz while imports are running. An idle window costs about
5 frames per second. Toggle it with View → Low-Power Idle, or use
`--continuous` for benchmarks.

### Safety notes

- `queryAssets(filter, params)` takes a SQL boolean expression so smart
  collections stay flexible. Pass values through `?` parameters. The filter
  runs on a **read-only** connection with an authorizer that allows only
  reads, and multi-statement input is rejected.
- `ComputeNode::setInputTexture` may reallocate GPU resources, so the graph
  must not be re-wired while a submission that uses it is still in flight.
  With the UI, frames can still be in flight, so rebuilding the develop graph
  waits for the device to go idle first. Rebuilds also bump
  `FrameContext::canvasGeneration`, and the UI keys its sampled-texture
  descriptor on that counter, not on the image view handle (handles can be
  recycled). It frees retired descriptors only after the frames that used
  them have completed.
- A failing develop graph (for example a missing shader) disables the canvas,
  not the device the UI renders with.

## Desktop UI

![The Catalog workspace: search, collections, metadata and the library grid](docs/images/workspace-catalog.png)

DarkHouse opens in a window titled **DarkHouse**, 85% of the screen by
default. Three workspaces follow the application mode. Switch with the
centred **Catalog | Canvas | Split** buttons or `Ctrl+1/2/3`:

| Workspace | Focus | Default layout |
| --- | --- | --- |
| **Catalog** | Asset grid | Search / Collections / Metadata · Library grid |
| **Canvas** | Layer stack | Collections + Search / Metadata · Viewport over Filmstrip · Layers / Adjustments |
| **Split** | Dual view | Collections + Search / Metadata · Library grid + Viewport over Filmstrip · Layers / Adjustments |

Every panel is a dockable ImGui window. Each workspace has its own dockspace
and its own arrangement, and all of them persist to
`<config>/DarkHouse/layout.ini` (`$XDG_CONFIG_HOME` or `~/.config` on
Linux, `~/Library/Application Support` on macOS, `%APPDATA%` on Windows).
View → Reset Layout restores the default. With `--viewports`, panels can be
dragged out into their own OS windows, for example a second monitor.

| Dock | Panel | What it does |
| --- | --- | --- |
| Left | **Collections** | All Photographs, Imported This Session, Picks, Rejected, Unrated; the folder tree; smart collections (Five Stars, High ISO, Wide Angle, Telephoto), with counts. |
| Left | **Search** | Text (file name, camera, lens), minimum rating, flag, colour label, camera, ISO range, sort order. Filtering runs in memory on every keystroke. |
| Left | **Metadata** | Rating, pick/reject and colour label (editable), camera, lens, exposure, dates, GPS and file details of the selected photo. |
| Center | **Library** | Virtualized thumbnail grid with zoom, context menu, tooltips and keyboard culling. |
| Center | **Viewport** | The develop graph's output texture over a transparency checkerboard. Wheel zooms around the cursor, drag pans, double-click toggles fit and 100%. |
| Center | **Filmstrip** | The current collection as a horizontal strip, kept in sync with the grid. |
| Right | **Layers** | The unified layer stack: parametric (ADJ), raster (PX), vector (VEC), smart object (OBJ) and group (GRP) layers, with visibility, blend mode, opacity, masks, add/delete/reorder. |
| Right | **Adjustments** | Tone (exposure, contrast, highlights, shadows) runs live on the GPU and is saved to the photo's edit stack on release. White balance, presence and 8-band HSL sliders are previews until their GPU nodes exist. |
| Floating | **Engine** | GPU, validation, swapchain and frame-timing diagnostics (View → Panels). |

Keyboard (grid and filmstrip focused):

| Keys | Action |
| --- | --- |
| Arrows, Home, End | Move the selection (Up/Down by one grid row) |
| `0`–`5` | Star rating |
| `P` / `X` / `U` | Pick / reject / unflag |
| `6` `7` `8` `9` | Red / yellow / green / blue label (press again to clear) |
| `Enter`, double-click | Open in the Canvas workspace |
| `Ctrl+1/2/3` | Catalog / Canvas / Split |
| `Ctrl+I` | Import Photos… (or drop files and folders onto the window) |
| `Ctrl+Q` | Quit |

Thumbnails are placeholders tinted from each file's content hash until the
decoder produces previews. Layers → Add → **Test Chart** paints a raster layer,
so the whole pixel path can be seen working: CPU tiles, FP16 upload, the GPU
exposure node, then the ImGui viewport.

## Building

### Prerequisites

| | macOS | Linux (Debian/Ubuntu) | Windows |
| --- | --- | --- | --- |
| Compiler | Xcode 15+ / Apple Clang 15+ | GCC 11+ or Clang 14+ | MSVC 2022 (17.4+) |
| Vulkan 1.3 | [LunarG SDK](https://vulkan.lunarg.com/) or `brew install vulkan-headers vulkan-loader molten-vk` | `libvulkan-dev` + a 1.3 driver | LunarG SDK |
| GLSL → SPIR-V | ships with the SDK, or `brew install shaderc` | `glslc` or `glslang-tools` | ships with the SDK |
| SQLite 3 | system library | `libsqlite3-dev` | `vcpkg install sqlite3` |
| CMake ≥ 3.22 | `brew install cmake` | `apt install cmake` | installer / VS |
| Window system (UI) | nothing extra | `libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev libwayland-dev wayland-protocols` | nothing extra |
| Validation layers (Debug) | SDK / `brew install vulkan-validationlayers` | `vulkan-validationlayers` | SDK |
| ONNX Runtime (optional) | `brew install onnxruntime` | release tarball | release zip |

GLFW 3.5.1, GLM 1.0.3 and Dear ImGui v1.92.9b-docking are fetched with CMake
`FetchContent` at configure time, pinned to those release tags. Offline builds
can point CMake at local checkouts with `-DFETCHCONTENT_SOURCE_DIR_IMGUI=…`,
`…_GLFW=…` and `…_GLM=…`. `-DDARKHOUSE_GUI=OFF` builds the command line only,
with no windowing dependencies and no downloads.

On macOS the context enables `VK_KHR_portability_enumeration` and
`VK_KHR_portability_subset` automatically, so MoltenVK is picked up without
extra flags. It needs a MoltenVK recent enough to expose Vulkan 1.3.

### Configure and build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The build produces:

- `build/bin/DarkHouse`, the executable.
- `build/shaders/*.spv`, compiled shaders. The executable knows this path;
  override it with `--shaders` or `DARKHOUSE_SHADER_DIR`.
- `build/generated/`: the embedded catalog schema and UI font.
- `darkhouse_core` / `darkhouse_gui` static libraries, and the test programs.

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `DARKHOUSE_GUI` | `ON` | Build the desktop UI (GLFW, Dear ImGui, GLM). `OFF` builds a headless CLI. |
| `DARKHOUSE_VULKAN_VALIDATION` | `AUTO` | Whether validation layers are on by default. `AUTO` = on for Debug and RelWithDebInfo, evaluated per configuration. `--validation`, `--no-validation` and `DARKHOUSE_VULKAN_VALIDATION=0/1` override it at runtime. If the loader cannot find the layer, the build-time manifest directory is added via `VK_ADD_LAYER_PATH`. |
| `DARKHOUSE_BUILD_TESTS` | `ON` | Build the CTest programs. |
| `DARKHOUSE_ONNXRUNTIME` | `AUTO` | `AUTO` uses ONNX Runtime if found, `ON` requires it, `OFF` disables it. Set `ONNXRUNTIME_ROOT` for a non-system install. |
| `DARKHOUSE_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` for every first-party target |

### Running

```bash
# The desktop UI (catalog in the current directory by default)
build/bin/DarkHouse --catalog photos.sqlite
build/bin/DarkHouse --catalog photos.sqlite --mode split --window 1920x1080

# Batch: import a folder recursively (RAW, JPEG, TIFF, PNG, ...), then list high-ISO shots
build/bin/DarkHouse --headless --catalog photos.sqlite --import ~/Pictures/2024 --query "m.iso >= 3200"

# Batch: rate an asset, open it on the canvas and run a few frames of the GPU develop graph
build/bin/DarkHouse --headless --catalog photos.sqlite --rate <asset-id> 4 --open <asset-id> --frames 10

# Catalog-only machine (no Vulkan)
build/bin/DarkHouse --no-gpu --catalog photos.sqlite --query ""
```

UI options: `--window WxH`, `--maximized`, `--no-vsync`, `--continuous`,
`--viewports`, `--layout <file>`. Without a display (for example over SSH)
DarkHouse logs a warning and runs headless, so batch work still completes.

Put `subject_segmentation.onnx` / `sky_segmentation.onnx` in a directory and
pass `--models <dir>` to enable AI selection. Run `DarkHouse --help` for every
option. Exit codes: `0` success, `1` fatal error, `2` some imports failed,
`64` bad arguments.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

| Test | Covers |
| --- | --- |
| `library_model` | Runs the engine headless on a temporary catalog: import, collections, folder tree, search, sorting, selection, and rating/flag/label round trips through engine events. |
| `present_smoke` | Opens a window, creates the presenting Vulkan 1.3 context and swapchain, and clears and presents 120 frames with dynamic rendering, resizing halfway. Runs with core **and synchronization** validation, and any validation error fails it. Exits 77 (skipped) without a display. |

The GUI tests run headless under Xvfb with Mesa's software Vulkan driver
(lavapipe):

```bash
sudo apt install xvfb mesa-vulkan-drivers vulkan-validationlayers
Xvfb :99 -screen 0 1920x1080x24 &
DISPLAY=:99 ctest --test-dir build --output-on-failure
```

The shell has also been checked with Release GCC and Clang `-Werror` builds,
an ASan/UBSan build, and scripted UI sessions under synchronization
validation. Those sessions cover importing, culling, opening assets (which
rebuilds the develop graph with frames in flight), live exposure edits,
workspace switches and window resizes.

## Status

This is the core architecture plus the desktop shell. What works today:

- **Catalog**: WAL schema, parallel import with dedupe, and metadata from
  TIFF-based RAW (DNG/CR2/NEF/ARW/ORF/RW2/PEF), JPEG/EXIF and PNG. That covers
  camera, lens, exposure, GPS and capture time with its UTC offset. Also
  parameterized queries, ratings, flags, colour labels and edit-stack
  persistence.
- **GPU**: device selection (discrete first), textures, staging uploads, the
  compute-node DAG with cycle detection and barriers, and the exposure node
  plus its shader. A presenting context with swapchain, frame
  synchronization and a debug-utils validation messenger.
- **Layers**: sparse FP16 tiles, dirty tracking, the layer tree with masks and
  groups, and the CPU reference compositor for all five blend modes.
- **App**: event-driven frame loop, three modes, graceful degradation, headless
  batch mode.
- **Desktop UI**: docking shell with three persistent workspace layouts, the
  nine panels above, culling shortcuts, import by dialog or drag and drop,
  live GPU exposure editing, idle-aware frame pacing, and a neutral grey theme.

Next milestones:

1. Display transform for the viewport. The develop output is scene-linear
   and is shown without a view transform, so it looks darker and higher in
   contrast than it will.
2. RAW decoding (LibRaw): demosaic into the RGBA16F source texture and real
   thumbnails for the grid and filmstrip. Add CR3, HEIF and RAF metadata.
3. GPU compositing: blend modes and masks as compute nodes, with the CPU
   compositor kept as the parity reference. Adjustment, vector and
   smart-object layers render there.
4. GPU nodes for white balance, presence and HSL, wired to the existing sliders.
5. Vector rasterization of `VECTOR_SHAPE` layers, and smart-object rendering.
6. CI across macOS, Linux (Xvfb + lavapipe, as above) and Windows.

## Third-party components

| Component | License | Use |
| --- | --- | --- |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | MIT | UI toolkit, GLFW and Vulkan backends |
| [GLFW](https://www.glfw.org/) | zlib | Windowing, input, Vulkan surfaces |
| [GLM](https://github.com/g-truc/glm) | MIT | Vector math |
| [Roboto](https://fonts.google.com/specimen/Roboto) (from Dear ImGui's `misc/fonts`) | Apache 2.0 | Embedded UI font |
