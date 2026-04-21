#include "yolo_board_module.h"
#include "logos_api.h"
#include "logos_api_client.h"
#include "storage_module_api.h"

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QUuid>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QUrl>

const char* YoloBoardModule::kZoneModuleName = "liblogos_zone_sequencer_module";
const char* YoloBoardModule::kStorageModuleName = "storage_module";

static QString uiConfigPath() {
    QString dir = QDir::homePath() + "/.config/logos";
    QDir().mkpath(dir);
    return dir + "/yolo_board.json";
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

YoloBoardModule::YoloBoardModule() {
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &YoloBoardModule::pollNextChannel);
}

YoloBoardModule::~YoloBoardModule() {
    m_alive->store(false);
    if (m_pollTimer) m_pollTimer->stop();
    for (auto& c : m_backfillCancelled) c->store(true);
    QThreadPool::globalInstance()->waitForDone(5000);
}

static void ybmDiag(const QString& line);  // fwd decl; defined below

void YoloBoardModule::initLogos(LogosAPI* api) {
    if (logosAPI) return;
    logosAPI = api;
    qInfo() << "YoloBoardModule: initLogos called";
    // Reset diagnostic log per plugin load for easier tailing.
    QFile::remove(QStringLiteral("/tmp/yolo_board_module.diag"));
    ybmDiag(QStringLiteral("initLogos begin"));

    // Load saved config so get_state returns the persisted dataDir/nodeUrl
    // even before the user clicks Connect. The UI can then show them and
    // auto-trigger configure().
    {
        QFile f(uiConfigPath());
        if (f.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                m_dataDir = obj["dataDir"].toString();
                m_nodeUrl = obj["nodeUrl"].toString();
                m_savedPeerId = obj["peerId"].toString();
                m_savedPeerAddrs = obj["peerAddrs"].toString();
                qInfo() << "YoloBoardModule: loaded saved config dataDir=" << m_dataDir
                        << "nodeUrl=" << m_nodeUrl
                        << "peerId=" << m_savedPeerId;
            }
        }
    }

    // Construct the typed storage wrapper and subscribe its events
    // IMMEDIATELY in initLogos — mirroring logos-irc-module's pattern. We
    // are NOT inside a Q_INVOKABLE IPC handler here (logos_host calls us
    // directly during plugin load), so the QRO-servicing-thread-deadlock
    // concern doesn't apply yet and we can call sync operations freely.
    //
    // StorageModule's ctor is cheap — it only calls logosAPI->getClient()
    // and stores the pointer. The first blocking call (ensureReplica via
    // .on()) happens during subscribeStorageEvents(); storage_module is a
    // declared dependency in metadata.json, so its QRO source is already
    // published by the time we get here.
    m_storage = new StorageModule(logosAPI);
    ybmDiag(QStringLiteral("StorageModule constructed"));
    subscribeStorageEvents();
    ybmDiag(QStringLiteral("subscribeStorageEvents done"));

    m_zoneClient = logosAPI->getClient(kZoneModuleName);
    ybmDiag(QStringLiteral("zoneClient obtained"));

    // Auto-connect with saved config. configure() defers its body via one
    // QTimer::singleShot(0), which is still needed (configure is reachable
    // via Q_INVOKABLE from QML/logoscore later; same entry point used here
    // to keep behavior consistent).
    if (!m_dataDir.isEmpty() && !m_nodeUrl.isEmpty() && !m_connected) {
        qInfo() << "YoloBoardModule: auto-connecting with saved config";
        configure(m_dataDir, m_nodeUrl);
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

QVariant YoloBoardModule::zoneCall(const QString& method, const QVariantList& args) {
    qInfo() << "YoloBoardModule::zoneCall" << method << "logosAPI=" << (logosAPI != nullptr);
    if (!m_zoneClient && logosAPI) {
        m_zoneClient = logosAPI->getClient(kZoneModuleName);
        qInfo() << "YoloBoardModule::zoneCall got client:" << (m_zoneClient != nullptr);
    }
    if (!m_zoneClient) {
        qWarning() << "YoloBoardModule::zoneCall no client!";
        return {};
    }
    qInfo() << "YoloBoardModule::zoneCall invoking" << method;
    QVariant r = m_zoneClient->invokeRemoteMethod(kZoneModuleName, method, args);
    qInfo() << "YoloBoardModule::zoneCall returned for" << method;
    return r;
}

QString YoloBoardModule::encodeChannelName(const QString& name) {
    static const QByteArray prefix = QByteArrayLiteral("logos:yolo:");
    QByteArray raw = prefix + name.toUtf8();
    if (raw.size() > 32) return {};
    raw = raw.leftJustified(32, '\0');
    return QString::fromLatin1(raw.toHex());
}

QString YoloBoardModule::decodeChannelName(const QString& hexId) {
    QByteArray bytes = QByteArray::fromHex(hexId.toLatin1());
    if (bytes.size() != 32) return {};
    static const QByteArray prefix = QByteArrayLiteral("logos:yolo:");
    if (!bytes.startsWith(prefix)) return {};
    QByteArray name = bytes.mid(prefix.size());
    int end = name.size();
    while (end > 0 && name[end - 1] == '\0') --end;
    return QString::fromUtf8(name.left(end));
}

QString YoloBoardModule::channelDisplayName(const QString& channelId) {
    QString name = decodeChannelName(channelId);
    if (!name.isEmpty()) return name;
    if (channelId.length() > 12) return channelId.left(12) + QStringLiteral("\u2026");
    return channelId;
}

QString YoloBoardModule::mediaCacheDir() const {
    if (m_dataDir.isEmpty()) return {};
    return m_dataDir + "/media_cache";
}

QString YoloBoardModule::mediaCachePath(const QString& cid) const {
    QString dir = mediaCacheDir();
    if (dir.isEmpty()) return {};
    return dir + "/" + cid;
}

QString YoloBoardModule::cacheFilePath(const QString& channelId) const {
    if (m_dataDir.isEmpty()) return {};
    return m_dataDir + "/cache/" + channelId + ".json";
}

QVariantMap YoloBoardModule::parseMessagePayload(const QString& data) {
    QString trimmed = data.trimmed();
    if (!trimmed.startsWith('{'))
        return {{"text", data}, {"media", QVariantList{}}};
    QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8());
    if (!doc.isObject())
        return {{"text", data}, {"media", QVariantList{}}};
    QJsonObject obj = doc.object();
    if (!obj.contains("v"))
        return {{"text", data}, {"media", QVariantList{}}};

    QVariantMap result;
    result["text"] = obj["text"].toString();
    QVariantList media;
    for (const QJsonValue& m : obj["media"].toArray()) {
        QJsonObject mo = m.toObject();
        QVariantMap entry;
        entry["cid"]  = mo["cid"].toString();
        entry["type"] = mo["type"].toString();
        entry["name"] = mo["name"].toString();
        entry["size"] = mo["size"].toInt();
        media.append(entry);
    }
    result["media"] = media;
    return result;
}

