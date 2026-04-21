# Inter-Module Communication

> Notes and patterns accumulated while building yolo_board_module against
> the Logos module system (storage_module, zone_sequencer, out-of-process
> QRO IPC via logos_host). Originally maintained as a Claude Code skill;
> copied here as a shareable reference for anyone else writing a Logos
> module. Covers the gotchas that are easy to hit and hard to debug:
> `QStringList` serialization, `LogosResult` typed-vs-raw dispatch, event
> payload shape changes, nested sync IPC re-entrancy (~80 s of hidden
> startup waste), masking latency with disk preload, and the
> back-to-back `QTimer::singleShot(0)` trick for parallel sync IPC.

## Architecture

```
+------------+     Qt Remote Objects (per-process IPC)     +------------+
|  Module A  | ------------------------------------------> |  Module B  |
|  caller    |                                              |  callee    |
+------------+                                              +------------+
       │                                                          │
       │  LogosAPI client                                          │  initLogos() stores LogosAPI*
       │  invokeRemoteMethodAsync()                               │  Q_INVOKABLE methods
       │                                                          │  emit eventResponse(...)
```

## Step 1: Declare Dependencies

`metadata.json`:
```json
{ "name": "my_module", "dependencies": ["calc_module", "storage_module"] }
```

Each entry must match the `name` in that dependency's `metadata.json`.

`flake.nix` — add each as a flake input (input name must match):
```nix
inputs = {
  logos-module-builder.url = "github:logos-co/logos-module-builder";
  calc_module.url = "github:logos-co/logos-tutorial?dir=logos-calc-module";
};
```

## CRITICAL: `--whole-archive` for IPC targets

If your module is the **target** of cross-module calls (other modules invoke methods on it), the `liblogos_sdk.a` static archive must be linked with `-Wl,--whole-archive` so `ModuleProxy`/`LogosProviderObject` symbols aren't garbage-collected by the linker.

Symptom when missing: capability_module logs `ConnectionRefusedError "logos_<your_module>"` repeatedly; modules appear to load but no IPC reaches them.

Verify post-build: `nm <your_module>.so | grep -i ModuleProxy`. If empty, fix `CMakeLists.txt`:
```cmake
target_link_libraries(${PLUGIN_TARGET} PRIVATE
    -Wl,--whole-archive ${LOGOS_CPP_SDK_ROOT}/lib/liblogos_sdk.a -Wl,--no-whole-archive)
```

`logos_module()` from logos-module-builder *should* do this — but verify.

## CRITICAL: QtRO is main-thread bound

