#pragma once
#include "i_yolo_board_module.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QPair>
#include <atomic>
#include <memory>

class LogosAPI;
class LogosAPIClient;
class LogosObject;
class StorageModule;

class YoloBoardModule : public QObject, public IYoloBoardModule {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IYoloBoardModule_iid FILE YOLO_BOARD_MODULE_METADATA_FILE)
    Q_INTERFACES(PluginInterface IYoloBoardModule)

public:
    YoloBoardModule();
    ~YoloBoardModule() override;

    QString name() const override { return "yolo_board_module"; }
    QString version() const override { return "0.1.0"; }

    Q_INVOKABLE void initLogos(LogosAPI* api) override;

    Q_INVOKABLE QString configure(const QString& dataDir, const QString& nodeUrl) override;
    Q_INVOKABLE QString get_state() override;
    Q_INVOKABLE QString get_messages(const QString& channelId) override;
    Q_INVOKABLE QString get_channels() override;

    Q_INVOKABLE QString subscribe(const QString& channelIdOrName) override;
    Q_INVOKABLE QString unsubscribe(const QString& channelId) override;
    Q_INVOKABLE void clear_unread(const QString& channelId) override;

    Q_INVOKABLE QString publish(const QString& text) override;
    Q_INVOKABLE QString publish_with_attachment(const QString& text, const QString& filePath) override;

    Q_INVOKABLE QString resolve_media(const QString& cid) override;
    Q_INVOKABLE void fetch_media(const QString& cid) override;

    Q_INVOKABLE void reset_checkpoint() override;
    Q_INVOKABLE void start_backfill(const QString& channelId) override;
    Q_INVOKABLE void stop_backfill(const QString& channelId) override;
    Q_INVOKABLE QString load_saved_config() override;
    Q_INVOKABLE void set_ui_dir(const QString& uiDir) override;

    Q_INVOKABLE QString connect_storage_peer(const QString& peerId,
                                             const QString& addressesCsv) override;

    Q_INVOKABLE QString open_thread(const QString& parentChannelId,
                                    const QString& parentMsgId) override;
    Q_INVOKABLE void    close_thread(const QString& threadContentTopic) override;
    Q_INVOKABLE QString publish_thread_reply(const QString& threadContentTopic,
                                             const QString& text) override;
    Q_INVOKABLE QString get_thread_messages(const QString& threadContentTopic) override;
    Q_INVOKABLE QString get_participated_threads() override;
    Q_INVOKABLE QString is_thread_subscribed(const QString& threadContentTopic) override;

signals:
    void eventResponse(const QString& eventName, const QVariantList& data);

private slots:
    void pollNextChannel();

