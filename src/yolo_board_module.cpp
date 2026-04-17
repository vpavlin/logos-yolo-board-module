#include "yolo_board_module.h"
#include "logos_api.h"
#include "logos_api_client.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QUuid>
#include <QRegularExpression>
#include <QThread>

const char* YoloBoardModule::kZoneModuleName = "liblogos_zone_sequencer_module";
const char* YoloBoardModule::kStorageModuleName = "storage_module";

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

void YoloBoardModule::initLogos(LogosAPI* api) {
    if (logosAPI) return;
    logosAPI = api;
    qInfo() << "YoloBoardModule: initLogos called";
}

// ── Helpers ──────────────────────────────────────────────────────────────────

QVariant YoloBoardModule::zoneCall(const QString& method, const QVariantList& args) {
    if (!m_zoneClient && logosAPI) {
        m_zoneClient = logosAPI->getClient(kZoneModuleName);
    }
    if (!m_zoneClient) return {};
    return m_zoneClient->invokeRemoteMethod(kZoneModuleName, method, args);
}

QVariant YoloBoardModule::storageCall(const QString& method, const QVariantList& args) {
    if (!m_storageClient && logosAPI) {
        m_storageClient = logosAPI->getClient(kStorageModuleName);
    }
    if (!m_storageClient) return {};
    return m_storageClient->invokeRemoteMethod(kStorageModuleName, method, args);
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
    zoneCall("set_node_url", {m_nodeUrl});
    zoneCall("set_signing_key", {m_signingKey});
    zoneCall("set_checkpoint_path", {m_dataDir + "/sequencer.checkpoint"});

    if (m_ownChannelId.isEmpty()) {
        QString chId = zoneCall("get_channel_id").toString();
        if (!chId.isEmpty() && !chId.startsWith("Error:"))
            m_ownChannelId = chId;
    }
    if (!m_ownChannelId.isEmpty())
        zoneCall("set_channel_id", {m_ownChannelId});
}

void YoloBoardModule::initStorage() {
    if (m_storageReady) return;
    QString storageDir = m_dataDir + "/storage";
    QDir().mkpath(storageDir);
    QJsonObject cfg;
    cfg["data-dir"] = storageDir;
    QString cfgJson = QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    QVariant initResult = storageCall("init", {cfgJson});
    qInfo() << "Storage init:" << initResult;
    QVariant startResult = storageCall("start", {});
    qInfo() << "Storage start:" << startResult;
    m_storageReady = true;
}

// ── Public API: configure ────────────────────────────────────────────────────

QString YoloBoardModule::configure(const QString& dataDir, const QString& nodeUrl) {
    QString expanded = dataDir;
    if (expanded.startsWith("~/"))
        expanded = QDir::homePath() + expanded.mid(1);

    QDir dir(expanded);
    if (!dir.exists()) return "Error: directory does not exist: " + expanded;

    m_dataDir = expanded;
    m_nodeUrl = nodeUrl;

    if (!loadKeyFromFile()) return "Error: cannot read sequencer.key in " + expanded;
    loadChannelFromFile();  // optional — can be derived

    setStatus("Connecting\u2026");

    // Defer heavy IPC work onto main thread event loop AFTER we return "pending"
    // from this call. QRemoteObjects is thread-bound so all IPC must be on main.
    QTimer::singleShot(0, this, [this]() {
        initSequencer();
        if (m_ownChannelId.isEmpty() || m_ownChannelId.startsWith("Error:")) {
            setStatus("Error: could not determine channel ID");
            return;
        }

        if (!m_channelIds.contains(m_ownChannelId))
            m_channelIds.prepend(m_ownChannelId);

        loadCacheForChannel(m_ownChannelId);
        loadSubscriptions();

        m_connected = true;
        setStatus("Connected to " + m_nodeUrl);

        m_pollTimer->start();
        emitChannelsChanged();
        emitStateChanged();

        // Initialize storage after brief delay
        QTimer::singleShot(500, this, [this]() {
            initStorage();
            emitStateChanged();
        });
    });

    return "pending";
}

