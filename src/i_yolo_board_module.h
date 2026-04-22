#pragma once
#include <QtPlugin>
#include <QString>
#include <QVariantList>

class LogosAPI;

class PluginInterface {
public:
    virtual ~PluginInterface() {}
    virtual QString name() const = 0;
    virtual QString version() const = 0;
    LogosAPI* logosAPI = nullptr;
};
#define PluginInterface_iid "com.example.PluginInterface"
Q_DECLARE_INTERFACE(PluginInterface, PluginInterface_iid)

class IYoloBoardModule : public PluginInterface {
public:
    virtual ~IYoloBoardModule() {}

    // Lifecycle
    Q_INVOKABLE virtual void initLogos(LogosAPI* api) = 0;

    // Configuration
    Q_INVOKABLE virtual QString configure(const QString& dataDir, const QString& nodeUrl) = 0;
    Q_INVOKABLE virtual QString get_state() = 0;  // returns JSON snapshot of full state
    Q_INVOKABLE virtual QString get_messages(const QString& channelId) = 0;  // JSON array
    Q_INVOKABLE virtual QString get_channels() = 0;  // JSON array

    // Channels
    Q_INVOKABLE virtual QString subscribe(const QString& channelIdOrName) = 0;
    Q_INVOKABLE virtual QString unsubscribe(const QString& channelId) = 0;
    Q_INVOKABLE virtual void clear_unread(const QString& channelId) = 0;

    // Publishing
    Q_INVOKABLE virtual QString publish(const QString& text) = 0;
    Q_INVOKABLE virtual QString publish_with_attachment(const QString& text, const QString& filePath) = 0;

    // Media
    Q_INVOKABLE virtual QString resolve_media(const QString& cid) = 0;  // returns local path or ""
    Q_INVOKABLE virtual void fetch_media(const QString& cid) = 0;

    // Control
    Q_INVOKABLE virtual void reset_checkpoint() = 0;
    Q_INVOKABLE virtual void start_backfill(const QString& channelId) = 0;
    Q_INVOKABLE virtual void stop_backfill(const QString& channelId) = 0;

    // Config persistence (for UI auto-connect)
    Q_INVOKABLE virtual QString load_saved_config() = 0;  // returns JSON {"dataDir":"","nodeUrl":""}

    // The QML host sandbox restricts file:// to the plugin's own directory
    // and disables network. UI calls this once with its own dir so
    // resolve_media can mirror cached media files there.
    Q_INVOKABLE virtual void set_ui_dir(const QString& uiDir) = 0;

    // Ask the storage node to dial a peer. `addressesCsv` is a comma-separated
    // list of multiaddrs (may be empty to let the node discover via DHT).
    // Returns a short status string for immediate UI feedback; the underlying
    // call is async and logs completion via the usual storage events.
    Q_INVOKABLE virtual QString connect_storage_peer(const QString& peerId,
                                                     const QString& addressesCsv) = 0;

    // Per-message "troll box" threads. Derives a stable Waku content topic
    // from (parentChannelId, parentMsgId) and subscribes via delivery_module.
    // The topic is stable — anyone calling open_thread with the same inputs
    // joins the same room. Returns the derived topic on success, empty on
    // "delivery not ready yet" (UI shows connecting state).
    Q_INVOKABLE virtual QString open_thread(const QString& parentChannelId,
                                            const QString& parentMsgId) = 0;

    // Unsubscribes the thread; drops buffered messages.
    Q_INVOKABLE virtual void close_thread(const QString& threadContentTopic) = 0;

    // Publishes a reply to an open thread. Returns a local pending message
    // id on success, "Error:..." otherwise. The message is added to the
    // thread buffer optimistically; delivery_module's messageSent /
    // messageError events will flip pending/failed flags.
    Q_INVOKABLE virtual QString publish_thread_reply(const QString& threadContentTopic,
                                                     const QString& text) = 0;

    // JSON array of the currently-buffered messages in the given thread.
    // Each entry: { id, text, nick, ts, isOwn, pending, failed }.
    Q_INVOKABLE virtual QString get_thread_messages(const QString& threadContentTopic) = 0;

    // JSON array of { threadTopic, parentChannelId, parentMsgId, parentPreview,
    // firstSeenMs, lastSeenMs }. Persisted across restarts, but threads are
    // NOT auto-resubscribed — the user must open_thread again to rejoin.
    Q_INVOKABLE virtual QString get_participated_threads() = 0;

    // "true" once the delivery subscribe for the given topic has been
    // confirmed by the relay, "false" otherwise. UI uses this to clear a
    // "Connecting to relay…" indicator in the thread panel.
    Q_INVOKABLE virtual QString is_thread_subscribed(const QString& threadContentTopic) = 0;
};
#define IYoloBoardModule_iid "org.logos.iyoloboardmodule"
Q_DECLARE_INTERFACE(IYoloBoardModule, IYoloBoardModule_iid)