private:
    // Connection lifecycle
    bool loadKeyFromFile();
    bool loadChannelFromFile();
    void initSequencer();
    void initStorage();
    // Subscribes the four storage_module events we care about. Safe to call
    // more than once — no-ops on duplicate subscribes in the SDK.
    void subscribeStorageEvents();
    // Delivery module lifecycle: construct wrapper + subscribe events in
    // initLogos (mirror of storage), drive createNode + start from
    // configure().
    void subscribeDeliveryEvents();
    void initDelivery();
    // Event handler for delivery_module's messageReceived — parses payload
    // and appends to m_threadMessages buffer, emits threadMessagesChanged.
    void onThreadMessageReceived(const QString& contentTopic,
                                 const QString& payloadB64,
                                 quint64 timestampNs);
    // Derives a stable Waku content topic of the form
    // `/yolo/1/thread-<hex>/proto` from (parentChannelId, parentMsgId).
    static QString threadContentTopic(const QString& parentChannelId,
                                      const QString& parentMsgId);
    // Persistence + lookup helpers for the "My threads" list.
    void recordParticipation(const QString& threadTopic,
                             const QString& parentChannelId,
                             const QString& parentMsgId);
    void loadParticipatedThreads();
    void saveParticipatedThreads();
    QString participatedThreadsPath() const;
    // Populate m_storagePeerId / m_storageSpr / addrs via storage_module.debug().
    void refreshStorageInfo();
    // storageUploadDone event handler completes the two-step publish flow
    // we started in runUpload.
    void handleUploadComplete(const QString& sessionId, const QString& cid);

    // Persistence
    void loadSubscriptions();
    void saveSubscriptions();

    // IPC helpers (to zone-sequencer and storage modules)
    QVariant zoneCall(const QString& method, const QVariantList& args = {});
    // storageCall() removed — use m_storage->... (typed SDK) for storage_module
    // calls. Typed bindings marshal QStringList natively, surface LogosResult
    // directly, and support .on(event, cb) for async completion.

    // Channel helpers
    static QString encodeChannelName(const QString& name);
    static QString decodeChannelName(const QString& hexId);
    static QString channelDisplayName(const QString& channelId);

    // Cache helpers
    QString mediaCacheDir() const;
    QString mediaCachePath(const QString& cid) const;
    QString cacheFilePath(const QString& channelId) const;
    void loadCacheForChannel(const QString& channelId);
    void saveCacheForChannel(const QString& channelId);

    // Message helpers
    static QVariantMap parseMessagePayload(const QString& data);
    QVariantList buildChannelList() const;

    // Polling
    void fetchMessages(const QString& channelId);

    // Media upload/download
    void startUploadWhenReady(const QString& text, const QString& filePath, const QString& pendingMsgId);
    void runUpload(const QString& text, const QString& filePath, const QString& pendingMsgId);
    void runDownload(const QString& cid);

    // Backfill
    void runBackfill(const QString& channelId, std::shared_ptr<std::atomic<bool>> cancelled);

    // Events
    void emitStateChanged();
    void emitMessagesChanged(const QString& channelId);
    void emitChannelsChanged();
    void emitStatusChanged();
    void emitMediaReady(const QString& cid, const QString& path);
    void emitThreadMessagesChanged(const QString& threadTopic);

    void setStatus(const QString& msg);

    // IPC clients
    LogosAPIClient* m_zoneClient = nullptr;

    // Typed storage wrapper (generated from storage_module_plugin.h). Use
    // for every storage_module call — it marshals QStringList properly,
    // returns typed LogosResult, and exposes .on(event, cb) for async
    // completion (avoiding the manifest-polling anti-pattern).
    StorageModule* m_storage = nullptr;
    bool            m_storageEventsBound = false;
    bool            m_refreshingStorageInfo = false;

    // Delivery module — consumed over raw QRO IPC (no typed wrapper).
    // delivery_module is declared as a dep in metadata.json so logos_host
    // loads it before us; we use invokeRemoteMethod + onEvent directly.
    // The typed-wrapper route is blocked upstream because the existing
    // DeliveryModulePlugin class shape conflicts with the generator's
    // universal-mode output — see feat/qro-universal-interface fork for
    // the metadata-only path that would unlock it once the impl class is
    // refactored.
    LogosAPIClient* m_deliveryClient = nullptr;
    LogosObject*    m_deliveryReplica = nullptr;
    bool            m_deliveryEventsBound = false;
    bool            m_deliveryReady = false;
    bool            m_deliveryStarting = false;

    // Active thread subscriptions: topic → { parentChannelId, parentMsgId }.
    // Only populated between open_thread and close_thread.
    QMap<QString, QVariantMap>  m_activeThreads;
    // Volatile per-thread message buffer (not cached to disk — threads are
    // ephemeral by design; user rejoins to see new activity, not history).
    QMap<QString, QVariantList> m_threadMessages;
    // Map: delivery requestId → { threadTopic, localMsgId }. Lets
    // messageSent/messageError flip our pending flag on the right local
    // optimistic message.
    QMap<QString, QVariantMap>  m_threadPendingById;
    // topic → true once subscribe's async reply came back ok. Used by
    // the UI to hide a "Connecting to relay…" indicator.
    QMap<QString, bool>         m_threadSubscribed;
    // Persisted list of threads the user has participated in. Rendered
    // by the "My threads" UI panel. NOT auto-resubscribed on launch.
    QVariantList                m_participatedThreads;

    // Pending uploads — keyed by session id returned from uploadUrl. The
    // storageUploadDone event handler completes publishing and caching.
    struct PendingUpload {
        QString text;
        QString filePath;
        QString fileName;
        QString mimeType;
        int     fileSize = 0;
        QString pendingMsgId;
    };
    QMap<QString, PendingUpload> m_pendingUploads;

    // State
    QString      m_dataDir;
    QString      m_nodeUrl;
    QString      m_signingKey;
    QString      m_ownChannelId;
    QString      m_status;
    bool         m_connected = false;
    bool         m_storageReady = false;
    bool         m_uploading = false;
    // "in-progress" flags for the blinking start-up icons.
    bool         m_sequencerStarting = false;
    bool         m_storageStarting   = false;

    // Cached storage node identity — populated after storageStart fires.
    // Shown in a QML tooltip so the user can set up port-forwarding etc.
    QString      m_storagePeerId;
    QString      m_storageSpr;
    QStringList  m_storageListenAddrs;
    QStringList  m_storageAnnounceAddrs;
    // Reentrancy guard for configure() — stops a second concurrent configure
    // from double-initialising the modules while the first is still mid-flight.
    bool         m_configuring       = false;

    QString      m_uiDir;

    // Saved storage peer (persisted to uiConfigPath())
    QString      m_savedPeerId;
    QString      m_savedPeerAddrs;
    bool         m_savedPeerDialed = false;
    QStringList                 m_channelIds;
    QMap<QString, QVariantList> m_allMessages;
    QMap<QString, int>          m_unreadCounts;

    // Polling state
    QTimer*      m_pollTimer = nullptr;
    int          m_pollIndex = 0;
    QSet<QString> m_fetchingChannels;

    // Media state
    QMap<QString, QString> m_mediaPaths;
    QSet<QString>          m_fetchingMedia;

    // Backfill state
    QMap<QString, std::shared_ptr<std::atomic<bool>>> m_backfillCancelled;
    QMap<QString, QPair<quint64,quint64>> m_backfillSlots;

    // Liveness flag for background tasks
    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);

    static constexpr int kPollIntervalMs = 1500;
    static constexpr int kQueryLimit = 50;
    static constexpr int kMaxCachedMsgs = 200;
    static constexpr int kBackfillPageSize = 100;
    // Cap in-memory thread history per open thread. Threads are volatile
    // so this is purely a memory guard, not a "how much history" policy.
    static constexpr int kMaxThreadMsgs = 500;

    static const char* kZoneModuleName;
    static const char* kStorageModuleName;
    static const char* kDeliveryModuleName;
};
