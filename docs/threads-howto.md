# Yolo Board — per-message threads (Waku-backed) — how to test

A "💬" button next to every confirmed Yolo message opens a side panel
chat scoped to **that exact message**. Click 💬 on the same message from a
second Basecamp instance and you join the same room. Conversation flows
over the Waku relay (`logos-delivery-module`), keyed by a content topic
deterministically derived from `(parent_channel_id, parent_msg_id)`.

This doc is the install + smoke recipe.

## Repos / branches involved

| Repo | Branch | What's there |
|---|---|---|
| [vpavlin/logos-yolo-board-module](https://github.com/vpavlin/logos-yolo-board-module) | `master` | Module (out-of-process Logos plugin). Q_INVOKABLE thread API + delivery_module IPC wiring. Two commits ([build glue](https://github.com/vpavlin/logos-yolo-board-module/commit/9b1a4cf), [feature](https://github.com/vpavlin/logos-yolo-board-module/commit/b2f32ca)). |
| [vpavlin/zone-sdk-test](https://github.com/vpavlin/zone-sdk-test) | `basecamp` | Yolo Board UI plugin (QML). Thread panel, "My Threads" dialog, per-message 💬 button. [One commit](https://github.com/vpavlin/zone-sdk-test/commit/5226e2b3). |
| [logos-co/logos-delivery-module](https://github.com/logos-co/logos-delivery-module) | `master` (rev `8f521378`) | Upstream — **not modified**. We consume it as-is via QRO IPC. |

## Build + install (Linux x86_64, NixOS / nix-on-anything)

Prereqs already on your machine: `nix`, `lgpm`, Basecamp dev build at
`~/.local/share/Logos/LogosBasecampDev/`.

```bash
# 1. Build + install delivery_module .lgx (one-time)
LGX=$(nix build github:logos-co/logos-delivery-module#lgx --print-out-paths --no-link)
lgpm --modules-dir ~/.local/share/Logos/LogosBasecampDev/modules \
     --allow-unsigned install \
     --file $LGX/logos-delivery_module-module-lib.lgx

# 2. Build + install yolo_board_module
LGX=$(nix build github:vpavlin/logos-yolo-board-module --print-out-paths --no-link)
lgpm --modules-dir ~/.local/share/Logos/LogosBasecampDev/modules \
     --allow-unsigned install \
     --file $LGX/yolo-board-module.lgx

# 2b. Patch yolo's manifest variants (yolo's flake strips "-dev" suffix
# the way Basecamp's host expects). One-liner:
python3 - <<'PY'
import json
p = '/home/vpavlin/.local/share/Logos/LogosBasecampDev/modules/yolo_board_module/manifest.json'
m = json.load(open(p))
m['main'] = {k.replace('linux-amd64', 'linux-amd64-dev').replace('linux-x86_64', 'linux-x86_64-dev'): v
             for k, v in m['main'].items() if 'dev' not in k}
json.dump(m, open(p, 'w'), indent=2)
PY

# 3. Build + install the UI plugin .lgx
LGX=$(nix build github:vpavlin/zone-sdk-test --print-out-paths --no-link)
lgpm --modules-dir ~/.local/share/Logos/LogosBasecampDev/plugins \
     --allow-unsigned install \
     --file $LGX/yolo-board.lgx
# (or just copy src/qml/Main.qml directly into the plugins dir for hot iteration)
```

You should now have `delivery_module/` + `yolo_board_module/` under
`~/.local/share/Logos/LogosBasecampDev/modules/` and `yolo_board/` under
`…/plugins/`.

## Launching

```bash
~/devel/github.com/logos-co/logos-workspace/repos/logos-basecamp/result/bin/logos-basecamp
```

(Or whatever path you have for the Basecamp launcher; it must be the
`logos-basecamp` shell wrapper, not the inner `LogosBasecamp` binary —
the wrapper sets `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH`.)

In Basecamp, click the **Yolo** plugin in the side bar.

## What "ready" looks like

Top-bar status icons (left to right): sequencer (⬡), storage (▤),
**delivery (◈)**.

| Icon state | Meaning |
|---|---|
| Grey | Not started |
| Pulsing orange | Starting |
| Solid orange | Ready |

Wait until **all three are solid orange** (~60-90 s on first run; Waku
node startup dominates). The delivery icon's tooltip says *"My threads
(click to browse)"* when ready.

`tail -f /tmp/yolo_board_module.diag` mirrors the module's internal
state machine if you want a live log:

```
EVT connectionStateChanged status=Connected connected=1
initDelivery: start() ok=1
configure: initDelivery done deliveryReady=1
EVT storageStart ok=1
configure: initSequencer done ownChannel=…
```

## Test flow

### A. Single-instance — does the panel open and a message round-trip?

1. Click any **confirmed** message in the main feed (your own or one
   that already arrived via the zone sequencer). The 💬 button is to
   the right of the timestamp; hit area is generous (48×28) so it's
   easy to click without grabbing the scrollbar.
2. The right-side panel slides in **immediately** showing the parent
   message preview at the top + an orange "**Connecting to relay…**"
   strip with a pulsing dot.
3. After ~20 s the strip disappears (subscribe acked).
4. Type something + hit Send. The button changes to "**Sending…**" with
   a pulsing dot and is disabled.
5. ~20 s later the message appears with a small orange **✓** next to
   the nickname — *that's the network delivery confirmation.* It means
   the Waku relay accepted your message and either fired
   `messageSent` or echoed it back via `messageReceived`.
6. Close the panel (X). Reopen the same thread → your message is still
   there (in-RAM cache survives close/reopen for the session).

### B. Two instances on the same network — actual chat

Two independent Basecamp installs on the same LAN (or on the same machine
with two separate `dataDir`s):

1. Both connect to the same zone-sequencer node URL.
2. Instance A posts a message on a channel; note the message id (visible
   in the diag log when it's published, or you can just hover the row).
3. Instance B subscribes to A's channel (Yolo channel sidebar), sees A's
   message arrive.
4. Instance A clicks 💬 on its own message → posts "hi from A".
5. Instance B clicks 💬 on the **same** message → A's "hi from A"
   should appear within seconds (Waku gossip latency).
6. B replies → A sees it.

### C. Persistence

- Click 💬 on a message, send a reply, close the panel.
- Open the **My Threads** dialog (click the ◈ icon in the header).
- The thread you just used is listed. Click it → the panel reopens with
  history.
- Restart Basecamp. The My Threads list survives. (Message contents do
  not — that's session-only RAM today; cross-session disk persistence
  is the obvious next step if you need it.)

### D. Visual cues to verify

| Indicator | Where | What it means |
|---|---|---|
| ◈ orange (header) | Top bar | Delivery node connected |
| ◈ pulsing | Top bar | Delivery starting (during init) |
| 💬 high-opacity + orange dot | Per-message button | You've participated in this message's thread |
| 💬 low-opacity, no dot | Per-message button | Thread never opened by you |
| "Connecting to relay…" strip | Thread panel header | Waiting for module's `subscribe` ack |
| "you · sending…" + faded text | Thread message | Waku send in flight (~20 s) |
| ✓ next to nick | Thread message | Network delivery confirmed |
| Red strikethrough | Thread message | Send failed |

## Known limitations (don't be surprised)

- **Send round-trip is ~20 s.** That's `delivery_module`'s server-side
  `CALLBACK_TIMEOUT` waiting for Waku's send confirmation. Feels slow
  but it's an honest signal — the ✓ that follows means the relay really
  has it. Async/no-confirm send would feel snappier but lose this
  guarantee.
- **No signing yet.** The payload includes `nick` (first 8 chars of
  channel id), `text`, `ts`, `id`. Nobody verifies the nick is who they
  claim to be — anyone subscribed to the topic can post as anyone.
  Easy v2 add: sign the payload with the same Ed25519 key that signs
  zone inscriptions.
- **No history backfill.** If you join a thread late, you only see new
  messages. Waku has a store protocol but `delivery_module` doesn't
  expose it via QRO yet.
- **Cross-session message memory is RAM-only.** Restart Basecamp →
  thread message buffers reset (the participated-threads list survives
  via disk, just not the message content).
- **Logos.dev preset bootstrap nodes can be flaky.** If the ◈ icon
  pulses indefinitely or sends never confirm, check
  `tail -f /tmp/yolo_board_module.diag` for `START_NODE failed` or
  `rendezvous failed`. Worst case: kill leftover `logos_host` processes
  (`pkill -9 -f .logos_host-wra`), `rm -f
  /home/<user>/devel/tmp/yolo/storage/dht/providers/LOCK`, restart.

## Troubleshooting

- **Yolo plugin doesn't load** → check `~/.local/share/Logos/LogosBasecampDev/modules/yolo_board_module/manifest.json`'s `main` keys are `linux-amd64-dev` (not `linux-amd64`). Re-run the python patcher above.
- **"Address already in use" in delivery startup logs** → another
  `logos_host --name delivery_module` is still bound to ports
  61000 (TCP) / 9009 (UDP). `pkill -9 -f .logos_host-wra` and relaunch.
- **Panel opens but stays "Connecting…" forever** → Waku has no peers.
  Check `[LOGOS_HOST "delivery_module"]: ... rendezvous` in Basecamp's
  stderr. Could be a transient logos.dev bootstrap outage.
- **`pending → failed` immediately on send** → delivery server returned
  `success=false` within timeout. Inspect delivery's stderr for the
  underlying error (usually a Waku peer/protocol issue).

## Architecture (one-paragraph version)

The module declares `delivery_module` as a metadata dependency so
`logos_host` loads it before us. We acquire its replica via
`LogosAPIClient::requestObject("delivery_module")`, bind event
callbacks, then on `configure()` defer-init the Waku node
(`createNode` + `start`) with a 90 s QRO timeout so the 22-40 s startup
fits inside one round-trip. Subscribe / unsubscribe / send are wrapped
in `QTimer::singleShot(0, …) → invokeRemoteMethodAsync` so our own
Q_INVOKABLEs return to QML in microseconds while the actual Waku call
runs on the next event-loop tick. Topic derivation is
`/yolo/1/thread-<sha256("yolo:thread:v1\0" || parent || "\0" ||
msgId)[:16]>/proto`. Inbound `messageReceived` events with our own
`id` flip a `confirmed` flag on the local message — that's the ✓ in
the UI.