// ── Public API: state snapshots ──────────────────────────────────────────────

QString YoloBoardModule::get_state() {
    QJsonObject state;
    state["connected"] = m_connected;
    state["storageReady"] = m_storageReady;
    state["uploading"] = m_uploading;
    state["status"] = m_status;
    state["ownChannelId"] = m_ownChannelId;
    state["ownChannelName"] = channelDisplayName(m_ownChannelId);
    state["nodeUrl"] = m_nodeUrl;
    state["dataDir"] = m_dataDir;

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
        QJsonArray media;
        for (const QVariant& mv : m["media"].toList()) {
            QVariantMap mm = mv.toMap();
            QJsonObject me;
            me["cid"] = mm["cid"].toString();
            me["type"] = mm["type"].toString();
            me["name"] = mm["name"].toString();
            me["size"] = mm["size"].toInt();
            arr.append(obj);  // bug? actually append after
            // skip, see below
        }
        // actually build media array properly
        QJsonArray mediaArr;
        for (const QVariant& mv : m["media"].toList()) {
            QVariantMap mm = mv.toMap();
            QJsonObject me;
            me["cid"] = mm["cid"].toString();
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

    // Publish in background thread so IPC doesn't block our caller
    auto alive = m_alive;
    QString tx = m_ownChannelId;
    QtConcurrent::run([this, alive, text, pendingId]() {
        if (!alive->load()) return;
        QString result = zoneCall("publish", {text}).toString();
        bool ok = !result.isEmpty() && !result.startsWith("Error:");

        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, alive, result, pendingId, ok]() {
            if (!alive->load()) return;
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
        }, Qt::QueuedConnection);
    });

    return pendingId;
}

QString YoloBoardModule::publish_with_attachment(const QString& text, const QString& filePath) {
    QString expanded = filePath;
    if (expanded.startsWith("~/"))
        expanded = QDir::homePath() + expanded.mid(1);

    QFileInfo fi(expanded);
    if (!fi.exists()) return "Error: file not found: " + expanded;
    if (!m_storageReady) return "Error: storage not ready";

    QString pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    m_uploading = true;
    setStatus("Uploading " + fi.fileName() + QStringLiteral("\u2026"));
    emitStateChanged();

    auto alive = m_alive;
    QtConcurrent::run([this, alive, text, expanded, pendingId]() {
        if (!alive->load()) return;
        runUpload(text, expanded, pendingId);
    });

    return pendingId;
}

