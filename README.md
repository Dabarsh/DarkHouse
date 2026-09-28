# DarkHouse

DarkHouse combines four desktop imaging tools in one application: a
**non-destructive RAW developer**, a **high-throughput digital asset manager
(DAM)**, a **layered pixel compositor** and a **vector designer**. They share one
catalog, one GPU pipeline and one document model.

Stack: C++20 · CMake ≥ 3.22 · SQLite 3 (WAL) · Vulkan 1.3 / SPIR-V ·
Dear ImGui (Vulkan backend) · ONNX Runtime

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

## Architecture

```
                         ┌───────────────────────────┐
  FrontEnd (ImGui/Vulkan │       DarkHouseApp        │  modes: CATALOG | CANVAS | HYBRID_SPLIT
  or headless) ────────► │ event queue → frame loop  │
                         └──┬─────────┬─────────┬────┘
                            │         │         │
              ┌─────────────▼──┐ ┌────▼──────┐ ┌▼──────────────────────┐
              │  AssetManager  │ │ LayerNode │ │  RenderPipelineGraph  │
              │ SQLite WAL,    │ │ tree +    │ │  ComputeNode DAG      │
              │ import pool,   │ │ Sparse-   │ │  (ExposureNode, ...)  │
              │ EXIF, XXH64    │ │ Raster-   │ │  on VulkanContext     │
              └────────────────┘ │ Layer     │ └───────────▲───────────┘
                                 └────┬──────┘   dirty FP16 tiles → GPU upload
                                      └──────────────────────┘
                     AISegmentationEngine (ONNX Runtime) → masks
```

| Path | Responsibility |
| --- | --- |
| `schema/catalog.sql` | Catalog schema: `assets`, `metadata`, `edit_nodes`, indexes. Embedded into the binary at configure time. |
| `include/asset_manager.hpp` | `AssetRecord` / `AssetMetadata`, async `importFile()`, parameterized `queryAssets()`, ratings/flags/labels, edit stacks. |
| `include/vulkan_context.hpp` | `PixelFormat`, `GPUTexture`, `VulkanContext` (instance, device, queue, textures, uploads) and synchronization2 barrier helpers. |
| `include/render_pipeline.hpp` | `ComputeNode`, `ExposureNode`, `RenderPipelineGraph`. |
| `shaders/exposure.comp` | Exposure (EV), highlights/shadows and contrast on RGBA16F storage images, in 16×16 workgroups. |
| `include/layer_stack.hpp` | `TILE_SIZE`, FP16 tiles, `SparseRasterLayer`, `LayerNode` tree, blend modes, CPU reference compositor. |
| `include/ai_segmentation.hpp` | `AISegmentationEngine` (subject/sky) and the ONNX Runtime backend. |
| `include/app_controller.hpp` | `DarkHouseApp`, `AppMode`, `AppEvent`, and the `FrontEnd` seam for the UI layer. |
| `src/main.cpp` | Command-line interface. |

### Frame loop

Each frame runs these steps in order:

1. `FrontEnd::pumpPlatformEvents` turns OS input into `AppEvent`s.
2. Queued events are drained. Events are thread-safe to post from anywhere.
3. Finished imports are collected.
4. If the canvas is visible (`CANVAS` / `HYBRID_SPLIT`), dirty document tiles
   are composited, converted to FP16 and uploaded in one submission, and the
   develop graph is re-evaluated only when something changed.
5. `FrontEnd::drawFrame` presents.
6. The loop sleeps to hold the target frame rate.

### Safety notes

- `queryAssets(filter, params)` takes a SQL boolean expression so smart
  collections stay flexible. Pass values through `?` parameters. The filter
  runs on a **read-only** connection with an authorizer that allows only
  reads, and multi-statement input is rejected.
- `ComputeNode::setInputTexture` may reallocate GPU resources, so the graph
  must not be re-wired while a submission that uses it is still in flight.
  `VulkanContext::submitAndWait` guarantees this in the current loop.

## Building

### Prerequisites

| | macOS | Linux (Debian/Ubuntu) | Windows |
| --- | --- | --- | --- |
| Compiler | Xcode 15+ / Apple Clang 15+ | GCC 11+ or Clang 14+ | MSVC 2022 (17.4+) |
| Vulkan 1.3 | [LunarG SDK](https://vulkan.lunarg.com/) or `brew install vulkan-headers vulkan-loader molten-vk` | `libvulkan-dev` + a 1.3 driver | LunarG SDK |
| GLSL → SPIR-V | ships with the SDK, or `brew install shaderc` | `glslc` or `glslang-tools` | ships with the SDK |
| SQLite 3 | system library | `libsqlite3-dev` | `vcpkg install sqlite3` |
| CMake ≥ 3.22 | `brew install cmake` | `apt install cmake` | installer / VS |
| ONNX Runtime (optional) | `brew install onnxruntime` | release tarball | release zip |

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
- `build/generated/catalog_schema.hpp`, the embedded schema.

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `DARKHOUSE_ONNXRUNTIME` | `AUTO` | `AUTO` uses ONNX Runtime if found, `ON` requires it, `OFF` disables it. Set `ONNXRUNTIME_ROOT` for a non-system install. |
| `DARKHOUSE_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` |

### Running

```bash
# Import a folder recursively (RAW, JPEG, TIFF, PNG, ...), then list high-ISO shots
build/bin/DarkHouse --catalog photos.sqlite --import ~/Pictures/2024 --query "m.iso >= 3200"

# Rate an asset, open it on the canvas and run a few frames of the GPU develop graph
build/bin/DarkHouse --catalog photos.sqlite --rate <asset-id> 4 --open <asset-id> --frames 10

# Catalog-only machine (no Vulkan)
build/bin/DarkHouse --no-gpu --catalog photos.sqlite --query ""
```

Put `subject_segmentation.onnx` / `sky_segmentation.onnx` in a directory and
pass `--models <dir>` to enable AI selection. Run `DarkHouse --help` for every
option. Exit codes: `0` success, `1` fatal error, `2` some imports failed,
`64` bad arguments.

## Status

This is the core architecture. What works today:

- **Catalog**: WAL schema, parallel import with dedupe, and metadata from
  TIFF-based RAW (DNG/CR2/NEF/ARW/ORF/RW2/PEF), JPEG/EXIF and PNG. That covers
  camera, lens, exposure, GPS and capture time with its UTC offset. Also
  parameterized queries, ratings, flags, colour labels and edit-stack
  persistence.
- **GPU**: device selection, textures, staging uploads, the compute-node DAG
  with cycle detection and barriers, and the exposure node plus its shader.
- **Layers**: sparse FP16 tiles, dirty tracking, the layer tree with masks and
  groups, and the CPU reference compositor for all five blend modes.
- **App**: event-driven frame loop, three modes, graceful degradation, headless
  batch mode.

Next milestones:

1. Dear ImGui front-end: GLFW window, swapchain and render pass behind
   `FrontEnd`, plus a catalog grid and canvas viewport.
2. RAW decoding (LibRaw): demosaic into the RGBA16F source texture. Add CR3,
   HEIF and RAF metadata.
3. GPU compositing: blend modes and masks as compute nodes, with the CPU
   compositor kept as the parity reference.
4. Vector rasterization of `VECTOR_SHAPE` layers, and smart-object rendering.
5. A test suite in-tree (CTest) and CI across macOS, Linux and Windows.