`invokeRemoteMethod` and the `*Async` SDK wrappers **must** be called from the main thread (the thread the plugin's QObject lives on). Calls from `QtConcurrent::run`, `std::thread`, `QThreadPool` workers, etc. are silently dropped — no error, no callback.

If you need to do work in the background:
```cpp
// Wrong:
QtConcurrent::run([this]() {
    auto* c = m_logosAPI->getClient("storage_module");
    c->invokeRemoteMethod("storage_module", "uploadUrl", {path, 0});  // dropped
});

// Right — dispatch back to main thread:
QTimer::singleShot(0, this, [this, path]() {
    auto* c = m_logosAPI->getClient("storage_module");
    c->invokeRemoteMethod("storage_module", "uploadUrl", {path, 0});
});

// Or use QMetaObject::invokeMethod with QueuedConnection.
```

## Step 2: C++ → Other Module (LogosAPI)

```cpp
void MyModulePlugin::initLogos(LogosAPI* api) { m_logosAPI = api; }

void MyModulePlugin::doSomething()
{
    auto* client = m_logosAPI->getClient("calc_module");

    // Synchronous (blocks main thread until reply or 20s timeout)
    QVariant result = client->invokeRemoteMethod("calc_module", "add", 1, 2);

    // Async (preferred — non-blocking; callback fires on main thread)
    client->invokeRemoteMethodAsync("calc_module", "add",
        [](QVariant r) { qDebug() << "Got:" << r; }, 1, 2);
}
```

## Step 3: C++ → Other Module (typed SDK)

```bash
logos-cpp-generator --metadata metadata.json --module-dir ./modules --output-dir ./generated
```

```cpp
#include "logos_sdk.h"

void MyModulePlugin::initLogos(LogosAPI* api) {
    m_logos = new LogosModules(api);
}

int sum = m_logos->calc_module.add(1, 2);                       // sync
m_logos->calc_module.addAsync(1, 2, [](QVariant r) { ... });    // async
```

Every `foo()` gets a generated `fooAsync()`.

## Step 4: QML → Core Module (QML-only UI)

```qml
// Direct call
var result = logos.callModule("calc_module", "add", [1, 2])

// Event-based
Component.onCompleted: logos.onModuleEvent("calc_module", "versionReady")

Connections {
    target: logos
    function onModuleEventReceived(moduleName, eventName, data) {
        if (eventName === "versionReady") root.result = data[0]
    }
}
```

## Step 5: QML → C++ Backend (C++ UI module)

```qml
// SLOT call (returns via Promise)
logos.watch(backend.add(1, 2)).then(
    function(v) { root.result = String(v) },
    function(e) { root.errorText = String(e) }
)

// PROP read (auto-synced)
readonly property string status: backend ? backend.status : "Loading..."

// SIGNAL handler
Connections {
    target: backend
    function onErrorOccurred(msg) { root.errorText = msg }
}
```

## Step 6: Emit Events

```cpp
emit eventResponse("eventName", QVariantList() << "data1" << "data2");

// Complex data:
QVariantMap m; m["k"] = "v"; m["n"] = 42;
emit eventResponse("complexEvent", QVariantList() << QVariant(m));
```

## Events: subscribe and decode

Current generator-emitted providers ship event payloads as a **plain
`QVariantList`** — typically `{bool ok, QString message}`, or
`{bool ok, QString sessionId, QString cid}` for upload completion. They
are **not** JSON strings any more (older revisions were). Decode directly:

```cpp
m_storage->on("storageUploadDone", [this](const QVariantList& d) {
    bool ok = !d.isEmpty() && d[0].toBool();
    if (!ok) return;
    QString sessionId = d.size() > 1 ? d[1].toString() : QString();
    QString cid       = d.size() > 2 ? d[2].toString() : QString();
    ...
});
```

Don't parse with `QJsonDocument` — that worked for one generator
revision and breaks for the next. Empty `msg` in `storageStart`
payloads (just `{true, ""}`) is normal.

## Returning `LogosResult` through typed bindings works — raw `invokeRemoteMethod` doesn't

Typed generated methods returning `LogosResult` (`m_storage->uploadUrl(...)`,
`->debug()`, `->manifests()`, etc.) deserialize fine because the
generated wrapper knows how to unpack the `qRegisterMetaType<LogosResult>`
QDataStream stream. **But** reaching for the same method via raw
`LogosAPIClient::invokeRemoteMethod("storage_module", "uploadUrl", ...)`
returns a `QVariant` that holds `LogosResult` with typeName
`"LogosResult"` — and `.toString()` on that returns empty.

So:
- If you must call a `LogosResult`-returning method, use the typed wrapper:
  ```cpp
  LogosResult r = m_storage->uploadUrl(QVariant::fromValue(url), 65536);
  if (!r.success) { /* r.error.toString() */ }
  QString sessionId = r.value.toString();
  ```
- Only reach for raw `invokeRemoteMethod` for methods returning plain
  types (`bool`, `QString`, `int`) or for the `*Json` workaround wrappers
  older storage revisions ship with. Check `nm -D module.so | grep` to
  confirm the wrapper exists before relying on it.

## Headless visibility

`qInfo`/`qWarning` from a module's own process (the one `logos_host`
spawns) sometimes **fails to reach** logoscore's aggregated stdout in
our current rig — you'll see `StorageModuleImpl::init called` because
it's `fprintf(stderr, ...)`, but no matching qDebug lines from your own
module. The server-side storage logs (libstorage) DO appear because
they use stderr directly.

Workaround: file-based diagnostics are reliable. A 20-line helper like

```cpp
static void ybmDiag(const QString& line) {
    QFile f(QStringLiteral("/tmp/yolo_board_module.diag"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                        << " " << line << "\n";
    }
}
```

lets you trace `initLogos` → event subscribe return values → event
delivery without fighting with Qt message routing. Reset the file at
the top of `initLogos` with `QFile::remove`. Keep it in during bring-up;
gate behind a build define when you commit.

