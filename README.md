# logos-yolo-board-module (experimental)

A Logos Core module that was meant to own all Yolo Board backend logic (polling, media cache, subscriptions, upload orchestration) so the QML UI could be a thin client doing `logos.callModule("yolo_board_module", ...)` + event subscriptions.

## Status: shelved

The architecture is the right shape — the QML UI should NOT directly reach into `zone-sequencer` and `storage` modules; a domain module should mediate — but making it work in the current Basecamp stack hit **framework-level deadlocks**. See [../zone-sdk-test/docs/FEEDBACK.md](../zone-sdk-test/docs/FEEDBACK.md) §14–19 for details. Briefly:

1. **Sync module-to-module IPC deadlocks** when the caller is already inside an IPC handler (classic event-loop reentrancy). Our `configure()` blocks trying to call `zone-sequencer.set_node_url`, while `capability_module` simultaneously tries to `informModuleToken(yolo_board_module, ...)` — yolo's event loop is busy → 20s timeout per IPC round-trip.

2. **Per-call token re-request** amplifies the problem: `LogosAPIClient`'s token cache doesn't persist between calls (or is per-target not per-client), so every `zoneCall` triggers the full capability_module dance.

3. **SDK-level `ModuleProxy`** is required to receive `informModuleToken` calls, but without `--whole-archive` in the link step, the symbols are stripped from the plugin `.so` → the target module silently can't be informed.

4. **`RTLD_GLOBAL` plugin loading** means even if a plugin's `.so` has the right `ModuleProxy` (with the `LogosResult → JSON` fix), the host process's copy wins — fixes don't deploy per-plugin.

The zone-sequencer-module fork has been updated with `--whole-archive` (fixes #3 above), but #1/#2 require framework changes to either:
- Pre-warm tokens at module startup so no runtime handshake is needed, OR
- Provide a reliably-async `callModuleAsync` + `onModuleEvent` in `LogosQmlBridge` so the UI can talk directly to core modules without blocking.

Once one of those lands in Basecamp, this module becomes viable. Until then, Yolo Board's QML calls `zone-sequencer` and `storage` directly.

## What's in here

- `src/i_yolo_board_module.h` / `.cpp` — Q_INVOKABLE interface: `configure`, `subscribe`, `publish`, `publish_with_attachment`, `fetch_media`, `start_backfill`, etc. plus event emission for `stateChanged`, `messagesChanged`, `channelsChanged`, `statusChanged`, `mediaReady`.
- `src/yolo_board_module.cpp` — full implementation porting the business logic from the old `YoloBoardBackend.cpp` (polling, message cache, backfill, upload orchestration, etc.) adapted for the module lifecycle.
- Standard `CMakeLists.txt` + `flake.nix` + `metadata.json` for a `core` module.

Build works: `nix build .#plugin` produces `libyolo_board_module.so`. Loading works (logoscore confirms it). `configure()` with the dataDir path returns the channel ID immediately. Subsequent IPC calls to zone-sequencer hit the deadlock described above.

## Revisit conditions

Come back to this module when ANY of:
- Basecamp ships `callModuleAsync` + `onModuleEvent` in `LogosQmlBridge`
- SDK provides pre-warmed tokens at module load time
- Capability module's `informModuleToken` becomes fully async (doesn't need target to respond)
- The RTLD_GLOBAL ABI issue is solved so plugins can carry their own SDK

At that point the QML can be reduced to property bindings over `get_state()` plus a handful of `callModule` action invocations, and the domain logic lives in one place.