QVariantList YoloBoardModule::buildChannelList() const {
    QVariantList list;
    for (const QString& id : m_channelIds) {
        QVariantMap entry;
        entry["id"] = id;
        entry["name"] = decodeChannelName(id).isEmpty()
            ? (id.length() > 12 ? id.left(12) + QStringLiteral("\u2026") : id)
            : decodeChannelName(id);
        entry["isOwn"] = (id == m_ownChannelId);
        entry["unread"] = m_unreadCounts.value(id, 0);
        list.append(entry);
    }
    return list;
}

void YoloBoardModule::setStatus(const QString& msg) {
    if (m_status == msg) return;
    m_status = msg;
    qInfo() << "YoloBoardModule status:" << msg;
    emitStatusChanged();
}

// ── Cache persistence ────────────────────────────────────────────────────────

void YoloBoardModule::loadCacheForChannel(const QString& channelId) {
    QString path = cacheFilePath(channelId);
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    QVariantList& existing = m_allMessages[channelId];
    QSet<QString> seenIds;
    for (const QVariant& v : existing)
        seenIds.insert(v.toMap().value("id").toString());

    QVariantList loaded;
    for (const QJsonValue& val : doc.array()) {
        QJsonObject obj = val.toObject();
        QString id = obj["block_id"].toString();
        if (id.isEmpty() || seenIds.contains(id)) continue;
        QVariantMap msg;
        msg["id"]        = id;
        msg["data"]      = obj["text"].toString();
        msg["channel"]   = channelId;
        msg["isOwn"]     = (channelId == m_ownChannelId);
        msg["timestamp"] = obj["timestamp"].toString();
        msg["pending"]   = false;
        msg["failed"]    = false;
        QVariantMap parsed = parseMessagePayload(msg["data"].toString());
        msg["displayText"] = parsed["text"];
        msg["media"]       = parsed["media"];
        loaded.append(msg);
        seenIds.insert(id);
    }
    if (!loaded.isEmpty())
        existing = loaded + existing;
}

void YoloBoardModule::saveCacheForChannel(const QString& channelId) {
    QString path = cacheFilePath(channelId);
    if (path.isEmpty()) return;
    QDir().mkpath(QFileInfo(path).absolutePath());

    const QVariantList& msgs = m_allMessages.value(channelId);
    QJsonArray arr;
    int start = qMax(0, (int)msgs.size() - kMaxCachedMsgs);
    for (int i = start; i < msgs.size(); ++i) {
        QVariantMap m = msgs[i].toMap();
        if (m.value("pending", false).toBool()) continue;
        if (m.value("failed",  false).toBool()) continue;
        QJsonObject obj;
        obj["block_id"]  = m["id"].toString();
        obj["text"]      = m["data"].toString();
        obj["timestamp"] = m["timestamp"].toString();
        arr.append(obj);
    }
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// ── Subscription persistence ─────────────────────────────────────────────────

void YoloBoardModule::saveSubscriptions() {
    if (m_dataDir.isEmpty()) return;
    QString path = m_dataDir + "/subscriptions.json";
    QJsonArray arr;
    for (const QString& ch : m_channelIds)
        if (ch != m_ownChannelId)
            arr.append(ch);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void YoloBoardModule::loadSubscriptions() {
    if (m_dataDir.isEmpty()) return;
    QString path = m_dataDir + "/subscriptions.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;
    for (const QJsonValue& val : doc.array()) {
        QString ch = val.toString();
        if (ch.isEmpty() || m_channelIds.contains(ch)) continue;
        m_channelIds.append(ch);
        m_unreadCounts[ch] = 0;
        loadCacheForChannel(ch);
    }
}

// ── Initialization ───────────────────────────────────────────────────────────

bool YoloBoardModule::loadKeyFromFile() {
    if (m_dataDir.isEmpty()) return false;
    QString path = m_dataDir + "/sequencer.key";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray raw = f.readAll();
    if (raw.size() != 32) return false;
    m_signingKey = QString::fromLatin1(raw.toHex());
    return true;
}

bool YoloBoardModule::loadChannelFromFile() {
    if (m_dataDir.isEmpty()) return false;
    QString path = m_dataDir + "/channel.id";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray raw = f.readAll();
    if (raw.size() != 32) return false;
    m_ownChannelId = QString::fromLatin1(raw.toHex());
    return true;
}

void YoloBoardModule::initSequencer() {
    qInfo() << "YoloBoardModule: initSequencer: set_node_url";
    zoneCall("set_node_url", {m_nodeUrl});

    qInfo() << "YoloBoardModule: load_from_directory";
    // Single IPC call that does signing_key + checkpoint + channel_id
    QString chId = zoneCall("load_from_directory", {m_dataDir}).toString();
    qInfo() << "YoloBoardModule: load_from_directory result:" << chId;
    if (!chId.isEmpty() && !chId.startsWith("Error:"))
        m_ownChannelId = chId;

    qInfo() << "YoloBoardModule: initSequencer done, ownChannelId=" << m_ownChannelId;
}

void YoloBoardModule::initStorage() {
    if (m_storageReady) return;
    if (!m_storage) {
        if (!logosAPI) {
            qWarning() << "YoloBoardModule::initStorage: no logosAPI yet";
            return;
        }
        // Safety fallback — normally constructed in initLogos.
        m_storage = new StorageModule(logosAPI);
        subscribeStorageEvents();
    }

    QString storageDir = m_dataDir + "/storage";
    QDir().mkpath(storageDir);
    QJsonObject cfg;
    cfg["data-dir"] = storageDir;
    QString cfgJson = QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));

    // Sync init — server-side storage_new + waitSync(1000) returns ~ms.
    ybmDiag(QStringLiteral("initStorage: calling init()"));
    bool initOk = m_storage->init(cfgJson);
    ybmDiag(QStringLiteral("initStorage: init() returned %1").arg(initOk));
    if (!initOk) {
        setStatus("Storage init failed");
        emitStateChanged();
        return;
    }

    // Sync start — StorageModuleImpl::start() detaches libstorage.storage_start
    // to a std::thread and returns true immediately. Real readiness fires
    // via the "storageStart" event (see subscribeStorageEvents).
    ybmDiag(QStringLiteral("initStorage: calling start()"));
    bool acc = m_storage->start();
    ybmDiag(QStringLiteral("initStorage: start() returned %1").arg(acc));
    if (!acc) {
        setStatus("Storage start rejected");
        emitStateChanged();
        return;
    }
    // Ideally we'd flip m_storageReady on the "storageStart" event, but in
    // the out-of-process QRO setup we're in, events from storage_module
    // don't reach our subscription path reliably (the generated StorageModule
    // wrapper's .on() returns true, but handlers never fire). Since start()
    // blocked until libstorage's 27 s storage_start synchronously returned
    // (server-side is detached but its reply arrives only after), we know
    // storage is actually ready by this point — flip readiness here.
    m_storageReady = true;
    refreshStorageInfo();
    setStatus(QStringLiteral("Connected to ") + m_nodeUrl);
    emitStateChanged();
    ybmDiag(QStringLiteral("initStorage: m_storageReady=true (polled)"));
}