## Binary / header version alignment

When your client module is compiled against one revision of
`storage_module_api.{h,cpp}` (via the nix flake input) but `logos_host`
loads a **different** `storage_module_plugin.so` from Basecamp's
`~/.local/share/Logos/LogosBasecampDev/modules/`, method dispatch
silently returns empty results (no warning, no log). Symptom: typed
calls return empty `QVariant`; events may or may not flow.

Fix: copy the plugin from your flake input into Basecamp's modules dir:

```bash
cp /nix/store/<hash>-logos-storage_module-module/lib/storage_module_plugin.so \
   ~/.local/share/Logos/LogosBasecampDev/modules/storage_module/
cp /nix/store/<hash>-logos-storage_module-module/lib/libstorage.so \
   ~/.local/share/Logos/LogosBasecampDev/modules/storage_module/
chmod u+w ~/.local/share/Logos/LogosBasecampDev/modules/storage_module/*.so
```

`nm -D module.so | grep <method>` confirms which methods the loaded
binary actually exposes. If `uploadUrlJson` is missing it's the newer
typed-only build; drive it through the typed SDK instead of Json wrappers.

## Startup ordering for out-of-process modules

`QTimer::singleShot(0, ...)` in `initLogos` breaks the IRC pattern and
will cause `initAsync` callbacks to silently drop. Construct the typed
`LogosModules` / `StorageModule` **inline** in `initLogos`, subscribe
events **inline**, and only defer `configure()`-reachable IPC work via a
single outer `QTimer::singleShot(0, ...)` because that function is
reached via QRO and a sync reentrant call would deadlock the QRO reply
thread. For `connect` / `uploadUrl` / `downloadToUrl` — fire-and-forget
with event-based completion.

## CRITICAL: `QStringList` does not round-trip via raw `invokeRemoteMethod`

Wrapping a `QStringList` with `QVariant::fromValue(...)` and passing it through
the raw `LogosAPIClient::invokeRemoteMethod(...)` path does **not** deserialize
to `std::vector<std::string>` on the replica. The replica never invokes the
method — the caller's sync `invokeRemoteMethod` simply times out after ~20 s
with `callRemoteMethod failed or timed out`. Symptom in the UI: clicking
"Connect" or any action that sends a list of strings appears to hang the UI
for 20 s, then fails silently.

Three reliable fixes, in order of preference:

1. **Use the typed SDK wrapper** (`m_logos->module.connect(peerId, addrs)` where
   `addrs` is a plain `QStringList`). The generator emits code that marshals it
   correctly. This is the long-term right answer.
2. **If stuck on raw `invokeRemoteMethod`**, pass the list as a
   `QVariantList` of `QString`s (not a `QVariant`-wrapped `QStringList`):
   ```cpp
   QVariantList addrVariants;
   for (const QString& a : addrs) addrVariants.append(a);
   client->invokeRemoteMethod("storage_module", "connect", {peerId, addrVariants});
   ```
3. **Use the Json wrapper pattern** (below) to pass a serialized array.

Found via headless reproduction 2026-04-20: Basecamp Yolo Board's
`storage_module.connect(peerId, addrs)` hung; storage-UI calling the same
method through typed bindings returned immediately.

## ANTI-PATTERN: polling instead of subscribing

Do **not** poll `manifests()` in a loop waiting for an upload to appear. The
storage module emits `storageUploadDone` (and `storageUploadProgress`,
`storageDownloadDone`, `storageStart`, `storageStop`, `storageConnect`) as QRO
events — subscribe with `.on(name, callback)` on the typed wrapper:

```cpp
m_logos->storage_module.on("storageUploadDone", [this](const QVariantList& d) {
    bool success = d[0].toBool();
    QString cid = d[2].toString();
    // emit your own signal, update state, etc.
});
QString sessionId = m_logos->storage_module.uploadUrl(path, 0).value.toString();
// Return immediately; result arrives via the event handler.
```

