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

    // Persistence
    void loadSubscriptions();
    void saveSubscriptions();

    // IPC helpers (to zone-sequencer and storage modules)
    QVariant zoneCall(const QString& method, const QVariantList& args = {});
    QVariant storageCall(const QString& method, const QVariantList& args = {});

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
    LogosAPIClient* m_storageClient = nullptr;

    // State
    QString      m_dataDir;
    QString      m_nodeUrl;
    QString      m_signingKey;
    QString      m_ownChannelId;
    QString      m_status;
    bool         m_connected = false;
    bool         m_storageReady = false;
    bool         m_uploading = false;

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
