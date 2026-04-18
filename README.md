# logos-yolo-board-module

A Logos Core module that owns all Yolo Board backend logic — polling, message cache,
subscription persistence, upload orchestration, media cache. The QML UI is a thin
client that does `logos.callModule("yolo_board_module", ...)` plus event subscriptions.

## Status: working

The architecture took several iterations to land — the original attempt hit
framework-level deadlocks documented in [../zone-sdk-test/docs/FEEDBACK.md](../zone-sdk-test/docs/FEEDBACK.md)
§14–19. The combination of fixes that made it work:

1. **`--whole-archive` in CMakeLists.txt** so `ModuleProxy` symbols aren't
   stripped from the plugin `.so` → capability_module's `informModuleToken` calls
   actually reach this module.
2. **All cross-module IPC dispatched on the main thread via `QTimer::singleShot(0, this, ...)`**
   instead of `QtConcurrent::run` — `QRemoteObjects` clients are main-thread bound
   and silently drop calls from worker threads.
3. **JSON wrapper methods on storage_module** (`uploadUrlJson`, `manifestsJson`,
   `downloadFileJson`, `existsJson`) returning `std::string` to dodge the
   broken `LogosResult` Qt-IPC serialization.
4. **Detached `storage_module.start()`** (in our storage_module fork) so the
   ~30 s libstorage startup doesn't block this module's main thread.
5. **`set_ui_dir(qmlPath)`** invoked from the QML side at startup so
   `resolve_media` can copy cached media files into the QML plugin's allowed
   root — the only file:// location the Basecamp QML host accepts.

## What this module exposes

`Q_INVOKABLE` API (see `src/i_yolo_board_module.h`):

- **Lifecycle**: `initLogos`, `configure(dataDir, nodeUrl)`, `load_saved_config`, `set_ui_dir`
- **State snapshots**: `get_state()`, `get_messages(channelId)`, `get_channels()` — return JSON strings the QML side polls
- **Channels**: `subscribe(idOrName)`, `unsubscribe(id)`, `clear_unread(id)`
- **Publishing**: `publish(text)`, `publish_with_attachment(text, filePath)`
- **Media**: `resolve_media(cid)`, `fetch_media(cid)`
- **Control**: `reset_checkpoint`, `start_backfill(channelId)`, `stop_backfill(channelId)`

Events emitted as `eventResponse` signals:

- `stateChanged`, `messagesChanged`, `channelsChanged`, `statusChanged`, `mediaReady`

## How it talks to the world

```
Basecamp QML host                       Module process (logos_host)
┌──────────────────────┐                ┌─────────────────────────────────┐
│  Main.qml            │   callModule    │  yolo_board_module              │
│  poll get_state()    │ ─────────────> │   ├─ persistent state            │
│  poll get_messages() │                 │   ├─ poll zone-sequencer (1.5s) │
│  call publish_*()    │                 │   ├─ media cache                │
└──────────────────────┘                 │   └─ upload orchestration       │
                                         └────────┬───────┬───────────────┘
                                                  │       │
                                            QtRO  │       │  QtRO
                                                  ▼       ▼
                                  liblogos_zone_sequencer  storage_module
                                  (publishes inscriptions, (Codex node,
                                   queries channels)        upload + retrieval)
```

All cross-process IPC happens on this module's main thread via `QTimer::singleShot`
or repeating `QTimer` for polling — never from `QtConcurrent`.

## Auto-connect

On `initLogos`, the module loads `~/.config/logos/yolo_board.json` if present and
auto-runs `configure()` with the saved `dataDir` + `nodeUrl`. Subsequent
`get_state()` polls report `connected: true` once the zone-sequencer responds
and `storageReady: true` once libstorage's start completes.

## Build

```bash
nix build              # produces yolo-board-module.lgx
```

The `--whole-archive` link is set in `CMakeLists.txt`. If you fork or refactor,
verify with `nm libyolo_board_module.so | grep -i ModuleProxy` — if empty, IPC
won't work.

## Install

Use the `/basecamp-deploy` Claude Code skill, or manually:

```bash
LGPM=…/logos-package-manager/scripts/lgpm
BASECAMP=~/.local/share/Logos/LogosBasecampDev
$LGPM --modules-dir $BASECAMP/modules --ui-plugins-dir $BASECAMP/plugins \
      install --file ./result/yolo-board-module.lgx
sed -i 's/"linux-amd64"/"linux-amd64-dev"/g; s/"linux-x86_64"/"linux-x86_64-dev"/g' \
      $BASECAMP/modules/yolo_board_module/manifest.json
```

## Caveats / known issues

- `storage_module.start()` blocks ~30 s on first launch (discovery + transport
  bind). Our forked storage_module detaches it; without that fork, this
  module's main thread is blocked the same ~30 s and `publish_with_attachment`
  retries `uploadUrlJson` until libstorage is up.
- `resolve_media` only works for files this module has cached locally
  (own-channel uploads + previously-fetched downloads). Cross-channel images
  trigger `fetch_media` → `runDownload`, which only completes if storage has
  peers serving the CID.
- `LogosResult` returns from upstream modules are still broken — every storage
  call we make goes through the `*Json` wrappers. If new storage methods are
  needed, add JSON wrappers in storage_module first.
- Backfill (`runBackfill`) still uses `QtConcurrent::run` for the loop body —
  it works because it dispatches IPC results back via
  `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)`, but the
  cleaner fix would be a single repeating `QTimer` like `runUpload`.