Polling serializes sync calls on the single QRO outgoing channel and stacks
behind any other outstanding sync call (e.g. a `connect` that's still dialing).
Events are multiplexed separately and don't suffer this contention.

Events are only available on the **typed wrapper** (generated per-module
`XxxModule` class). Raw `LogosAPIClient` has no `.on(...)`. If your module
needs events from another module, you must use the typed SDK — there is no
workaround.

## CRITICAL: `LogosResult` Qt-IPC serialization is broken

`LogosResult` returns from cross-process calls arrive as empty `QVariant()`. Until the SDK fix lands, **work around with JSON wrapper methods that return `std::string`**.

In the module:
```cpp
// In your interface header:
Q_INVOKABLE virtual std::string uploadUrlJson(const std::string& filePath, int64_t chunkSize) = 0;

// In your impl:
static std::string stdLogosResultToJson(const StdLogosResult& r) {
    nlohmann::json j;
    j["success"] = r.success;
    if (r.success) j["value"] = r.value; else j["error"] = r.error;
    return j.dump();
}

std::string MyPlugin::uploadUrlJson(const std::string& path, int64_t cs) {
    return stdLogosResultToJson(uploadUrl(path, cs));   // delegate to real method
}
```

The codegen automatically maps `std::string` ↔ `QString` for IPC, so the caller gets a valid `QString` JSON payload back and parses it:
```cpp
QString r = client->invokeRemoteMethod("storage_module", "uploadUrlJson", {path, 0}).toString();
QJsonDocument doc = QJsonDocument::fromJson(r.toUtf8());
bool ok = doc.object()["success"].toBool();
```

This pattern lives in the storage_module fork and is the proven workaround as of April 2026.

## Nested sync IPC re-entrancy — 80 s of hidden startup waste

Sync IPC spins a nested `QEventLoop` while waiting for the reply. **Event
handlers, UI state polls, and other timers fire inside that nested loop.**
If any of them make sync calls on the *same replica* (e.g. a second
`m_storage->debug()` while the first is still in flight), the calls stack
up. The storage replica serializes them — so the second call waits for
the first's 40 s QRO timeout before it even gets to try. On yolo_board_module
startup this cost **~80 s of pure dead wall time** before it was fixed.

Measured pattern that caused it:
- `initStorage` polled fallback: `start()` returned → `refreshStorageInfo()` → sync `debug()` spinning.
- `storageStart` event fires during the spin → handler also calls `refreshStorageInfo()` → sync `debug()` nested.
- UI poll timer fires `get_state()` every 500 ms → lazy retry calls `debug()` again.
- All three pile up on the same replica. Two 40 s QRO timeouts burn before the third succeeds.

Defences:
- **Guard re-entrancy** with a `bool m_refreshing` flag; early-return if already in flight.
- **Short-circuit once populated** (`if (peerIdKnown && addrsKnown) return;`) so UI polls stop hammering.
- **Prefer async** (`fooAsync`) for anything called from event handlers or polls — async replies don't nest loops.

## Mask sync IPC latency with disk preload

If the slow path is sync IPC to another process, check whether persisted
state on disk already answers the question. In yolo_board_module,
`configure()` knew the channel id from `${dataDir}/channel.id`, so it could
call `loadCacheForChannel()` + `loadSubscriptions()` + `emitChannelsChanged()`
**before** the ~80 s sequencer init finished. UI renders in <1 s instead
of sitting empty. The post-IPC-done path still runs but is gated so it
only re-hydrates if the cache was actually cold.

Rule of thumb: anything that only needs to be "eventually consistent" with
what IPC will return is a preload candidate. Messages, channel lists,
peer lists, identity info — all live on disk already.

## Parallel sync IPC via back-to-back `QTimer::singleShot(0, …)`

When a module needs sync IPC to *two independent* modules (zone_sequencer +
storage_module in our case), schedule both via independent singleShots
scheduled from the same outer function. The first one runs and enters its
sync IPC nested event loop; the second singleShot's timer fires during
that spin and runs concurrently. Net: `max(A, B)` instead of `A + B`.

```cpp
QTimer::singleShot(0, this, [this]() { initSequencer(); /* sync zoneCall */ });
QTimer::singleShot(0, this, [this]() { initStorage();   /* sync storage init+start */ });
```

Caveat: only "parallel" if the two modules are distinct replicas. Two
sync calls on the *same* replica still serialize.

## CRITICAL: Long-running blocking methods

If a SLOT does sync work that takes > a few hundred ms (libstorage's `start()` blocks ~30s for discovery + transport bind), the IPC caller's main thread is blocked the same amount.

For native lib startup that's actually async-callback-based but appears blocking, detach to `std::thread`:

```cpp
bool MyPlugin::start() {
    auto* ctx = new SimpleEventCtx(this, "myStart");
    auto* sctx = m_storageCtx;
    std::thread([sctx, ctx]() {
        if (native_start(sctx, asyncDispatch, ctx) != RET_OK) delete ctx;
    }).detach();
    return true;   // IPC returns immediately; readiness via "myStart" event
}
```

Caller side: subscribe to the completion event OR retry calls until they stop returning "not ready". See `basecamp-deploy` for the storage example.

**Measured reality: storage_module's `init()` also blocks ~20 s** (not
just `start()`). So a caller that runs `init()` then `start()` burns
~40 s on the caller's main thread even though each one returns in
"milliseconds". If you're budgeting startup time, count both. On
yolo_board_module the floor from `configure()` to "storage ready" is
~60 s, of which 40 s is pure libstorage sync IPC. There's no shave
below that without changing storage_module itself.

## `LogosResult` Helpers (when not using JSON wrapper)

```cpp
LogosResult r = m_logos->other.fetchData(id);
if (r.success) {
    QString name = r.getString("name");
    int n = r.getInt("count", 0);
    QVariantMap m = r.getMap();
} else {
    QString e = r.getError();
}
```

## Communication Mode

| Mode | Use Case | How to Set |
|------|----------|------------|
| **Remote** (default) | Desktop apps | Modules in separate processes, QtRO IPC |
| **Local** | Mobile / single-process | All modules in one process, `PluginRegistry` |

```cpp
LogosModeConfig::setMode(LogosMode::Remote);
LogosModeConfig::setMode(LogosMode::Local);
```

## Best Practices

1. **Prefer async** — `invokeRemoteMethodAsync` / `fooAsync()` to avoid blocking main thread
2. **Use typed SDK** when available — type-safe, no `QVariant` juggling
3. **Check `client != nullptr`** — module may not be loaded yet
4. **JSON-wrap any `LogosResult` return** until the SDK fix lands
5. **Keep blocking IPC out of background threads** — main-thread only
6. **Detach native blocking calls** to `std::thread` so your IPC returns fast
7. **Subscribe to events early** in `initLogos()`

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `client` is null | Module not loaded yet, or missing from `dependencies` |
| Method not found | Method not `Q_INVOKABLE` in target |
| Async callback never fires | Target crashed, or call from worker thread (silently dropped) |
| Sync call times out after 20 s with list arg | `QVariant::fromValue(QStringList)` doesn't deserialize — use `QVariantList` of `QString`s, or the typed SDK |
| Upload appears stuck "uploading…" forever | You're polling `manifests()` instead of subscribing to `storageUploadDone` — switch to typed SDK + `.on()` |
| `ConnectionRefusedError "logos_<module>"` storms in logs | Target module missing `--whole-archive` (no `ModuleProxy` symbols) |
| `LogosResult` returns empty | Use the `*Json` wrapper pattern |
| `LogosAPI not available` warning | capability_module's `informModuleToken` failed — same root cause as ConnectionRefused |
| Type mismatch | Argument types don't match signature |
| Deadlock | Sync call inside another sync call's callback — switch to async |
| Startup takes minutes; multiple 40 s gaps in timing logs | Nested sync IPC re-entrancy — guard with a "refreshing" flag, short-circuit once populated, prefer async from event handlers |
| UI empty during long init | Preload cached state from disk in `configure()` before kicking off sync IPC; emit state events so QML renders immediately |

## Final Checklist

- [ ] Dependencies in `metadata.json` and `flake.nix` match by name
- [ ] `LogosAPI*` stored in `initLogos()`
- [ ] `nm` confirms `ModuleProxy` symbols in target module's `.so`
- [ ] Async calls preferred over sync
- [ ] Typed SDK used where available
- [ ] All `LogosResult` returns wrapped with `*Json` for now
- [ ] No IPC calls from worker threads
- [ ] Long-running native calls detached to `std::thread`
- [ ] Event subscriptions set up
- [ ] Missing-module / null-client handled gracefully
