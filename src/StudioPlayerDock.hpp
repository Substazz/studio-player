#pragma once

#include <QComboBox>
#include <QDockWidget>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPushButton>
#include <QSlider>
#include <QTcpServer>
#include <QTimer>

#include <obs.h>
#include <obs-frontend-api.h>

class StudioPlayerDock : public QDockWidget {
    Q_OBJECT

public:
    explicit StudioPlayerDock(QWidget *parent = nullptr);
    ~StudioPlayerDock() override;

private slots:
    void onLoadClicked();
    void onQueueClicked();
    void onQueuePlaySelectedClicked();
    void onQueueRemoveClicked();
    void onQueueClearClicked();
    void onQueueSelectionChanged();

    void onPlayClicked();
    void onPauseClicked();
    void onStopClicked();
    void onVolumeChanged(int value);

    void onShowClicked();
    void onHideClicked();
    void onOpenPlayerClicked();

    void onImportQueueClicked();
    void onExportQueueClicked();

    void onNextHotkey();
    void onVolumeUpHotkey();
    void onVolumeDownHotkey();

    void onSavePlaylistClicked();
    void onLoadPlaylistClicked();
    void onDeletePlaylistClicked();

    void onClockTick();

private:
    void setupUI();
    void updateButtons();

    void activateMedia(const QString &source, bool autoplay, bool focusPlayer);
    void loadMediaIntoState(const QString &source, bool updateScreen, bool focusPlayer);
    void setPlaybackState(double timestamp, bool playing);
    void setVolumePercent(int value, bool dispatchCommand);
    void handlePlayerEvent(const QJsonObject &event);
    void updateCurrentMediaMeta(const QString &title,
                                const QString &subtitle,
                                const QString &durationText,
                                const QString &sourceOverride = QString());

    bool ensurePlayerHttpServer();
    void handlePlayerHttpConnections();
    QByteArray loadModuleHtml(const char *filename) const;
    void dispatchPlayerCommand(QJsonObject payload, int attempts = 1, int delayMs = 250);
    void ensureBrowserSource();
    void setSourceVisible(bool visible);
    void dispatchBrowserEvent(obs_source_t *source, const QString &eventName, const QJsonObject &payload);
    bool writeLocalMediaResponse(QTcpSocket *socket,
                                 const QByteArray &method,
                                 const QUrl &requestUrl,
                                 const QList<QByteArray> &headerLines);

    double currentTime() const;
    QString formatTime(double seconds) const;
    QString formatDurationText(double seconds) const;

    QString normalizeMediaInput(const QString &input) const;
    bool isLikelyControllableSource(const QString &source) const;
    QString localFilePathFromSource(const QString &source) const;
    QString playbackUrlForSource(const QString &source);
    QString fallbackQueueTitle(const QString &source) const;
    QString fallbackQueueSubtitle(const QString &source) const;
    QString queueDisplayLabel(QListWidgetItem *item, int row) const;

    int enqueueVideosFromText(const QString &input);
    void addQueueItem(const QString &source, const QString &rawInput);
    void refreshQueueLabels();
    bool playQueuedItemAt(int row, bool focusPlayer = false);
    bool playNextQueuedVideo(bool focusPlayer = false);
    void requestQueueMetadata(const QString &itemId, const QString &source);
    void requestHtmlMetadata(const QString &itemId, const QString &source);
    void updateQueueItemMetadata(const QString &itemId,
                                 const QString &title,
                                 const QString &subtitle,
                                 const QString &durationText);
    QListWidgetItem *findQueueItemById(const QString &itemId) const;

    void saveQueue();
    void loadQueue();
    void saveQueueToFile(const QString &path);
    bool loadQueueFromFile(const QString &path);
    void refreshPlaylistCombo();
    QString queueConfigPath() const;
    QString playlistsDirPath() const;

    void setStatus(const QString &msg, bool error = false);

    QLineEdit   *m_urlInput       = nullptr;
    QPushButton *m_loadBtn        = nullptr;
    QPushButton *m_queueBtn       = nullptr;
    QLabel      *m_videoMeta      = nullptr;
    QPushButton *m_openPlayerBtn  = nullptr;

    QListWidget *m_queueList        = nullptr;
    QPushButton *m_queuePlayBtn     = nullptr;
    QPushButton *m_queueRemoveBtn   = nullptr;
    QPushButton *m_queueClearBtn    = nullptr;
    QPushButton *m_importQueueBtn   = nullptr;
    QPushButton *m_exportQueueBtn   = nullptr;

    QComboBox   *m_playlistCombo    = nullptr;
    QPushButton *m_savePlaylistBtn  = nullptr;
    QPushButton *m_loadPlaylistBtn  = nullptr;
    QPushButton *m_deletePlaylistBtn = nullptr;

    QPushButton *m_playBtn        = nullptr;
    QPushButton *m_pauseBtn       = nullptr;
    QPushButton *m_stopBtn        = nullptr;
    QSlider     *m_volumeSlider   = nullptr;
    QLabel      *m_volumeValue    = nullptr;
    QLabel      *m_timeLabel      = nullptr;
    QLabel      *m_stateLabel     = nullptr;
    QPushButton *m_showBtn        = nullptr;
    QPushButton *m_hideBtn        = nullptr;

    QLabel      *m_statusBar      = nullptr;

    QString      m_mediaSource;
    QString      m_currentTitle;
    QString      m_currentSubtitle;
    QString      m_currentDurationText;
    bool         m_isPlaying                 = false;
    bool         m_currentSourceControllable = false;
    int          m_volumePercent             = 100;
    double       m_savedTimestamp            = 0.0;
    double       m_playStartedAt             = 0.0;

    obs_source_t *m_browserSource = nullptr;

    QTimer                *m_clock         = nullptr;
    QTcpServer            *m_playerServer  = nullptr;
    QNetworkAccessManager *m_net           = nullptr;
    QByteArray             m_playerHtml;
    quint16                m_playerServerPort    = 0;
    quint64                m_nextPlayerCommandId = 1;

    QString      m_queuePlayingId;

    obs_hotkey_id m_hotkeyPlay    = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id m_hotkeyPause   = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id m_hotkeyStop    = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id m_hotkeyNext    = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id m_hotkeyVolUp   = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id m_hotkeyVolDown = OBS_INVALID_HOTKEY_ID;

    static constexpr const char *DISPLAY_NAME = "Studio Player";
    static constexpr const char *SOURCE_NAME = "Studio Player";
    static constexpr const char *LEGACY_SOURCE_NAME = "Watch Together";
};