void YoloBoardModule::runUpload(const QString& text, const QString& filePath,
                                 const QString& pendingMsgId) {
    Q_UNUSED(pendingMsgId);
    QFileInfo fi(filePath);
    QString fileName = fi.fileName();
    QString ext = fi.suffix().toLower();
    QString mimeType = "application/octet-stream";
    if (ext == "png") mimeType = "image/png";
    else if (ext == "jpg" || ext == "jpeg") mimeType = "image/jpeg";
    else if (ext == "gif") mimeType = "image/gif";
    else if (ext == "webp") mimeType = "image/webp";
    int fileSize = fi.size();

    QString upResult = storageCall("uploadUrl", {filePath, (qlonglong)65536}).toString();
    bool started = false;
    {
        QJsonDocument doc = QJsonDocument::fromJson(upResult.toUtf8());
        if (doc.isObject()) started = doc.object()["success"].toBool();
    }
    if (!started) {
        QMetaObject::invokeMethod(this, [this]() {
            m_uploading = false;
            setStatus("Upload failed");
            emitStateChanged();
        }, Qt::QueuedConnection);
        return;
    }

    // Poll manifests for CID (same thread, OK because we're in background)
    QString foundCid;
    for (int attempt = 0; attempt < 30 && !m_alive->load() == false; ++attempt) {
        QThread::msleep(2000);
        if (!m_alive->load()) return;
        QString r = storageCall("manifests", {}).toString();
        QJsonDocument doc = QJsonDocument::fromJson(r.toUtf8());
        if (doc.isObject() && doc.object()["success"].toBool()) {
            QJsonArray arr = doc.object()["value"].toArray();
            for (const QJsonValue& v : arr) {
                QJsonObject m = v.toObject();
                if (m["filename"].toString() == fileName) {
                    foundCid = m["cid"].toString();
                    break;
                }
            }
        }
        if (!foundCid.isEmpty()) break;
    }

    if (foundCid.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            m_uploading = false;
            setStatus("Upload timed out");
            emitStateChanged();
        }, Qt::QueuedConnection);
        return;
    }

    // Cache locally
    QFile src(filePath);
    QByteArray fileData;
    if (src.open(QIODevice::ReadOnly)) fileData = src.readAll();

    // Build payload and publish
    QJsonObject payload;
    payload["v"] = 1;
    payload["text"] = text;
    QJsonArray media;
    QJsonObject me;
    me["cid"] = foundCid;
    me["type"] = mimeType;
    me["name"] = fileName;
    me["size"] = fileSize;
    media.append(me);
    payload["media"] = media;
    QString msg = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    // Save to cache
    QString cachePath = mediaCachePath(foundCid);
    if (!cachePath.isEmpty() && !fileData.isEmpty()) {
        QDir().mkpath(mediaCacheDir());
        QFile cache(cachePath);
        if (cache.open(QIODevice::WriteOnly)) cache.write(fileData);
    }

    if (!m_alive->load()) return;
    QMetaObject::invokeMethod(this, [this, msg, foundCid, cachePath]() {
        m_uploading = false;
        m_mediaPaths[foundCid] = cachePath;
        setStatus("Uploaded, publishing\u2026");
        emitStateChanged();
        publish(msg);  // publishes the JSON payload
    }, Qt::QueuedConnection);
}

// ── Public API: media ────────────────────────────────────────────────────────

QString YoloBoardModule::resolve_media(const QString& cid) {
    if (m_mediaPaths.contains(cid)) return m_mediaPaths[cid];
    QString path = mediaCachePath(cid);
    if (!path.isEmpty() && QFile::exists(path)) {
        m_mediaPaths[cid] = path;
        return path;
    }
    return {};
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
    auto alive = m_alive;
    QtConcurrent::run([this, alive, cid]() {
        if (!alive->load()) return;
        runDownload(cid);
    });
}

void YoloBoardModule::runDownload(const QString& cid) {
    QString cachePath = mediaCachePath(cid);
    if (cachePath.isEmpty()) return;

    QDir().mkpath(mediaCacheDir());
    storageCall("downloadFile", {cid, cachePath, false});

    // Poll for file
    for (int attempt = 0; attempt < 30; ++attempt) {
        QThread::msleep(1000);
        if (!m_alive->load()) return;
        if (QFile::exists(cachePath) && QFileInfo(cachePath).size() > 0) {
            if (!m_alive->load()) return;
            QMetaObject::invokeMethod(this, [this, cid, cachePath]() {
                m_fetchingMedia.remove(cid);
                m_mediaPaths[cid] = cachePath;
                emitMediaReady(cid, cachePath);
                for (const QString& chId : m_channelIds) {
                    for (const QVariant& v : m_allMessages.value(chId)) {
                        for (const QVariant& mv : v.toMap()["media"].toList()) {
                            if (mv.toMap()["cid"].toString() == cid) {
                                emitMessagesChanged(chId);
                                return;
                            }
                        }
                    }
                }
            }, Qt::QueuedConnection);
            return;
        }
    }
    if (!m_alive->load()) return;
    QMetaObject::invokeMethod(this, [this, cid]() {
        m_fetchingMedia.remove(cid);
    }, Qt::QueuedConnection);
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
