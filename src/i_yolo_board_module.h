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
};
#define IYoloBoardModule_iid "org.logos.iyoloboardmodule"
Q_DECLARE_INTERFACE(IYoloBoardModule, IYoloBoardModule_iid)
