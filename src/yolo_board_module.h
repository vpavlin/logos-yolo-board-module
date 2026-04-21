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

    static const char* kZoneModuleName;
    static const char* kStorageModuleName;
};
