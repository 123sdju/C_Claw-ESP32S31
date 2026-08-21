# Main Component Architecture

Current status: this file summarizes `main/` only. The global implementation
map, SDK/firmware boundary contract, and known architecture risks live in
`../docs/全局实现细节.md`. For interview preparation, see
`../docs/嵌入式面试深度指南.md`; for a beginner-friendly walkthrough, see
`../docs/嵌入式新手完整学习指南.md`.

`main` remains one ESP-IDF component. The folders below are ownership
boundaries, not separate components. All source files in `main/` have been
annotated with detailed Chinese function-level comments covering purpose,
parameters, return values, and side-effects.

## Layout

- `core/`: composition root, app context, lifecycle, status, config, runtime feature aggregation.
- `services/`: application services for runtime, interaction, commands, network, USB storage, OTA, event routing, and Lua runtime.
- `board/`: board adapters for display, UI, camera, font loading, snapshots, and Lua screen drawing.
- `transports/`: Web console and serial console. Transports parse protocol input and call service/context callbacks.
- `tools/`: agent-facing tools and `app_tool_registry`.

## Dependency Direction

`core` wires the application. `transports` do not call board or low-level OTA
manager APIs directly; they go through service/context callbacks. `board` does
not own config, runtime, network, or OTA flow.

## Config Surface

`model.capabilities` declares intended model input/output modalities, while
`model.tls.verify` controls device-side HTTPS certificate verification for the
current provider endpoint. Top-level `multimodal` applies the runtime modality
switches and artifact limits used by the SDK runtime.

`storage.root`, `storage.sessions`, `storage.memory`, and `storage.logs` must be
device paths such as `/usb0/cclaw/...`; host paths like `C:/...` are rejected.
`system`, `events`, and `tools` own prompt, event routing, and tool enablement
for the product surface.

## Comment Status

All `main/` source files (~75 files, 300+ functions) now carry detailed Chinese
function-level comments. See `../README.md` Comment Policy for format and
maintenance rules. This is a new architecture — old `drivers/*`, legacy config
fields, compatibility shims, legacy Web routes, and legacy tool names are not
preserved.