void YoloBoardModule::refreshStorageInfo() {
    if (!m_storage) return;
    // m_storage->debug() is sync IPC that spins a nested event loop. While
    // it's waiting, the storageStart event, get_state polls, and the polled
    // fallback can all re-enter this function and queue *another* debug()
    // call on the same storage replica. In practice that piles up 40 s QRO
    // timeouts and costs ~80 s of pure wall time on startup. Guard re-entry.
    if (m_refreshingStorageInfo) return;
    // Once populated, don't re-hit the wire on every caller.
    if (!m_storagePeerId.isEmpty() && !m_storageListenAddrs.isEmpty()) return;
    m_refreshingStorageInfo = true;
    LogosResult r = m_storage->debug();
    m_refreshingStorageInfo = false;
    if (!r.success) {
        ybmDiag(QStringLiteral("refreshStorageInfo: debug() failed err=%1").arg(r.error.toString()));
        return;
    }
    m_storagePeerId        = r.getString("id");
    m_storageSpr           = r.getString("spr");
    m_storageListenAddrs   = r.getValue<QStringList>("addrs");
    m_storageAnnounceAddrs = r.getValue<QStringList>("announceAddresses");
    ybmDiag(QStringLiteral("storage info: peer=%1 addrs=[%2] announce=[%3]")
                .arg(m_storagePeerId)
                .arg(m_storageListenAddrs.join(','))
                .arg(m_storageAnnounceAddrs.join(',')));
}

// Debug: append a line to a host-visible diagnostic file. qInfo from the
// logos_host child process gets swallowed by the logoscore framework in
// headless tests; a file is the cheapest way to see what actually ran.
static void ybmDiag(const QString& line) {
    QFile f(QStringLiteral("/tmp/yolo_board_module.diag"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                        << " " << line << "\n";
    }
}

void YoloBoardModule::subscribeStorageEvents() {
    if (!m_storage || m_storageEventsBound) return;
    m_storageEventsBound = true;
    ybmDiag(QStringLiteral("subscribeStorageEvents: start"));

    // Current storage_module emits events as QVariantList{bool ok, QString msg}
    // (see EventCallbackCtx::handleResponse in storage_module_plugin.cpp).
    // storageUploadDone carries a third element: the CID on success.
    auto evtOk   = [](const QVariantList& d)       { return !d.isEmpty() && d[0].toBool(); };
    auto evtStr  = [](const QVariantList& d, int i){ return d.size() > i ? d[i].toString() : QString(); };

    bool okA = m_storage->on("storageStart", [this, evtOk, evtStr](const QVariantList& d) {
        bool ok = evtOk(d);
        QString msg = evtStr(d, 1);
        ybmDiag(QStringLiteral("EVT storageStart ok=%1 msg=%2").arg(ok).arg(msg));
        // Only flip to true on success. Never flip back to false — a method-
        // level start() failure would have been caught by our fallback polling.
        if (ok) {
            m_storageReady = true;
            refreshStorageInfo();
            setStatus(QStringLiteral("Connected to ") + m_nodeUrl);
            emitStateChanged();
        }
    });
    ybmDiag(QStringLiteral("subscribe storageStart ok=%1").arg(okA));

    bool okB = m_storage->on("storageUploadDone", [this, evtOk, evtStr](const QVariantList& d) {
        bool ok = evtOk(d);
        // Payload on success: [true, sessionId, cid]. Some builds send
        // [true, cid] with the session id dropped. Best-effort: if there
        // are 3 elements, d[1]=session, d[2]=cid. If 2, d[1]=cid and we
        // match to a pending session by cid/filename later.
        QString second = evtStr(d, 1);
        QString third  = evtStr(d, 2);
        ybmDiag(QStringLiteral("EVT storageUploadDone ok=%1 [1]=%2 [2]=%3")
                    .arg(ok).arg(second).arg(third));
        if (!ok) return;
        QString sessionId = !third.isEmpty() ? second : QString();
        QString cid       = !third.isEmpty() ? third  : second;
        if (sessionId.isEmpty()) {
            // Find a pending upload matching this cid (best effort)
            for (auto it = m_pendingUploads.begin(); it != m_pendingUploads.end(); ++it) {
                sessionId = it.key();
                break;  // take the first; crude but works for single-upload flows
            }
        }
        if (!sessionId.isEmpty()) handleUploadComplete(sessionId, cid);
    });
    ybmDiag(QStringLiteral("subscribe storageUploadDone ok=%1").arg(okB));

    bool okC = m_storage->on("storageDownloadDone", [this, evtOk, evtStr](const QVariantList& d) {
        bool ok = evtOk(d);
        QString sid = evtStr(d, 1);
        ybmDiag(QStringLiteral("EVT storageDownloadDone ok=%1 session=%2").arg(ok).arg(sid));
        if (!ok) {
            m_fetchingMedia.remove(sid);
            return;
        }
        QString cid = sid;
        QString cachePath = mediaCachePath(cid);
        if (!cachePath.isEmpty() && QFile::exists(cachePath)) {
            m_mediaPaths[cid] = cachePath;
            emitMediaReady(cid, cachePath);
            // Re-emit messages for any channel that has this cid so the UI
            // refreshes the now-ready image placeholder.
            for (const QString& chId : m_channelIds) {
                for (const QVariant& v : m_allMessages.value(chId)) {
                    for (const QVariant& mv : v.toMap()["media"].toList()) {
                        if (mv.toMap()["cid"].toString() == cid) {
                            emitMessagesChanged(chId);
                            break;
                        }
                    }
                }
            }
        }
        m_fetchingMedia.remove(cid);
    });

    bool okD = m_storage->on("storageConnect", [this, evtOk, evtStr](const QVariantList& d) {
        bool ok = evtOk(d);
        QString msg = evtStr(d, 1);
        ybmDiag(QStringLiteral("EVT storageConnect ok=%1 msg=%2").arg(ok).arg(msg));
        setStatus(ok ? QStringLiteral("Storage peer connected")
                     : QStringLiteral("Storage peer connect failed: ") + msg);
        emitStateChanged();
    });
    ybmDiag(QStringLiteral("subscribe storageConnect ok=%1").arg(okD));
}

void YoloBoardModule::handleUploadComplete(const QString& sessionId, const QString& cid) {
    if (!m_pendingUploads.contains(sessionId)) {
        qWarning() << "handleUploadComplete: unknown session" << sessionId;
        return;
    }
    PendingUpload up = m_pendingUploads.take(sessionId);
    qInfo() << "handleUploadComplete: session=" << sessionId << "cid=" << cid
            << "file=" << up.fileName;

    // Remove the placeholder pending message — publish() will add a fresh
    // one with the real inscription id shortly.
    QVariantList& msgs = m_allMessages[m_ownChannelId];
    for (int i = 0; i < msgs.size(); ++i) {
        if (msgs[i].toMap()["id"].toString() == up.pendingMsgId) {
            msgs.removeAt(i);
            break;
        }
    }

    // Cache the original file locally so resolve_media serves it without
    // a refetch across the network.
    QFile src(up.filePath);
    if (src.open(QIODevice::ReadOnly)) {
        QByteArray data = src.readAll();
        QString cachePath = mediaCachePath(cid);
        if (!cachePath.isEmpty() && !data.isEmpty()) {
            QDir().mkpath(mediaCacheDir());
            QFile cache(cachePath);
            if (cache.open(QIODevice::WriteOnly)) cache.write(data);
        }
        m_mediaPaths[cid] = cachePath;
    }

    // Build the YOLO board JSON payload and inscribe it on the zone.
    QJsonObject payload;
    payload["v"]    = 1;
    payload["text"] = up.text;
    QJsonArray media;
    QJsonObject me;
    me["cid"]  = cid;
    me["type"] = up.mimeType;
    me["name"] = up.fileName;
    me["size"] = up.fileSize;
    media.append(me);
    payload["media"] = media;
    QString msg = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    m_uploading = false;
    setStatus(QStringLiteral("Uploaded, publishing\u2026"));
    emitStateChanged();
    publish(msg);
}

// ── Public API: configure ────────────────────────────────────────────────────

QString YoloBoardModule::configure(const QString& dataDir, const QString& nodeUrl) {
    QString expanded = dataDir;
    if (expanded.startsWith("~/"))
        expanded = QDir::homePath() + expanded.mid(1);

    QDir dir(expanded);
    if (!dir.exists()) return "Error: directory does not exist: " + expanded;

    // Idempotent: if we're already configured with these exact values, or
    // configure is mid-flight, don't kick off another round. storage_module's
    // init() is not idempotent — a second call with an already-initialized
    // context fails with "Failed to create Storage", and two concurrent
    // initSequencer calls also race.
    if (m_configuring) {
        ybmDiag(QStringLiteral("configure: already configuring; skipping"));
        return QStringLiteral("configuring");
    }
    if (m_connected && m_dataDir == expanded && m_nodeUrl == nodeUrl) {
        ybmDiag(QStringLiteral("configure: already connected with same args; skipping"));
        return QStringLiteral("already connected");
    }

    m_dataDir = expanded;
    m_nodeUrl = nodeUrl;

    if (!loadKeyFromFile()) return "Error: cannot read sequencer.key in " + expanded;
    loadChannelFromFile();  // optional — can be derived

    m_configuring = true;
    m_sequencerStarting = true;
    m_storageStarting   = true;
    setStatus("Connecting & starting storage\u2026");
    emitStateChanged();

    // Both initSequencer() and initStorage() do slow sync IPC to separate
    // host processes — no shared state between them. Kick them off in
    // parallel via two independent QTimer::singleShots scheduled back-to-back.
    // Qt's Remote Objects sync calls spin a nested event loop while waiting
    // for the reply, so the second singleShot fires during the first's wait
    // and both IPC calls run concurrently on their respective host processes.
    // Net: configure now completes in max(sequencer, storage) instead of their
    // sum (historically ~80 s sequential → ~50 s parallel).

    QTimer::singleShot(0, this, [this]() {
        ybmDiag(QStringLiteral("configure: initSequencer begin"));
        initSequencer();
        m_sequencerStarting = false;
        ybmDiag(QStringLiteral("configure: initSequencer done ownChannel=%1").arg(m_ownChannelId));

        if (m_ownChannelId.isEmpty() || m_ownChannelId.startsWith("Error:")) {
            setStatus("Error: could not determine channel ID");
            if (!m_storageStarting) m_configuring = false;
            emitStateChanged();
            return;
        }

        if (!m_channelIds.contains(m_ownChannelId))
            m_channelIds.prepend(m_ownChannelId);

        loadCacheForChannel(m_ownChannelId);
        loadSubscriptions();

        m_connected = true;
        setStatus(m_storageReady
                      ? QStringLiteral("Connected to ") + m_nodeUrl
                      : QStringLiteral("Sequencer connected; storage starting…"));

        // Save config for next launch
        QJsonObject cfg;
        cfg["dataDir"] = m_dataDir;
        cfg["nodeUrl"] = m_nodeUrl;
        QFile f(uiConfigPath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(cfg).toJson(QJsonDocument::Compact));

        m_pollTimer->start();
        emitChannelsChanged();
        if (!m_storageStarting) m_configuring = false;
        emitStateChanged();
    });

    QTimer::singleShot(0, this, [this]() {
        ybmDiag(QStringLiteral("configure: initStorage begin"));
        initStorage();
        m_storageStarting = false;
        ybmDiag(QStringLiteral("configure: initStorage done storageReady=%1").arg(m_storageReady));
        if (m_connected) {
            setStatus(QStringLiteral("Connected to ") + m_nodeUrl);
        }
        if (!m_sequencerStarting) m_configuring = false;
        emitStateChanged();
    });

    return "pending";
}

// ── Public API: state snapshots ──────────────────────────────────────────────

QString YoloBoardModule::get_state() {
    // Lazy refresh: the first refreshStorageInfo() right after storageStart
    // sometimes gets empty strings back because the storage node is still
    // settling (peer id / SPR get published a few hundred ms after the event
    // fires). Retry on every get_state poll until we have real data.
    if (m_storage && m_storageReady && m_storagePeerId.isEmpty()) {
        refreshStorageInfo();
    }
    QJsonObject state;
    state["connected"] = m_connected;
    state["storageReady"] = m_storageReady;
    state["sequencerStarting"] = m_sequencerStarting;
    state["storageStarting"]   = m_storageStarting;
    state["uploading"] = m_uploading;
    state["status"] = m_status;
    state["ownChannelId"] = m_ownChannelId;
    state["ownChannelName"] = channelDisplayName(m_ownChannelId);
    state["nodeUrl"] = m_nodeUrl;
    state["dataDir"] = m_dataDir;
    state["savedPeerId"] = m_savedPeerId;
    state["savedPeerAddrs"] = m_savedPeerAddrs;

    // Storage node identity (for port-forwarding / SPR sharing).
    state["storagePeerId"]     = m_storagePeerId;
    state["storageSpr"]        = m_storageSpr;
    state["storageListenAddrs"]   = QJsonArray::fromStringList(m_storageListenAddrs);
    state["storageAnnounceAddrs"] = QJsonArray::fromStringList(m_storageAnnounceAddrs);

    QJsonArray channels;
    for (const QString& id : m_channelIds) {
        QJsonObject ch;
        ch["id"] = id;
        ch["name"] = channelDisplayName(id);
        ch["isOwn"] = (id == m_ownChannelId);
        ch["unread"] = m_unreadCounts.value(id, 0);
        channels.append(ch);
    }
    state["channels"] = channels;

    QJsonObject backfillProgress;
    for (auto it = m_backfillSlots.constBegin(); it != m_backfillSlots.constEnd(); ++it) {
        quint64 cs = it.value().first;
        quint64 ls = it.value().second;
        double p = (ls > 0) ? qMin(1.0, double(cs) / double(ls)) : 0.0;
        backfillProgress[it.key()] = p;
    }
    state["backfillProgress"] = backfillProgress;

    return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

QString YoloBoardModule::get_messages(const QString& channelId) {
    QJsonArray arr;
    for (const QVariant& v : m_allMessages.value(channelId)) {
        QVariantMap m = v.toMap();
        QJsonObject obj;
        obj["id"] = m["id"].toString();
        obj["data"] = m["data"].toString();
        obj["displayText"] = m["displayText"].toString();
        obj["channel"] = m["channel"].toString();
        obj["isOwn"] = m["isOwn"].toBool();
        obj["timestamp"] = m["timestamp"].toString();
        obj["pending"] = m["pending"].toBool();
        obj["failed"] = m["failed"].toBool();
        QJsonArray mediaArr;
        for (const QVariant& mv : m["media"].toList()) {
            QVariantMap mm = mv.toMap();
            QJsonObject me;
            me["cid"]  = mm["cid"].toString();
            me["type"] = mm["type"].toString();
            me["name"] = mm["name"].toString();
            me["size"] = mm["size"].toInt();
            mediaArr.append(me);
        }
        obj["media"] = mediaArr;
        arr.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString YoloBoardModule::get_channels() {
    QJsonArray arr;
    for (const QString& id : m_channelIds) {
        QJsonObject ch;
        ch["id"] = id;
        ch["name"] = channelDisplayName(id);
        ch["isOwn"] = (id == m_ownChannelId);
        ch["unread"] = m_unreadCounts.value(id, 0);
        arr.append(ch);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// ── Public API: channels ─────────────────────────────────────────────────────

QString YoloBoardModule::subscribe(const QString& input) {
    QString channelId = input.trimmed();
    if (channelId.isEmpty()) return "Error: empty input";

    static const QRegularExpression hexRe("^[0-9a-fA-F]{64}$");
    if (!hexRe.match(channelId).hasMatch()) {
        QString encoded = encodeChannelName(channelId);
        if (encoded.isEmpty()) return "Error: name too long";
        channelId = encoded;
    }

    if (m_channelIds.contains(channelId)) return "Error: already subscribed";

    m_channelIds.append(channelId);
    m_unreadCounts[channelId] = 0;
    saveSubscriptions();
    loadCacheForChannel(channelId);
    emitChannelsChanged();
    emitMessagesChanged(channelId);

    setStatus("Subscribed to " + channelDisplayName(channelId));
    fetchMessages(channelId);
    return channelId;
}

QString YoloBoardModule::unsubscribe(const QString& channelId) {
    if (!m_channelIds.contains(channelId)) return "Error: not subscribed";
    if (channelId == m_ownChannelId) return "Error: cannot unsubscribe from own channel";

    m_channelIds.removeAll(channelId);
    m_allMessages.remove(channelId);
    m_unreadCounts.remove(channelId);
    saveSubscriptions();
    emitChannelsChanged();
    return "ok";
}

void YoloBoardModule::clear_unread(const QString& channelId) {
    if (m_unreadCounts.value(channelId, 0) > 0) {
        m_unreadCounts[channelId] = 0;
        emitChannelsChanged();
    }
}

// ── Public API: publish ──────────────────────────────────────────────────────

QString YoloBoardModule::publish(const QString& text) {
    if (text.trimmed().isEmpty()) return "Error: empty message";
    if (!m_connected) return "Error: not connected";

    // Optimistic pending message
    QString pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap pendingMsg;
    pendingMsg["id"]        = pendingId;
    pendingMsg["data"]      = text;
    pendingMsg["channel"]   = m_ownChannelId;
    pendingMsg["isOwn"]     = true;
    pendingMsg["timestamp"] = QDateTime::currentDateTime().toString("HH:mm:ss");
    pendingMsg["pending"]   = true;
    pendingMsg["failed"]    = false;
    QVariantMap parsed = parseMessagePayload(text);
    pendingMsg["displayText"] = parsed["text"];
    pendingMsg["media"]       = parsed["media"];
    m_allMessages[m_ownChannelId].append(pendingMsg);
    emitMessagesChanged(m_ownChannelId);

    // Defer publish to next event loop iteration on main thread.
    // QRemoteObjects is thread-bound; calling zoneCall from a background
    // thread would silently fail.
    QTimer::singleShot(0, this, [this, text, pendingId]() {
        QString result = zoneCall("publish", {text}).toString();
        bool ok = !result.isEmpty() && !result.startsWith("Error:");

        QVariantList& msgs = m_allMessages[m_ownChannelId];
        for (int i = 0; i < msgs.size(); ++i) {
            QVariantMap m = msgs[i].toMap();
            if (m["id"].toString() == pendingId) {
                m["pending"] = false;
                m["failed"]  = !ok;
                if (ok) m["id"] = result;
                msgs[i] = m;
                break;
            }
        }
        emitMessagesChanged(m_ownChannelId);
        setStatus(ok ? "Published: " + result.left(12) + QStringLiteral("\u2026")
                     : "Publish failed: " + result);
        saveCacheForChannel(m_ownChannelId);
    });

    return pendingId;
}

QString YoloBoardModule::publish_with_attachment(const QString& text, const QString& filePath) {
    qInfo() << "YoloBoardModule: publish_with_attachment called text=" << text << "path=" << filePath << "storageReady=" << m_storageReady;
    QString expanded = filePath;
    if (expanded.startsWith("~/"))
        expanded = QDir::homePath() + expanded.mid(1);

    QFileInfo fi(expanded);
    if (!fi.exists()) {
        qWarning() << "publish_with_attachment: file not found" << expanded;
        return "Error: file not found: " + expanded;
    }

    QString pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Optimistic pending message — added immediately so the user gets
    // visual feedback even if storage isn't ready yet (start can take 20-30s).
    QVariantMap pendingMsg;
    pendingMsg["id"]        = pendingId;
    pendingMsg["data"]      = text.isEmpty() ? (QStringLiteral("[image] ") + fi.fileName()) : text;
    pendingMsg["channel"]   = m_ownChannelId;
    pendingMsg["isOwn"]     = true;
    pendingMsg["timestamp"] = QDateTime::currentDateTime().toString("HH:mm:ss");
    pendingMsg["pending"]   = true;
    pendingMsg["failed"]    = false;
    pendingMsg["displayText"] = text;
    QVariantMap mediaPlaceholder;
    mediaPlaceholder["cid"]  = "uploading";
    mediaPlaceholder["type"] = "image/png";
    mediaPlaceholder["name"] = fi.fileName();
    mediaPlaceholder["size"] = (int)fi.size();
    pendingMsg["media"]     = QVariantList{mediaPlaceholder};
    m_allMessages[m_ownChannelId].append(pendingMsg);
    emitMessagesChanged(m_ownChannelId);

    m_uploading = true;
    if (!m_storageReady) {
        setStatus("Waiting for storage to start\u2026");
    } else {
        setStatus("Uploading " + fi.fileName() + QStringLiteral("\u2026"));
    }
    emitStateChanged();

    // Defer to main thread; if storage isn't ready, runUpload retries until it is.
    QTimer::singleShot(0, this, [this, text, expanded, pendingId]() {
        startUploadWhenReady(text, expanded, pendingId);
    });

    return pendingId;
}

void YoloBoardModule::startUploadWhenReady(const QString& text, const QString& filePath,
                                            const QString& pendingMsgId) {
    if (!m_storageReady) {
        QTimer::singleShot(1000, this, [this, text, filePath, pendingMsgId]() {
            startUploadWhenReady(text, filePath, pendingMsgId);
        });
        return;
    }
    QFileInfo fi(filePath);
    setStatus("Uploading " + fi.fileName() + QStringLiteral("\u2026"));
    emitStateChanged();
    runUpload(text, filePath, pendingMsgId);
}

void YoloBoardModule::runUpload(const QString& text, const QString& filePath,
                                 const QString& pendingMsgId) {
    qInfo() << "YoloBoardModule::runUpload starting" << filePath;
    if (!m_storage) {
        qWarning() << "runUpload: no storage wrapper";
        m_uploading = false;
        setStatus("Upload failed: storage not available");
        emitStateChanged();
        return;
    }

    QFileInfo fi(filePath);
    QString fileName = fi.fileName();
    QString ext = fi.suffix().toLower();
    QString mimeType = "application/octet-stream";
    if (ext == "png")                       mimeType = "image/png";
    else if (ext == "jpg" || ext == "jpeg") mimeType = "image/jpeg";
    else if (ext == "gif")                  mimeType = "image/gif";
    else if (ext == "webp")                 mimeType = "image/webp";
    int fileSize = fi.size();

    // Newer storage_module only exposes typed methods returning LogosResult.
    // ModuleProxy serializes LogosResult as a JSON string on the wire; the
    // generated StorageModule wrapper's `.value<LogosResult>()` can't
    // reconstruct it on the client side. Work around by calling uploadUrl
    // through raw invokeRemoteMethod and parsing the JSON ourselves.
    // Signature: `uploadUrl(const QUrl& url, int chunkSize)`.
    // Typed SDK: the generated StorageModule wrapper unpacks LogosResult
    // correctly for us. IMPORTANT: raw invokeRemoteMethod returns a QVariant
    // holding a LogosResult (registered via qRegisterMetaType), but
    // .toString() on that returns empty — so use the typed wrapper instead.
    LogosResult r = m_storage->uploadUrl(
        QVariant::fromValue(QUrl::fromLocalFile(filePath)), 65536);
    ybmDiag(QStringLiteral("uploadUrl success=%1 valueType=%2 error=%3")
                .arg(r.success).arg(r.value.typeName()).arg(r.error.toString()));
    if (!r.success) {
        QVariantList& msgs = m_allMessages[m_ownChannelId];
        for (int i = 0; i < msgs.size(); ++i) {
            QVariantMap m = msgs[i].toMap();
            if (m["id"].toString() == pendingMsgId) {
                m["pending"] = false; m["failed"] = true;
                msgs[i] = m; break;
            }
        }
        emitMessagesChanged(m_ownChannelId);
        m_uploading = false;
        setStatus("Upload rejected: " + r.error.toString());
        emitStateChanged();
        return;
    }

    QString sessionId = r.value.toString();
    PendingUpload up;
    up.text         = text;
    up.filePath     = filePath;
    up.fileName     = fileName;
    up.mimeType     = mimeType;
    up.fileSize     = fileSize;
    up.pendingMsgId = pendingMsgId;
    m_pendingUploads.insert(sessionId, up);
    ybmDiag(QStringLiteral("runUpload accepted session=%1").arg(sessionId));

    // Completion arrives via the storageUploadDone event (subscribed in
    // subscribeStorageEvents). A safety timeout catches stuck uploads.
    QTimer::singleShot(120000, this, [this, sessionId]() {
        if (!m_pendingUploads.contains(sessionId)) return;  // already completed
        ybmDiag(QStringLiteral("upload timeout session=%1").arg(sessionId));
        PendingUpload up = m_pendingUploads.take(sessionId);
        QVariantList& msgs = m_allMessages[m_ownChannelId];
        for (int i = 0; i < msgs.size(); ++i) {
            QVariantMap m = msgs[i].toMap();
            if (m["id"].toString() == up.pendingMsgId) {
                m["pending"] = false;
                m["failed"] = true;
                msgs[i] = m;
                break;
            }
        }
        emitMessagesChanged(m_ownChannelId);
        m_uploading = false;
        setStatus("Upload timed out");
        emitStateChanged();
    });
}

// ── Public API: media ────────────────────────────────────────────────────────

QString YoloBoardModule::resolve_media(const QString& cid) {
    QString path;
    if (m_mediaPaths.contains(cid)) {
        path = m_mediaPaths[cid];
    } else {
        path = mediaCachePath(cid);
        if (path.isEmpty() || !QFile::exists(path))
            return {};
        m_mediaPaths[cid] = path;
    }

    // The QML plugin engine forbids both file:// outside its own dir and
    // network access (so data: URLs also fail). Mirror the file into the
    // QML plugin's directory if the UI told us where that is, and return
    // the path so QML can load it as a regular file://.
    if (!m_uiDir.isEmpty()) {
        QString sub = m_uiDir + "/media";
        QDir().mkpath(sub);
        QString mirrored = sub + "/" + cid;
        QFileInfo mi(mirrored);
        // If symlink (from older builds) or stale, replace with a real copy
        // — the QML sandbox resolves symlinks and refuses if the target is
        // outside the allowed root.
        if (mi.isSymLink() || !mi.exists()) {
            QFile::remove(mirrored);
            QFile::copy(path, mirrored);
        }
        return mirrored;
    }
    return path;
}

void YoloBoardModule::set_ui_dir(const QString& uiDir) {
    QString d = uiDir;
    if (d.startsWith("file://")) d = d.mid(7);
    while (d.endsWith('/')) d.chop(1);
    m_uiDir = d;
    qInfo() << "YoloBoardModule: ui dir set to" << m_uiDir;
}

void YoloBoardModule::fetch_media(const QString& cid) {
    if (cid.isEmpty()) return;
    if (m_fetchingMedia.contains(cid)) return;
    if (!resolve_media(cid).isEmpty()) {
        emitMediaReady(cid, m_mediaPaths[cid]);
        return;
    }
    if (!m_storageReady) return;

    m_fetchingMedia.insert(cid);
    QTimer::singleShot(0, this, [this, cid]() { runDownload(cid); });
}

void YoloBoardModule::runDownload(const QString& cid) {
    if (!m_storage) {
        qWarning() << "runDownload: no storage wrapper";
        m_fetchingMedia.remove(cid);
        return;
    }
    QString cachePath = mediaCachePath(cid);
    if (cachePath.isEmpty()) {
        m_fetchingMedia.remove(cid);
        return;
    }
    QDir().mkpath(mediaCacheDir());

    LogosResult dr = m_storage->downloadToUrl(
        cid, QVariant::fromValue(QUrl::fromLocalFile(cachePath)), /*local=*/false);
    ybmDiag(QStringLiteral("downloadToUrl cid=%1 success=%2 err=%3")
                .arg(cid).arg(dr.success).arg(dr.error.toString()));
    if (!dr.success) {
        m_fetchingMedia.remove(cid);
        return;
    }
    // Completion arrives via storageDownloadDone event. Safety timeout.
    QTimer::singleShot(60000, this, [this, cid]() {
        if (m_fetchingMedia.contains(cid)) {
            ybmDiag(QStringLiteral("download timeout cid=%1").arg(cid));
            m_fetchingMedia.remove(cid);
        }
    });
}

// ── Control ──────────────────────────────────────────────────────────────────

void YoloBoardModule::reset_checkpoint() {
    if (m_dataDir.isEmpty()) return;
    QString path = m_dataDir + "/sequencer.checkpoint";
    if (QFile::exists(path)) {
        QFile::rename(path, path + ".bak");
        setStatus("Checkpoint reset");
    } else {
        setStatus("No checkpoint to reset");
    }
}

void YoloBoardModule::start_backfill(const QString& channelId) {
    if (m_backfillCancelled.contains(channelId)) return;

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_backfillCancelled[channelId] = cancelled;
    m_backfillSlots[channelId] = {0, 1};
    emitStateChanged();

    QtConcurrent::run([this, channelId, cancelled]() {
        runBackfill(channelId, cancelled);
    });
}

void YoloBoardModule::stop_backfill(const QString& channelId) {
    auto it = m_backfillCancelled.find(channelId);
    if (it != m_backfillCancelled.end()) {
        (*it)->store(true);
        m_backfillCancelled.erase(it);
    }
    m_backfillSlots.remove(channelId);
    emitStateChanged();
}

void YoloBoardModule::runBackfill(const QString& channelId,
                                   std::shared_ptr<std::atomic<bool>> cancelled) {
    QString cursorJson;
    auto alive = m_alive;

    while (!cancelled->load() && alive->load()) {
        QString result = zoneCall("query_channel_paged",
                                  {channelId, cursorJson, kBackfillPageSize}).toString();
        if (result.isEmpty()) break;

        QJsonDocument doc = QJsonDocument::fromJson(result.toUtf8());
        if (!doc.isObject()) break;
        QJsonObject root = doc.object();

        quint64 cs = (quint64)root["cursor_slot"].toDouble();
        quint64 ls = (quint64)root["lib_slot"].toDouble();
        bool done = root["done"].toBool(false);

        if (!alive->load()) break;
        QMetaObject::invokeMethod(this, [this, channelId, cs, ls]() {
            m_backfillSlots[channelId] = {cs, ls};
            emitStateChanged();
        }, Qt::QueuedConnection);

        QJsonArray msgs = root["messages"].toArray();
        if (!msgs.isEmpty()) {
            QVariantList newMsgs;
            for (const QJsonValue& val : msgs) {
                QJsonObject obj = val.toObject();
                QVariantMap msg;
                msg["id"]      = obj["id"].toString();
                msg["data"]    = obj["data"].toString();
                msg["channel"] = channelId;
                msg["isOwn"]   = (channelId == m_ownChannelId);
                msg["timestamp"] = QString();
                msg["pending"] = false;
                msg["failed"]  = false;
                QVariantMap parsed = parseMessagePayload(msg["data"].toString());
                msg["displayText"] = parsed["text"];
                msg["media"]       = parsed["media"];
                newMsgs.append(msg);
            }
            if (!alive->load()) break;
            QMetaObject::invokeMethod(this, [this, channelId, newMsgs]() {
                QVariantList& existing = m_allMessages[channelId];
                QSet<QString> seenIds;
                for (const QVariant& v : existing)
                    seenIds.insert(v.toMap().value("id").toString());

                QVariantList prepend;
                for (const QVariant& v : newMsgs) {
                    QString id = v.toMap().value("id").toString();
                    if (!seenIds.contains(id)) {
                        prepend.append(v);
                        seenIds.insert(id);
                    }
                }
                if (!prepend.isEmpty()) {
                    existing = prepend + existing;
                    saveCacheForChannel(channelId);
                    emitMessagesChanged(channelId);
                }
            }, Qt::QueuedConnection);
        }

        QJsonDocument cursorDoc(root["cursor"].toObject());
        cursorJson = QString::fromUtf8(cursorDoc.toJson(QJsonDocument::Compact));

        if (done) break;
        QThread::msleep(200);
    }

    if (!alive->load()) return;
    QMetaObject::invokeMethod(this, [this, channelId]() {
        m_backfillCancelled.remove(channelId);
        m_backfillSlots.remove(channelId);
        emitStateChanged();
        setStatus("Backfill complete for " + channelDisplayName(channelId));
    }, Qt::QueuedConnection);
}

// ── Polling ──────────────────────────────────────────────────────────────────

void YoloBoardModule::pollNextChannel() {
    if (m_channelIds.isEmpty()) return;
    if (m_pollIndex >= m_channelIds.size()) m_pollIndex = 0;
    QString ch = m_channelIds.at(m_pollIndex);
    m_pollIndex++;
    fetchMessages(ch);
}

void YoloBoardModule::fetchMessages(const QString& channelId) {
    if (m_fetchingChannels.contains(channelId)) return;
    m_fetchingChannels.insert(channelId);

    // zoneCall is thread-bound to main thread — already on main thread here
    // since this is called from poll timer. Just run sync.
    QString json = zoneCall("query_channel", {channelId, kQueryLimit}).toString();
    m_fetchingChannels.remove(channelId);
    if (json.isEmpty() || json == "[]") return;

    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) return;

    QVariantList& existing = m_allMessages[channelId];
    QSet<QString> seenIds;
    for (const QVariant& v : existing)
        seenIds.insert(v.toMap().value("id").toString());

    bool added = false;
    for (const QJsonValue& val : doc.array()) {
        QJsonObject obj = val.toObject();
        QString id = obj["id"].toString();
        if (seenIds.contains(id)) continue;

        QString text = obj["data"].toString();

        bool confirmed = false;
        for (int i = 0; i < existing.size(); ++i) {
            QVariantMap m = existing[i].toMap();
            if ((m.value("pending").toBool() || m.value("failed").toBool())
                && m.value("data").toString() == text) {
                m["pending"] = false;
                m["failed"]  = false;
                m["id"]      = id;
                existing[i]  = m;
                confirmed = true;
                break;
            }
        }

        if (!confirmed) {
            QVariantMap msg;
            msg["id"]        = id;
            msg["data"]      = text;
            msg["channel"]   = channelId;
            msg["isOwn"]     = (channelId == m_ownChannelId);
            msg["timestamp"] = QDateTime::currentDateTime().toString("HH:mm:ss");
            msg["pending"]   = false;
            msg["failed"]    = false;
            QVariantMap parsed = parseMessagePayload(text);
            msg["displayText"] = parsed["text"];
            msg["media"]       = parsed["media"];
            existing.append(msg);
            if (channelId != m_ownChannelId)
                m_unreadCounts[channelId] = m_unreadCounts.value(channelId, 0) + 1;
        }
        seenIds.insert(id);
        added = true;
    }

    if (added) {
        saveCacheForChannel(channelId);
        emitMessagesChanged(channelId);
        emitChannelsChanged();
    }
}

// ── Events ───────────────────────────────────────────────────────────────────

void YoloBoardModule::emitStateChanged() {
    emit eventResponse("stateChanged", {get_state()});
}

void YoloBoardModule::emitMessagesChanged(const QString& channelId) {
    emit eventResponse("messagesChanged", {channelId});
}

void YoloBoardModule::emitChannelsChanged() {
    emit eventResponse("channelsChanged", {get_channels()});
}

void YoloBoardModule::emitStatusChanged() {
    emit eventResponse("statusChanged", {m_status});
}

void YoloBoardModule::emitMediaReady(const QString& cid, const QString& path) {
    emit eventResponse("mediaReady", {cid, path});
}

// ── Config persistence ──────────────────────────────────────────────────────

QString YoloBoardModule::load_saved_config() {
    QFile f(uiConfigPath());
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("{}");
    return QString::fromUtf8(f.readAll());
}

// ── Storage peer connect ────────────────────────────────────────────────────

QString YoloBoardModule::connect_storage_peer(const QString& peerId,
                                              const QString& addressesCsv) {
    if (!m_storageReady) return QStringLiteral("Error: storage not ready");
    QString pid = peerId.trimmed();
    if (pid.isEmpty()) return QStringLiteral("Error: peer id required");

    QStringList addrs;
    for (const QString& a : addressesCsv.split(',', Qt::SkipEmptyParts)) {
        QString t = a.trimmed();
        if (!t.isEmpty()) addrs.append(t);
    }

    if (!logosAPI) return QStringLiteral("Error: no logos api");
    qInfo() << "connect_storage_peer" << pid << addrs;
    // Bypass the typed wrapper for this call: StorageModule::connect's
    // template-variadic invokeRemoteMethod wraps the QStringList with
    // QVariant::fromValue(QStringList), which does NOT deserialize to
    // QList<QString> on the remote replica and causes the call to hang
    // until the 20 s QRO timeout (this is the same QStringList-serialization
    // bug we patched earlier). Passing a QVariantList of QString works.
    LogosAPIClient* sc = logosAPI->getClient(kStorageModuleName);
    ybmDiag(QStringLiteral("connect_storage_peer %1 addrs=%2").arg(pid).arg(addrs.join(',')));
    // Pass the QStringList directly (wrapped once); server side takes
    // QList<QString>& which matches QMetaType::QStringList.
    sc->invokeRemoteMethod(kStorageModuleName, "connect",
                           QVariantList{ QVariant(pid), QVariant::fromValue(addrs) });
    setStatus(QStringLiteral("connect %1: accepted")
                  .arg(pid.left(12) + QStringLiteral("\u2026")));

    // Persist so we auto-dial on next start.
    m_savedPeerId = pid;
    m_savedPeerAddrs = addressesCsv.trimmed();
    m_savedPeerDialed = true;
    {
        QFile f(uiConfigPath());
        QJsonObject cfg;
        if (f.open(QIODevice::ReadOnly)) {
            cfg = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
        }
        cfg["dataDir"] = m_dataDir;
        cfg["nodeUrl"] = m_nodeUrl;
        cfg["peerId"] = m_savedPeerId;
        cfg["peerAddrs"] = m_savedPeerAddrs;
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    }
    emitStateChanged();
    return QStringLiteral("accepted");
}
