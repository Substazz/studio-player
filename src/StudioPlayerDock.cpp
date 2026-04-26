#include "StudioPlayerDock.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTcpSocket>
#include <QTextDocumentFragment>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {
constexpr int QueueIdRole = Qt::UserRole;
constexpr int QueueSourceRole = Qt::UserRole + 1;
constexpr int QueueRawInputRole = Qt::UserRole + 2;
constexpr int QueueTitleRole = Qt::UserRole + 3;
constexpr int QueueSubtitleRole = Qt::UserRole + 4;
constexpr int QueueDurationRole = Qt::UserRole + 5;

constexpr qint64 LocalMediaChunkSize = 256 * 1024;
constexpr const char *MetadataUserAgent = "StudioPlayerOBS/1.0";

int clampVolumePercent(int value)
{
    return qBound(0, value, 100);
}

QString simplifiedHtmlText(const QString &value)
{
    return QTextDocumentFragment::fromHtml(value).toPlainText().simplified();
}

QString normalizedHost(const QUrl &url)
{
    QString host = url.host().toLower().trimmed();
    host.remove(QRegularExpression("^(www\\.|m\\.|music\\.|old\\.|new\\.|np\\.)"));
    return host;
}

QString extractYouTubeId(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return {};

    static const QRegularExpression bareIdRe("^[A-Za-z0-9_-]{11}$");
    if (bareIdRe.match(trimmed).hasMatch())
        return trimmed;

    const QUrl url = QUrl::fromUserInput(trimmed);
    if (!url.isValid())
        return {};

    const QString host = normalizedHost(url);
    if (host == "youtu.be") {
        const QString candidate = url.path().section('/', 1, 1).left(11);
        if (bareIdRe.match(candidate).hasMatch())
            return candidate;
    }

    if (host == "youtube.com" || host == "youtube-nocookie.com") {
        if (url.path() == "/watch") {
            const QString candidate = QUrlQuery(url).queryItemValue("v");
            if (bareIdRe.match(candidate).hasMatch())
                return candidate;
        }

        const QRegularExpression pathRe("^/(?:shorts|embed|live)/([A-Za-z0-9_-]{11})(?:/.*)?$");
        const QRegularExpressionMatch match = pathRe.match(url.path());
        if (match.hasMatch())
            return match.captured(1);
    }

    return {};
}

QString extractRedditPostId(const QString &input)
{
    const QUrl url = QUrl::fromUserInput(input.trimmed());
    if (!url.isValid())
        return {};

    const QString host = normalizedHost(url);
    if (host == "redd.it") {
        const QString candidate = url.path().section('/', 1, 1);
        return candidate;
    }

    if (host != "reddit.com" && host != "redditmedia.com")
        return {};

    const QRegularExpression matchComments("/comments/([A-Za-z0-9]+)/");
    const QRegularExpressionMatch commentMatch = matchComments.match(url.path());
    if (commentMatch.hasMatch())
        return commentMatch.captured(1);

    const QRegularExpression matchMediaEmbed("/mediaembed/([A-Za-z0-9]+)");
    const QRegularExpressionMatch embedMatch = matchMediaEmbed.match(url.path());
    if (embedMatch.hasMatch())
        return embedMatch.captured(1);

    return {};
}

bool isDirectMediaUrl(const QString &source)
{
    const QUrl url = QUrl::fromUserInput(source);
    if (!url.isValid())
        return false;

    static const QRegularExpression mediaExtRe(
        "\\.(mp4|m4v|webm|ogv|ogg|mov|m3u8|mp3|m4a|wav|aac|flac)$",
        QRegularExpression::CaseInsensitiveOption);

    return mediaExtRe.match(url.path()).hasMatch();
}

QString lastPathSegment(const QUrl &url)
{
    const QString segment = url.path().section('/', -1);
    return QUrl::fromPercentEncoding(segment.toUtf8());
}

QString extractTwitchVodId(const QString &input)
{
    const QUrl url = QUrl::fromUserInput(input.trimmed());
    if (!url.isValid())
        return {};

    if (normalizedHost(url) != "twitch.tv")
        return {};

    static const QRegularExpression re("^/videos/(\\d+)$");
    const QRegularExpressionMatch match = re.match(url.path());
    return match.hasMatch() ? match.captured(1) : QString{};
}

QString extractTwitchClipId(const QString &input)
{
    const QUrl url = QUrl::fromUserInput(input.trimmed());
    if (!url.isValid())
        return {};

    const QString host = normalizedHost(url);

    if (host == "clips.twitch.tv") {
        const QString candidate = url.path().section('/', 1, 1);
        if (!candidate.isEmpty() && candidate != "embed")
            return candidate;
    }

    if (host == "twitch.tv") {
        static const QRegularExpression re("/[^/]+/clip/([^/?#]+)");
        const QRegularExpressionMatch match = re.match(url.path());
        if (match.hasMatch())
            return match.captured(1);
    }

    return {};
}

class MediaInputLineEdit final : public QLineEdit {
public:
    using QLineEdit::QLineEdit;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls() || event->mimeData()->hasText()) {
            event->acceptProposedAction();
            return;
        }

        QLineEdit::dragEnterEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        const QMimeData *mime = event->mimeData();
        if (mime->hasUrls()) {
            const QList<QUrl> urls = mime->urls();
            if (!urls.isEmpty()) {
                const QUrl first = urls.first();
                setText(first.isLocalFile() ? first.toLocalFile() : first.toString());
                event->acceptProposedAction();
                return;
            }
        }

        if (mime->hasText()) {
            setText(mime->text().trimmed());
            event->acceptProposedAction();
            return;
        }

        QLineEdit::dropEvent(event);
    }
};
}

StudioPlayerDock::StudioPlayerDock(QWidget *parent)
    : QDockWidget(tr(DISPLAY_NAME), parent)
{
    setObjectName("StudioPlayerDock");
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    setMinimumWidth(300);

    setupUI();

    m_net = new QNetworkAccessManager(this);
    m_playerServer = new QTcpServer(this);
    connect(m_playerServer, &QTcpServer::newConnection,
            this, &StudioPlayerDock::handlePlayerHttpConnections);

    m_clock = new QTimer(this);
    m_clock->setInterval(500);
    connect(m_clock, &QTimer::timeout, this, &StudioPlayerDock::onClockTick);
    m_clock->start();

    m_hotkeyPlay = obs_hotkey_register_frontend(
        "studio_player_play", "Studio Player: Play",
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed)
                QMetaObject::invokeMethod(static_cast<StudioPlayerDock *>(data),
                                          "onPlayClicked", Qt::QueuedConnection);
        }, this);
    m_hotkeyPause = obs_hotkey_register_frontend(
        "studio_player_pause", "Studio Player: Pause",
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed)
                QMetaObject::invokeMethod(static_cast<StudioPlayerDock *>(data),
                                          "onPauseClicked", Qt::QueuedConnection);
        }, this);
    m_hotkeyStop = obs_hotkey_register_frontend(
        "studio_player_stop", "Studio Player: Stop",
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed)
                QMetaObject::invokeMethod(static_cast<StudioPlayerDock *>(data),
                                          "onStopClicked", Qt::QueuedConnection);
        }, this);
    m_hotkeyNext = obs_hotkey_register_frontend(
        "studio_player_next", "Studio Player: Next",
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed)
                QMetaObject::invokeMethod(static_cast<StudioPlayerDock *>(data),
                                          "onNextHotkey", Qt::QueuedConnection);
        }, this);
    m_hotkeyVolUp = obs_hotkey_register_frontend(
        "studio_player_vol_up", "Studio Player: Volume Up",
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed)
                QMetaObject::invokeMethod(static_cast<StudioPlayerDock *>(data),
                                          "onVolumeUpHotkey", Qt::QueuedConnection);
        }, this);
    m_hotkeyVolDown = obs_hotkey_register_frontend(
        "studio_player_vol_down", "Studio Player: Volume Down",
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed)
                QMetaObject::invokeMethod(static_cast<StudioPlayerDock *>(data),
                                          "onVolumeDownHotkey", Qt::QueuedConnection);
        }, this);

    loadQueue();
    refreshPlaylistCombo();
}

StudioPlayerDock::~StudioPlayerDock()
{
    if (m_clock)
        m_clock->stop();

    if (m_browserSource) {
        obs_source_release(m_browserSource);
        m_browserSource = nullptr;
    }

    for (obs_hotkey_id id : {m_hotkeyPlay, m_hotkeyPause, m_hotkeyStop,
                              m_hotkeyNext, m_hotkeyVolUp, m_hotkeyVolDown}) {
        if (id != OBS_INVALID_HOTKEY_ID)
            obs_hotkey_unregister(id);
    }
}

void StudioPlayerDock::setupUI()
{
    auto *container = new QWidget();
    auto *scroll = new QScrollArea(this);
    scroll->setWidget(container);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWidget(scroll);

    auto *root = new QVBoxLayout(container);
    root->setSpacing(8);
    root->setContentsMargins(8, 8, 8, 8);

    auto *videoBox = new QGroupBox(tr("Video"));
    auto *videoLayout = new QVBoxLayout(videoBox);
    videoLayout->setSpacing(6);

    m_urlInput = new MediaInputLineEdit();
    m_urlInput->setAcceptDrops(true);
    m_urlInput->setClearButtonEnabled(true);
    m_urlInput->setPlaceholderText(tr("Paste a link or drop a local media file here"));

    auto *actionRow = new QHBoxLayout();
    actionRow->setSpacing(6);
    m_loadBtn = new QPushButton(tr("Load"));
    m_queueBtn = new QPushButton(tr("Queue"));
    m_loadBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_queueBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    actionRow->addWidget(m_loadBtn);
    actionRow->addWidget(m_queueBtn);

    m_videoMeta = new QLabel();
    m_videoMeta->setStyleSheet("color: gray; font-size: 11px;");
    m_videoMeta->setWordWrap(true);
    m_videoMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_videoMeta->hide();

    m_openPlayerBtn = new QPushButton(tr("Open Player Window"));
    m_openPlayerBtn->setEnabled(false);

    videoLayout->addWidget(m_urlInput);
    videoLayout->addLayout(actionRow);
    videoLayout->addWidget(m_videoMeta);
    videoLayout->addWidget(m_openPlayerBtn);
    root->addWidget(videoBox);

    connect(m_loadBtn, &QPushButton::clicked, this, &StudioPlayerDock::onLoadClicked);
    connect(m_queueBtn, &QPushButton::clicked, this, &StudioPlayerDock::onQueueClicked);
    connect(m_urlInput, &QLineEdit::returnPressed, this, &StudioPlayerDock::onLoadClicked);
    connect(m_openPlayerBtn, &QPushButton::clicked, this, &StudioPlayerDock::onOpenPlayerClicked);

    auto *queueBox = new QGroupBox(tr("Queue"));
    auto *queueLayout = new QVBoxLayout(queueBox);
    queueLayout->setSpacing(6);

    m_queueList = new QListWidget();
    m_queueList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_queueList->setDragDropMode(QAbstractItemView::InternalMove);
    m_queueList->setDefaultDropAction(Qt::MoveAction);
    m_queueList->setDropIndicatorShown(true);
    m_queueList->setAlternatingRowColors(true);
    m_queueList->setMinimumHeight(168);
    m_queueList->setWordWrap(true);
    m_queueList->setSpacing(2);

    auto *queueHint = new QLabel(tr("Drag to reorder. Titles and lengths appear when available."));
    queueHint->setStyleSheet("color: gray; font-size: 11px;");
    queueHint->setWordWrap(true);

    auto *queueBtnRow = new QHBoxLayout();
    queueBtnRow->setSpacing(6);
    m_queuePlayBtn = new QPushButton(tr("Play Selected"));
    m_queueRemoveBtn = new QPushButton(tr("Remove"));
    m_queueClearBtn = new QPushButton(tr("Clear"));
    m_queuePlayBtn->setEnabled(false);
    m_queueRemoveBtn->setEnabled(false);
    m_queueClearBtn->setEnabled(false);
    queueBtnRow->addWidget(m_queuePlayBtn, 2);
    queueBtnRow->addWidget(m_queueRemoveBtn, 1);
    queueBtnRow->addWidget(m_queueClearBtn, 1);

    auto *queueIoRow = new QHBoxLayout();
    queueIoRow->setSpacing(6);
    m_importQueueBtn = new QPushButton(tr("Import…"));
    m_exportQueueBtn = new QPushButton(tr("Export…"));
    m_importQueueBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_exportQueueBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    queueIoRow->addWidget(m_importQueueBtn);
    queueIoRow->addWidget(m_exportQueueBtn);

    queueLayout->addWidget(m_queueList);
    queueLayout->addWidget(queueHint);
    queueLayout->addLayout(queueBtnRow);
    queueLayout->addLayout(queueIoRow);
    root->addWidget(queueBox);

    connect(m_queuePlayBtn, &QPushButton::clicked, this, &StudioPlayerDock::onQueuePlaySelectedClicked);
    connect(m_queueRemoveBtn, &QPushButton::clicked, this, &StudioPlayerDock::onQueueRemoveClicked);
    connect(m_queueClearBtn, &QPushButton::clicked, this, &StudioPlayerDock::onQueueClearClicked);
    connect(m_importQueueBtn, &QPushButton::clicked, this, &StudioPlayerDock::onImportQueueClicked);
    connect(m_exportQueueBtn, &QPushButton::clicked, this, &StudioPlayerDock::onExportQueueClicked);
    connect(m_queueList, &QListWidget::itemSelectionChanged, this, &StudioPlayerDock::onQueueSelectionChanged);
    connect(m_queueList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        onQueuePlaySelectedClicked();
    });
    connect(m_queueList->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        refreshQueueLabels();
        saveQueue();
        setStatus(tr("Queue reordered"));
        updateButtons();
    });

    auto *playlistBox = new QGroupBox(tr("Playlists"));
    auto *playlistLayout = new QHBoxLayout(playlistBox);
    playlistLayout->setSpacing(6);
    m_playlistCombo = new QComboBox();
    m_playlistCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_savePlaylistBtn = new QPushButton(tr("Save As"));
    m_loadPlaylistBtn = new QPushButton(tr("Load"));
    m_deletePlaylistBtn = new QPushButton(tr("Delete"));
    m_loadPlaylistBtn->setEnabled(false);
    m_deletePlaylistBtn->setEnabled(false);
    playlistLayout->addWidget(m_playlistCombo, 1);
    playlistLayout->addWidget(m_savePlaylistBtn);
    playlistLayout->addWidget(m_loadPlaylistBtn);
    playlistLayout->addWidget(m_deletePlaylistBtn);
    root->addWidget(playlistBox);

    connect(m_savePlaylistBtn, &QPushButton::clicked, this, &StudioPlayerDock::onSavePlaylistClicked);
    connect(m_loadPlaylistBtn, &QPushButton::clicked, this, &StudioPlayerDock::onLoadPlaylistClicked);
    connect(m_deletePlaylistBtn, &QPushButton::clicked, this, &StudioPlayerDock::onDeletePlaylistClicked);
    connect(m_playlistCombo, &QComboBox::currentTextChanged, this, [this]() {
        const bool hasItems = m_playlistCombo->count() > 0;
        m_loadPlaylistBtn->setEnabled(hasItems);
        m_deletePlaylistBtn->setEnabled(hasItems);
    });

    auto *playbackBox = new QGroupBox(tr("Playback"));
    auto *playbackLayout = new QVBoxLayout(playbackBox);
    playbackLayout->setSpacing(6);

    auto *playbackButtons = new QHBoxLayout();
    playbackButtons->setSpacing(6);
    m_playBtn = new QPushButton(tr("Play"));
    m_pauseBtn = new QPushButton(tr("Pause"));
    m_stopBtn = new QPushButton(tr("Stop"));
    for (auto *button : {m_playBtn, m_pauseBtn, m_stopBtn}) {
        button->setEnabled(false);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        playbackButtons->addWidget(button);
    }

    auto *infoRow = new QHBoxLayout();
    m_timeLabel = new QLabel("0:00");
    m_timeLabel->setStyleSheet("font-weight: bold;");
    m_stateLabel = new QLabel(tr("No video loaded"));
    m_stateLabel->setStyleSheet("color: gray; font-size: 11px;");
    infoRow->addWidget(m_timeLabel);
    infoRow->addStretch();
    infoRow->addWidget(m_stateLabel);

    playbackLayout->addLayout(playbackButtons);
    playbackLayout->addLayout(infoRow);

    auto *volumeRow = new QHBoxLayout();
    volumeRow->setSpacing(8);
    auto *volumeLabel = new QLabel(tr("Volume"));
    volumeLabel->setStyleSheet("font-size: 11px; color: gray;");
    m_volumeSlider = new QSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setSingleStep(1);
    m_volumeSlider->setPageStep(10);
    m_volumeSlider->setValue(m_volumePercent);
    m_volumeSlider->setEnabled(false);
    m_volumeValue = new QLabel(QString("%1%").arg(m_volumePercent));
    m_volumeValue->setMinimumWidth(42);
    m_volumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeValue->setStyleSheet("font-size: 11px; color: gray;");
    volumeRow->addWidget(volumeLabel);
    volumeRow->addWidget(m_volumeSlider, 1);
    volumeRow->addWidget(m_volumeValue);

    playbackLayout->addLayout(volumeRow);
    root->addWidget(playbackBox);

    connect(m_playBtn, &QPushButton::clicked, this, &StudioPlayerDock::onPlayClicked);
    connect(m_pauseBtn, &QPushButton::clicked, this, &StudioPlayerDock::onPauseClicked);
    connect(m_stopBtn, &QPushButton::clicked, this, &StudioPlayerDock::onStopClicked);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &StudioPlayerDock::onVolumeChanged);

    auto *screenBox = new QGroupBox(tr("On Screen"));
    auto *screenLayout = new QHBoxLayout(screenBox);
    screenLayout->setSpacing(6);
    m_showBtn = new QPushButton(tr("Show"));
    m_hideBtn = new QPushButton(tr("Hide"));
    m_showBtn->setEnabled(false);
    m_hideBtn->setEnabled(false);
    m_showBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_hideBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    screenLayout->addWidget(m_showBtn);
    screenLayout->addWidget(m_hideBtn);
    root->addWidget(screenBox);

    connect(m_showBtn, &QPushButton::clicked, this, &StudioPlayerDock::onShowClicked);
    connect(m_hideBtn, &QPushButton::clicked, this, &StudioPlayerDock::onHideClicked);

    m_statusBar = new QLabel();
    m_statusBar->setStyleSheet("color: gray; font-size: 10px;");
    m_statusBar->setWordWrap(true);
    root->addWidget(m_statusBar);
    root->addStretch();
}

void StudioPlayerDock::activateMedia(const QString &source, bool autoplay, bool focusPlayer)
{
    loadMediaIntoState(source, true, focusPlayer);

    if (!autoplay || !m_currentSourceControllable)
        return;

    setPlaybackState(0.0, true);
    dispatchPlayerCommand({{"type", "play"}, {"seconds", 0.0}}, 6, 250);
    updateButtons();
}

int StudioPlayerDock::enqueueVideosFromText(const QString &input)
{
    const QString source = normalizeMediaInput(input);
    if (!source.isEmpty()) {
        addQueueItem(source, input);
        refreshQueueLabels();
        return 1;
    }

    const QStringList rawInputs = input.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    int added = 0;
    for (const QString &raw : rawInputs) {
        const QString normalized = normalizeMediaInput(raw);
        if (normalized.isEmpty())
            continue;

        addQueueItem(normalized, raw);
        ++added;
    }

    if (added > 0)
        refreshQueueLabels();

    return added;
}

QString StudioPlayerDock::fallbackQueueTitle(const QString &source) const
{
    const QString localPath = localFilePathFromSource(source);
    if (!localPath.isEmpty())
        return QFileInfo(localPath).fileName();

    if (!extractYouTubeId(source).isEmpty())
        return tr("YouTube video");

    if (!extractRedditPostId(source).isEmpty())
        return tr("Reddit post");

    const QString twitchVodId = extractTwitchVodId(source);
    if (!twitchVodId.isEmpty())
        return tr("Twitch VOD %1").arg(twitchVodId);

    const QString twitchClipId = extractTwitchClipId(source);
    if (!twitchClipId.isEmpty())
        return twitchClipId;

    const QUrl url = QUrl::fromUserInput(source);
    if (!url.isValid())
        return source.simplified();

    const QString pathName = lastPathSegment(url).trimmed();
    if (!pathName.isEmpty())
        return pathName;

    const QString host = normalizedHost(url);
    if (!host.isEmpty())
        return host;

    return source.simplified();
}

QString StudioPlayerDock::fallbackQueueSubtitle(const QString &source) const
{
    const QString localPath = localFilePathFromSource(source);
    if (!localPath.isEmpty())
        return tr("Local file");

    if (!extractTwitchVodId(source).isEmpty())
        return tr("Twitch");

    if (!extractTwitchClipId(source).isEmpty())
        return tr("Twitch Clip");

    const QUrl url = QUrl::fromUserInput(source);
    const QString host = normalizedHost(url);
    if (host.isEmpty())
        return {};

    if (!extractYouTubeId(source).isEmpty())
        return tr("YouTube");

    if (!extractRedditPostId(source).isEmpty())
        return tr("Reddit");

    return host;
}

void StudioPlayerDock::addQueueItem(const QString &source, const QString &rawInput)
{
    auto *item = new QListWidgetItem();
    const QString itemId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item->setData(QueueIdRole, itemId);
    item->setData(QueueSourceRole, source);
    item->setData(QueueRawInputRole, rawInput.simplified());
    item->setData(QueueTitleRole, fallbackQueueTitle(source));
    item->setData(QueueSubtitleRole, fallbackQueueSubtitle(source));
    item->setData(QueueDurationRole, QString());
    item->setToolTip(QString("Source: %1").arg(source));
    m_queueList->addItem(item);
    requestQueueMetadata(itemId, source);
}

QString StudioPlayerDock::queueDisplayLabel(QListWidgetItem *item, int row) const
{
    if (!item)
        return {};

    QString title = item->data(QueueTitleRole).toString().simplified();
    QString subtitle = item->data(QueueSubtitleRole).toString().simplified();
    const QString durationText = item->data(QueueDurationRole).toString().simplified();

    if (title.isEmpty())
        title = fallbackQueueTitle(item->data(QueueSourceRole).toString());
    if (subtitle.isEmpty())
        subtitle = fallbackQueueSubtitle(item->data(QueueSourceRole).toString());

    if (title.size() > 74)
        title = title.left(71) + "...";
    if (subtitle.size() > 48)
        subtitle = subtitle.left(45) + "...";

    QString secondLine = subtitle;
    if (!durationText.isEmpty())
        secondLine = secondLine.isEmpty() ? durationText : secondLine + " • " + durationText;

    const QString firstLine = QString("%1. %2").arg(row + 1).arg(title);
    const QString label = secondLine.isEmpty() ? firstLine : firstLine + "\n" + secondLine;

    const bool isPlaying = !m_queuePlayingId.isEmpty() &&
        item->data(QueueIdRole).toString() == m_queuePlayingId;
    return isPlaying ? "\u25B6 " + label : label;
}

void StudioPlayerDock::refreshQueueLabels()
{
    if (!m_queueList)
        return;

    for (int i = 0; i < m_queueList->count(); ++i) {
        QListWidgetItem *item = m_queueList->item(i);
        item->setText(queueDisplayLabel(item, i));
        item->setSizeHint(QSize(0, item->text().contains('\n') ? 44 : 26));
    }
}

QListWidgetItem *StudioPlayerDock::findQueueItemById(const QString &itemId) const
{
    if (!m_queueList || itemId.isEmpty())
        return nullptr;

    for (int i = 0; i < m_queueList->count(); ++i) {
        QListWidgetItem *item = m_queueList->item(i);
        if (item && item->data(QueueIdRole).toString() == itemId)
            return item;
    }

    return nullptr;
}

void StudioPlayerDock::updateQueueItemMetadata(const QString &itemId,
                                                const QString &title,
                                                const QString &subtitle,
                                                const QString &durationText)
{
    QListWidgetItem *item = findQueueItemById(itemId);
    if (!item)
        return;

    if (!title.trimmed().isEmpty())
        item->setData(QueueTitleRole, title.trimmed());
    if (!subtitle.trimmed().isEmpty())
        item->setData(QueueSubtitleRole, subtitle.trimmed());
    if (!durationText.trimmed().isEmpty())
        item->setData(QueueDurationRole, durationText.trimmed());

    refreshQueueLabels();
}

void StudioPlayerDock::requestHtmlMetadata(const QString &itemId, const QString &source)
{
    if (!m_net)
        return;

    const QUrl url = QUrl::fromUserInput(source);
    if (!url.isValid())
        return;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, MetadataUserAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, itemId, source, reply]() {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        if (!ok || body.isEmpty())
            return;

        const QString html = QString::fromUtf8(body);

        auto findMeta = [&html](const QString &pattern) -> QString {
            const QRegularExpression re(pattern,
                QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::DotMatchesEverythingOption);
            const QRegularExpressionMatch match = re.match(html);
            if (!match.hasMatch())
                return {};

            return simplifiedHtmlText(match.captured(1));
        };

        QString title = findMeta("<meta[^>]+property=[\"']og:title[\"'][^>]+content=[\"']([^\"']+)[\"']");
        if (title.isEmpty())
            title = findMeta("<title[^>]*>(.*?)</title>");

        QString subtitle;
        if (!extractYouTubeId(source).isEmpty()) {
            subtitle = findMeta("<link[^>]+itemprop=[\"']name[\"'][^>]+content=[\"']([^\"']+)[\"']");
            if (subtitle.isEmpty())
                subtitle = findMeta("\"ownerChannelName\":\"([^\"]+)\"");
        }

        QString durationText;
        QRegularExpression lengthSecondsRe("\"lengthSeconds\":\"(\\d+)\"");
        QRegularExpressionMatch secondsMatch = lengthSecondsRe.match(html);
        if (secondsMatch.hasMatch()) {
            durationText = formatDurationText(secondsMatch.captured(1).toDouble());
        } else {
            QRegularExpression approxDurationRe("\"approxDurationMs\":\"(\\d+)\"");
            QRegularExpressionMatch durationMatch = approxDurationRe.match(html);
            if (durationMatch.hasMatch())
                durationText = formatDurationText(durationMatch.captured(1).toDouble() / 1000.0);
        }

        if (durationText.isEmpty()) {
            QRegularExpression ogDurationRe(
                "<meta[^>]+property=[\"']video:duration[\"'][^>]+content=[\"'](\\d+)[\"']",
                QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch ogDurationMatch = ogDurationRe.match(html);
            if (ogDurationMatch.hasMatch())
                durationText = formatDurationText(ogDurationMatch.captured(1).toDouble());
        }

        updateQueueItemMetadata(itemId, title, subtitle, durationText);
    });
}

void StudioPlayerDock::requestQueueMetadata(const QString &itemId, const QString &source)
{
    if (!m_net)
        return;

    if (!localFilePathFromSource(source).isEmpty())
        return;

    const QString twitchVodId = extractTwitchVodId(source);
    if (!twitchVodId.isEmpty()) {
        updateQueueItemMetadata(itemId, tr("Twitch VOD %1").arg(twitchVodId), tr("Twitch"), {});
        return;
    }

    const QString twitchClipId = extractTwitchClipId(source);
    if (!twitchClipId.isEmpty()) {
        updateQueueItemMetadata(itemId, twitchClipId, tr("Twitch Clip"), {});
        return;
    }

    const QUrl sourceUrl = QUrl::fromUserInput(source);
    const QString host = normalizedHost(sourceUrl);

    // Reddit: use JSON API directly — bypasses bot-detection that plagues oembed/HTML scraping
    const QString redditPostId = extractRedditPostId(source);
    if (!redditPostId.isEmpty()) {
        const QUrl jsonUrl(QString("https://www.reddit.com/comments/%1.json?raw_json=1&limit=1")
                               .arg(redditPostId));
        QNetworkRequest request(jsonUrl);
        request.setHeader(QNetworkRequest::UserAgentHeader, MetadataUserAgent);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply *reply = m_net->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, itemId, reply]() {
            const QByteArray body = reply->readAll();
            const bool ok = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();

            if (!ok || body.isEmpty())
                return;

            QJsonParseError parseError{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isArray())
                return;

            const QJsonObject postData = doc.array().first().toObject()
                                             .value("data").toObject()
                                             .value("children").toArray()
                                             .first().toObject()
                                             .value("data").toObject();

            const QString title = postData.value("title").toString().trimmed();
            const QString subreddit = postData.value("subreddit_name_prefixed").toString().trimmed();

            QString durationText;
            const double duration = postData.value("secure_media").toObject()
                                        .value("reddit_video").toObject()
                                        .value("duration").toDouble();
            if (duration > 0)
                durationText = formatDurationText(duration);

            updateQueueItemMetadata(itemId, title, subreddit, durationText);
        });
        return;
    }

    QUrl oembedUrl;
    if (!extractYouTubeId(source).isEmpty()) {
        oembedUrl = QUrl("https://www.youtube.com/oembed");
    } else if (host == "vimeo.com" || host == "player.vimeo.com") {
        oembedUrl = QUrl("https://vimeo.com/api/oembed.json");
    } else if (host == "dailymotion.com" || host == "dai.ly") {
        oembedUrl = QUrl("https://www.dailymotion.com/services/oembed");
    }

    if (!oembedUrl.isValid() || oembedUrl.isEmpty()) {
        if (!isDirectMediaUrl(source))
            requestHtmlMetadata(itemId, source);
        return;
    }

    QUrlQuery query;
    query.addQueryItem("url", source);
    query.addQueryItem("format", "json");
    oembedUrl.setQuery(query);

    QNetworkRequest request(oembedUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, MetadataUserAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, itemId, source, reply]() {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        if (!ok || body.isEmpty()) {
            if (!isDirectMediaUrl(source))
                requestHtmlMetadata(itemId, source);
            return;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (!isDirectMediaUrl(source))
                requestHtmlMetadata(itemId, source);
            return;
        }

        const QJsonObject obj = doc.object();
        const QString title = obj.value("title").toString().trimmed();
        QString subtitle = obj.value("author_name").toString().trimmed();
        if (subtitle.isEmpty())
            subtitle = obj.value("provider_name").toString().trimmed();

        QString durationText;
        if (obj.contains("duration"))
            durationText = formatDurationText(obj.value("duration").toDouble());

        updateQueueItemMetadata(itemId, title, subtitle, durationText);

        if (!extractYouTubeId(source).isEmpty() || durationText.isEmpty())
            requestHtmlMetadata(itemId, source);
    });
}

bool StudioPlayerDock::playQueuedItemAt(int row, bool focusPlayer)
{
    if (!m_queueList || row < 0 || row >= m_queueList->count())
        return false;

    QListWidgetItem *item = m_queueList->item(row);
    if (!item)
        return false;

    const QString source = item->data(QueueSourceRole).toString();
    if (source.isEmpty())
        return false;

    m_queuePlayingId = item->data(QueueIdRole).toString();
    m_queueList->setCurrentRow(row);
    refreshQueueLabels();

    activateMedia(source, true, focusPlayer);
    setStatus(tr("Now playing queued item"));
    return true;
}

bool StudioPlayerDock::playNextQueuedVideo(bool focusPlayer)
{
    if (!m_queueList || m_queueList->count() == 0) {
        m_queuePlayingId.clear();
        return false;
    }

    int nextRow = 0;
    if (!m_queuePlayingId.isEmpty()) {
        for (int i = 0; i < m_queueList->count(); ++i) {
            if (m_queueList->item(i)->data(QueueIdRole).toString() == m_queuePlayingId) {
                nextRow = i + 1;
                break;
            }
        }
    }

    if (nextRow >= m_queueList->count()) {
        m_queuePlayingId.clear();
        refreshQueueLabels();
        return false;
    }

    return playQueuedItemAt(nextRow, focusPlayer);
}

QString StudioPlayerDock::localFilePathFromSource(const QString &source) const
{
    const QString trimmed = source.trimmed();
    if (trimmed.isEmpty())
        return {};

    const QUrl url = QUrl::fromUserInput(trimmed);
    if (url.isValid() && url.isLocalFile()) {
        QFileInfo info(url.toLocalFile());
        if (info.exists() && info.isFile())
            return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    }

    QFileInfo info(trimmed);
    if (info.exists() && info.isFile())
        return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();

    return {};
}

QString StudioPlayerDock::playbackUrlForSource(const QString &source)
{
    const QString localPath = localFilePathFromSource(source);
    if (localPath.isEmpty())
        return source;

    if (!ensurePlayerHttpServer())
        return QUrl::fromLocalFile(localPath).toString(QUrl::FullyEncoded);

    const QString fileSource = QUrl::fromLocalFile(localPath).toString(QUrl::FullyEncoded);
    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(fileSource));
    return QString("http://127.0.0.1:%1/media?source=%2")
        .arg(m_playerServerPort)
        .arg(encoded);
}

void StudioPlayerDock::updateCurrentMediaMeta(const QString &title,
                                               const QString &subtitle,
                                               const QString &durationText,
                                               const QString &sourceOverride)
{
    if (!title.trimmed().isEmpty())
        m_currentTitle = title.trimmed();
    if (!subtitle.trimmed().isEmpty())
        m_currentSubtitle = subtitle.trimmed();
    if (!durationText.trimmed().isEmpty())
        m_currentDurationText = durationText.trimmed();

    QString heading = m_currentTitle;
    if (heading.isEmpty()) {
        const QString effectiveSource = sourceOverride.isEmpty() ? m_mediaSource : sourceOverride;
        heading = fallbackQueueTitle(effectiveSource);
    }

    if (!m_currentDurationText.isEmpty())
        heading = heading + " • " + m_currentDurationText;

    QString detail = m_currentSubtitle;
    if (detail.isEmpty()) {
        const QString effectiveSource = sourceOverride.isEmpty() ? m_mediaSource : sourceOverride;
        detail = fallbackQueueSubtitle(effectiveSource);
    }

    QStringList lines;
    if (!heading.isEmpty())
        lines << heading;
    if (!detail.isEmpty())
        lines << detail;

    if (lines.isEmpty()) {
        m_videoMeta->hide();
        return;
    }

    m_videoMeta->setText(lines.join("\n"));
    const QString tooltipSource = sourceOverride.isEmpty() ? m_mediaSource : sourceOverride;
    m_videoMeta->setToolTip(tooltipSource);
    m_videoMeta->show();
}

void StudioPlayerDock::loadMediaIntoState(const QString &source, bool updateScreen, bool focusPlayer)
{
    m_mediaSource = source;
    m_savedTimestamp = 0.0;
    m_isPlaying = false;
    m_playStartedAt = 0.0;
    m_currentTitle = fallbackQueueTitle(source);
    m_currentSubtitle = fallbackQueueSubtitle(source);
    m_currentDurationText.clear();
    m_currentSourceControllable = isLikelyControllableSource(source);

    updateCurrentMediaMeta(m_currentTitle, m_currentSubtitle, QString(), source);
    m_stateLabel->setText(m_currentSourceControllable
                              ? tr("Loaded - press Play")
                              : tr("Embedded page loaded"));
    m_timeLabel->setText(m_currentSourceControllable ? "0:00" : tr("Embed"));

    if (updateScreen) {
        ensureBrowserSource();
        QJsonObject payload{
            {"type", "load"},
            {"source", source},
            {"playbackUrl", playbackUrlForSource(source)},
            {"volume", m_volumePercent}
        };
        dispatchPlayerCommand(payload, 6);
    }

    if (focusPlayer && !m_browserSource)
        ensureBrowserSource();

    if (focusPlayer && m_browserSource)
        obs_frontend_open_source_interaction(m_browserSource);

    setStatus(tr("Video loaded"));
    updateButtons();
}

void StudioPlayerDock::setPlaybackState(double timestamp, bool playing)
{
    m_savedTimestamp = qMax(0.0, timestamp);
    m_isPlaying = playing;

    if (playing) {
        m_playStartedAt = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
        m_stateLabel->setText(tr("Playing"));
    } else {
        m_stateLabel->setText(tr("Paused"));
    }
}

void StudioPlayerDock::setVolumePercent(int value, bool dispatchCommand)
{
    const int clamped = clampVolumePercent(value);
    m_volumePercent = clamped;

    if (m_volumeSlider && m_volumeSlider->value() != clamped) {
        const QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(clamped);
    }

    if (m_volumeValue)
        m_volumeValue->setText(QString("%1%").arg(clamped));

    if (dispatchCommand && !m_mediaSource.isEmpty() && m_currentSourceControllable)
        dispatchPlayerCommand({{"type", "volume"}, {"volume", clamped}}, 2, 120);
}


void StudioPlayerDock::handlePlayerEvent(const QJsonObject &event)
{
    const QString type = event.value("type").toString();
    const QString source = event.value("source").toString();
    const double timestamp = event.value("currentTime").toDouble();
    const bool paused = event.value("paused").toBool(true);
    const double durationSeconds = event.value("duration").toDouble();

    if (!source.isEmpty() && source != m_mediaSource &&
        (type == "loaded" || type == "ready" || type == "play")) {
        loadMediaIntoState(source, false, false);
    }

    if (event.contains("controllable"))
        m_currentSourceControllable = event.value("controllable").toBool(m_currentSourceControllable);
    if (event.contains("volume"))
        setVolumePercent(event.value("volume").toInt(m_volumePercent), false);
    QString title = event.value("title").toString().trimmed();
    QString subtitle = event.value("channel").toString().trimmed();
    QString durationText;
    if (durationSeconds > 0.0)
        durationText = formatDurationText(durationSeconds);

    updateCurrentMediaMeta(title, subtitle, durationText, source.isEmpty() ? m_mediaSource : source);

    if (type == "loaded" || type == "ready") {
        m_savedTimestamp = qMax(0.0, timestamp);
        m_isPlaying = false;
        m_playStartedAt = 0.0;
        m_stateLabel->setText(m_currentSourceControllable
                                  ? tr("Loaded - press Play")
                                  : tr("Embedded page loaded"));
    } else if (type == "play") {
        setPlaybackState(timestamp, true);
    } else if (type == "pause") {
        setPlaybackState(timestamp, false);
    } else if (type == "seek") {
        setPlaybackState(timestamp, !paused);
    } else if (type == "stopped") {
        m_savedTimestamp = 0.0;
        m_isPlaying = false;
        m_playStartedAt = 0.0;
        m_stateLabel->setText(tr("Stopped"));
    } else if (type == "ended") {
        setPlaybackState(timestamp, false);
        if (playNextQueuedVideo(false))
            return;

        m_stateLabel->setText(tr("Ended"));
    }

    if (!m_currentSourceControllable && !m_mediaSource.isEmpty())
        m_timeLabel->setText(tr("Embed"));

    updateButtons();
}

void StudioPlayerDock::onLoadClicked()
{
    const QString source = normalizeMediaInput(m_urlInput->text().trimmed());
    if (source.isEmpty()) {
        setStatus(tr("Enter a valid link or drop a local media file"), true);
        return;
    }

    activateMedia(source, false, true);
}

void StudioPlayerDock::onQueueClicked()
{
    const QString input = m_urlInput->text().trimmed();
    if (input.isEmpty()) {
        setStatus(tr("Paste one or more links or drop a local media file"), true);
        return;
    }

    const int added = enqueueVideosFromText(input);
    if (added <= 0) {
        setStatus(tr("No valid items found to queue"), true);
        return;
    }

    m_urlInput->clear();
    saveQueue();
    setStatus(tr("Added %1 item(s) to queue").arg(added));
    updateButtons();
}

void StudioPlayerDock::onQueuePlaySelectedClicked()
{
    const int row = m_queueList ? m_queueList->currentRow() : -1;
    if (!playQueuedItemAt(row, false))
        setStatus(tr("Select a queued item to play"), true);

    updateButtons();
}

void StudioPlayerDock::onQueueRemoveClicked()
{
    if (!m_queueList)
        return;

    QListWidgetItem *item = m_queueList->takeItem(m_queueList->currentRow());
    if (!item) {
        setStatus(tr("Select a queued item to remove"), true);
        return;
    }

    if (item->data(QueueIdRole).toString() == m_queuePlayingId)
        m_queuePlayingId.clear();

    delete item;
    refreshQueueLabels();
    saveQueue();
    setStatus(tr("Removed item from queue"));
    updateButtons();
}

void StudioPlayerDock::onQueueClearClicked()
{
    if (!m_queueList || m_queueList->count() == 0)
        return;

    m_queuePlayingId.clear();
    m_queueList->clear();
    saveQueue();
    setStatus(tr("Queue cleared"));
    updateButtons();
}

void StudioPlayerDock::onQueueSelectionChanged()
{
    updateButtons();
}

void StudioPlayerDock::onPlayClicked()
{
    if (m_mediaSource.isEmpty() || !m_currentSourceControllable)
        return;

    const double timestamp = currentTime();
    setPlaybackState(timestamp, true);
    dispatchPlayerCommand({{"type", "play"}, {"seconds", timestamp}}, 3);
    updateButtons();
}

void StudioPlayerDock::onPauseClicked()
{
    if (m_mediaSource.isEmpty() || !m_currentSourceControllable)
        return;

    const double timestamp = currentTime();
    setPlaybackState(timestamp, false);
    dispatchPlayerCommand({{"type", "pause"}, {"seconds", timestamp}}, 2);
    updateButtons();
}

void StudioPlayerDock::onStopClicked()
{
    if (m_mediaSource.isEmpty())
        return;

    m_savedTimestamp = 0.0;
    m_isPlaying = false;
    m_playStartedAt = 0.0;
    m_stateLabel->setText(tr("Stopped"));
    m_timeLabel->setText(m_currentSourceControllable ? "0:00" : tr("Embed"));

    dispatchPlayerCommand({{"type", "stop"}}, 2);
    setStatus(tr("Playback stopped"));
    updateButtons();
}

void StudioPlayerDock::onVolumeChanged(int value)
{
    setVolumePercent(value, true);
}

void StudioPlayerDock::onShowClicked()
{
    setSourceVisible(true);
}

void StudioPlayerDock::onHideClicked()
{
    setSourceVisible(false);
}

void StudioPlayerDock::onOpenPlayerClicked()
{
    if (m_mediaSource.isEmpty()) {
        setStatus(tr("Load a video first"), true);
        return;
    }

    ensureBrowserSource();
    if (!m_browserSource)
        return;

    obs_frontend_open_source_interaction(m_browserSource);
}

bool StudioPlayerDock::ensurePlayerHttpServer()
{
    if (m_playerServer->isListening())
        return true;

    m_playerHtml = loadModuleHtml("player.html");
    if (m_playerHtml.isEmpty()) {
        setStatus(tr("Failed to load local player page"), true);
        return false;
    }

    if (!m_playerServer->listen(QHostAddress::LocalHost, 0)) {
        setStatus(tr("Failed to start local player server: ") + m_playerServer->errorString(), true);
        return false;
    }

    m_playerServerPort = m_playerServer->serverPort();
    return true;
}

bool StudioPlayerDock::writeLocalMediaResponse(QTcpSocket *socket,
                                                const QByteArray &method,
                                                const QUrl &requestUrl,
                                                const QList<QByteArray> &headerLines)
{
    if (requestUrl.path() != "/media")
        return false;

    if (method != "GET" && method != "HEAD") {
        socket->write(
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        return true;
    }

    const QUrlQuery query(requestUrl);
    const QString source = query.queryItemValue("source", QUrl::FullyDecoded);
    const QString localPath = localFilePathFromSource(source);
    if (localPath.isEmpty()) {
        socket->write(
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        return true;
    }

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        socket->write(
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        return true;
    }

    const qint64 fileSize = file.size();
    qint64 rangeStart = 0;
    qint64 rangeEnd = qMax<qint64>(0, fileSize - 1);
    bool partial = false;

    QByteArray rangeHeader;
    for (const QByteArray &rawLine : headerLines) {
        const QByteArray line = rawLine.trimmed();
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;

        const QByteArray key = line.left(colon).trimmed().toLower();
        if (key == "range") {
            rangeHeader = line.mid(colon + 1).trimmed();
            break;
        }
    }

    if (!rangeHeader.isEmpty() && rangeHeader.startsWith("bytes=")) {
        const QByteArray spec = rangeHeader.mid(6);
        const QList<QByteArray> parts = spec.split('-');
        bool startOk = false;
        bool endOk = false;

        if (!parts.value(0).isEmpty())
            rangeStart = parts.value(0).toLongLong(&startOk);
        else
            startOk = true;

        if (!parts.value(1).isEmpty())
            rangeEnd = parts.value(1).toLongLong(&endOk);
        else
            endOk = true;

        if (parts.value(0).isEmpty() && endOk) {
            const qint64 suffixLength = rangeEnd;
            rangeStart = qMax<qint64>(0, fileSize - suffixLength);
            rangeEnd = fileSize - 1;
        }

        if (!startOk || !endOk || rangeStart < 0 || rangeStart >= fileSize || rangeEnd < rangeStart) {
            socket->write(
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Range: bytes */" + QByteArray::number(fileSize) + "\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n");
            return true;
        }

        rangeEnd = qMin(rangeEnd, fileSize - 1);
        partial = true;
    }

    const qint64 bytesToSend = rangeEnd - rangeStart + 1;
    const QString mimeType = QMimeDatabase().mimeTypeForFile(localPath).name();
    QByteArray headers = partial ? "HTTP/1.1 206 Partial Content\r\n"
                                 : "HTTP/1.1 200 OK\r\n";
    headers += "Content-Type: " + mimeType.toUtf8() + "\r\n";
    headers += "Accept-Ranges: bytes\r\n";
    headers += "Content-Length: " + QByteArray::number(bytesToSend) + "\r\n";
    headers += "Cache-Control: no-store\r\n";
    if (partial) {
        headers += "Content-Range: bytes " + QByteArray::number(rangeStart) + "-" +
                   QByteArray::number(rangeEnd) + "/" + QByteArray::number(fileSize) + "\r\n";
    }
    headers += "Connection: close\r\n\r\n";
    socket->write(headers);

    if (method == "HEAD")
        return true;

    file.seek(rangeStart);
    qint64 remaining = bytesToSend;
    while (remaining > 0) {
        const QByteArray chunk = file.read(qMin(LocalMediaChunkSize, remaining));
        if (chunk.isEmpty())
            break;

        socket->write(chunk);
        remaining -= chunk.size();
    }

    return true;
}

void StudioPlayerDock::handlePlayerHttpConnections()
{
    while (m_playerServer->hasPendingConnections()) {
        QTcpSocket *socket = m_playerServer->nextPendingConnection();
        if (!socket)
            continue;

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            QByteArray buffer = socket->property("wt_request_buffer").toByteArray();
            buffer += socket->readAll();
            socket->setProperty("wt_request_buffer", buffer);

            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;

            const int lineEnd = buffer.indexOf("\r\n");
            const QByteArray requestLine = lineEnd >= 0 ? buffer.left(lineEnd) : QByteArray();
            const QList<QByteArray> requestParts = requestLine.split(' ');
            const QByteArray method = requestParts.size() >= 1 ? requestParts.at(0) : QByteArray("GET");
            const QByteArray rawTarget = requestParts.size() >= 2 ? requestParts.at(1) : QByteArray("/");
            const QUrl requestUrl = QUrl::fromEncoded(rawTarget);
            const QByteArray path = requestUrl.path(QUrl::FullyEncoded).isEmpty()
                                        ? QByteArray("/")
                                        : requestUrl.path(QUrl::FullyEncoded).toUtf8();
            const bool isHead = (method == "HEAD");
            QByteArray response;

            if (method == "POST" && path == "/player-event") {
                int contentLength = 0;
                const QList<QByteArray> headerLines = buffer.left(headerEnd).split('\n');
                for (int i = 1; i < headerLines.size(); ++i) {
                    const QByteArray line = headerLines.at(i).trimmed();
                    const int colon = line.indexOf(':');
                    if (colon < 0)
                        continue;

                    const QByteArray key = line.left(colon).trimmed().toLower();
                    const QByteArray value = line.mid(colon + 1).trimmed();
                    if (key == "content-length")
                        contentLength = value.toInt();
                }

                const int totalNeeded = headerEnd + 4 + contentLength;
                if (buffer.size() < totalNeeded)
                    return;

                const QByteArray body = buffer.mid(headerEnd + 4, contentLength);
                QJsonParseError error{};
                const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
                if (error.error == QJsonParseError::NoError && doc.isObject()) {
                    handlePlayerEvent(doc.object());
                    response =
                        "HTTP/1.1 204 No Content\r\n"
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n\r\n";
                } else {
                    static const QByteArray badBody = "Bad Request";
                    response =
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: text/plain; charset=UTF-8\r\n"
                        "Cache-Control: no-store\r\n"
                        "Content-Length: " + QByteArray::number(badBody.size()) + "\r\n"
                        "Connection: close\r\n\r\n" + badBody;
                }
                socket->write(response);
                socket->disconnectFromHost();
                return;
            }

            const QList<QByteArray> headerLines = buffer.left(headerEnd).split('\n');
            if (writeLocalMediaResponse(socket, method, requestUrl, headerLines)) {
                socket->disconnectFromHost();
                return;
            }

            if (path == "/" || path == "/player.html") {
                const QByteArray body = isHead ? QByteArray() : m_playerHtml;
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html; charset=UTF-8\r\n"
                    "Cache-Control: no-store\r\n"
                    "Referrer-Policy: strict-origin-when-cross-origin\r\n"
                    "Content-Length: " + QByteArray::number(m_playerHtml.size()) + "\r\n"
                    "Connection: close\r\n\r\n";

                if (!isHead)
                    response += body;
            } else {
                response =
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/plain; charset=UTF-8\r\n"
                    "Cache-Control: no-store\r\n"
                    "Content-Length: 9\r\n"
                    "Connection: close\r\n\r\n";
                if (!isHead)
                    response += "Not Found";
            }

            socket->write(response);
            socket->disconnectFromHost();
        });

        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

QByteArray StudioPlayerDock::loadModuleHtml(const char *filename) const
{
    const char *rawPath = obs_module_file(filename);
    if (!rawPath)
        return {};

    const QString path = QString::fromUtf8(rawPath);
    bfree((void *)rawPath);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    return file.readAll();
}

void StudioPlayerDock::dispatchPlayerCommand(QJsonObject payload, int attempts, int delayMs)
{
    if (attempts <= 0)
        return;

    if (!payload.contains("commandId"))
        payload.insert("commandId", QString::number(m_nextPlayerCommandId++));

    dispatchBrowserEvent(m_browserSource, "wt-command", payload);

    if (attempts == 1)
        return;

    QTimer::singleShot(delayMs, this, [this, payload, attempts, delayMs]() {
        dispatchPlayerCommand(payload, attempts - 1, delayMs);
    });
}

void StudioPlayerDock::ensureBrowserSource()
{
    if (!ensurePlayerHttpServer())
        return;

    const QString playerUrl = QString("http://127.0.0.1:%1/player.html").arg(m_playerServerPort);

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "url", playerUrl.toUtf8().constData());
    obs_data_set_int(settings, "width", 1920);
    obs_data_set_int(settings, "height", 1080);
    obs_data_set_bool(settings, "shutdown", false);
    obs_data_set_bool(settings, "restart_when_active", false);
    obs_data_set_bool(settings, "reroute_audio", true);

    if (!m_browserSource) {
        m_browserSource = obs_get_source_by_name(SOURCE_NAME);
        if (!m_browserSource) {
            obs_source_t *legacySource = obs_get_source_by_name(LEGACY_SOURCE_NAME);
            if (legacySource) {
                obs_source_set_name(legacySource, SOURCE_NAME);
                m_browserSource = legacySource;
            }
        }
    }

    if (m_browserSource) {
        obs_source_update(m_browserSource, settings);
    } else {
        m_browserSource = obs_source_create("browser_source", SOURCE_NAME, settings, nullptr);

        obs_source_t *sceneSource = obs_frontend_get_current_scene();
        if (sceneSource) {
            obs_scene_t *scene = obs_scene_from_source(sceneSource);
            if (scene)
                obs_scene_add(scene, m_browserSource);
            obs_source_release(sceneSource);
        }
    }

    obs_data_release(settings);
    updateButtons();
}

void StudioPlayerDock::setSourceVisible(bool visible)
{
    if (!m_browserSource) {
        setStatus(tr("Load a video first"), true);
        return;
    }

    struct Params {
        obs_source_t *target = nullptr;
        bool visible = false;
        bool found = false;
    } params;
    params.target = m_browserSource;
    params.visible = visible;

    obs_source_t *sceneSource = obs_frontend_get_current_scene();
    if (!sceneSource) {
        setStatus(tr("No active scene"), true);
        return;
    }

    obs_scene_t *scene = obs_scene_from_source(sceneSource);
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *data) -> bool {
            auto *params = static_cast<Params *>(data);
            if (obs_sceneitem_get_source(item) == params->target) {
                obs_sceneitem_set_visible(item, params->visible);
                params->found = true;
                return false;
            }
            return true;
        },
        &params);

    obs_source_release(sceneSource);

    if (!params.found) {
        setStatus(tr("The browser source is not in the current scene"), true);
        return;
    }

    setStatus(visible ? tr("Showing on screen") : tr("Hidden from screen"));
}

void StudioPlayerDock::dispatchBrowserEvent(obs_source_t *source, const QString &eventName, const QJsonObject &payload)
{
    if (!source)
        return;

    proc_handler_t *handler = obs_source_get_proc_handler(source);
    if (!handler)
        return;

    calldata_t data = {};
    calldata_init(&data);
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    calldata_set_string(&data, "eventName", eventName.toUtf8().constData());
    calldata_set_string(&data, "jsonString", json.constData());
    proc_handler_call(handler, "javascript_event", &data);
    calldata_free(&data);
}

void StudioPlayerDock::onClockTick()
{
    if (m_mediaSource.isEmpty())
        return;

    if (!m_currentSourceControllable) {
        m_timeLabel->setText(tr("Embed"));
        return;
    }

    m_timeLabel->setText(formatTime(currentTime()));
}

double StudioPlayerDock::currentTime() const
{
    if (!m_isPlaying)
        return m_savedTimestamp;

    const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    return m_savedTimestamp + (now - m_playStartedAt);
}

QString StudioPlayerDock::formatTime(double seconds) const
{
    const int total = qMax(0, static_cast<int>(seconds));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;

    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }

    return QString("%1:%2").arg(minutes).arg(secs, 2, 10, QChar('0'));
}

QString StudioPlayerDock::formatDurationText(double seconds) const
{
    return formatTime(seconds);
}

QString StudioPlayerDock::normalizeMediaInput(const QString &input) const
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return {};

    const QString localPath = localFilePathFromSource(trimmed);
    if (!localPath.isEmpty())
        return QUrl::fromLocalFile(localPath).toString(QUrl::FullyEncoded);

    const QString youtubeId = extractYouTubeId(trimmed);
    if (!youtubeId.isEmpty())
        return QString("https://www.youtube.com/watch?v=%1").arg(youtubeId);

    const QUrl url = QUrl::fromUserInput(trimmed);
    if (!url.isValid())
        return {};

    const QString scheme = url.scheme().toLower();
    if (scheme != "http" && scheme != "https")
        return {};

    if (url.host().trimmed().isEmpty())
        return {};

    return url.toString(QUrl::FullyEncoded);
}

bool StudioPlayerDock::isLikelyControllableSource(const QString &source) const
{
    if (!extractTwitchClipId(source).isEmpty())
        return false;
    return !extractYouTubeId(source).isEmpty() ||
           !localFilePathFromSource(source).isEmpty() ||
           isDirectMediaUrl(source) ||
           !extractTwitchVodId(source).isEmpty();
}

void StudioPlayerDock::setStatus(const QString &msg, bool error)
{
    m_statusBar->setText(msg);
    m_statusBar->setStyleSheet(error
                                   ? "color: #ef4444; font-size: 10px;"
                                   : "color: gray; font-size: 10px;");
}

void StudioPlayerDock::updateButtons()
{
    const bool hasMedia = !m_mediaSource.isEmpty();
    const bool hasQueueItems = m_queueList && m_queueList->count() > 0;
    const bool hasQueueSelection = m_queueList && m_queueList->currentRow() >= 0;
    const bool controllable = hasMedia && m_currentSourceControllable;

    m_playBtn->setEnabled(controllable && !m_isPlaying);
    m_pauseBtn->setEnabled(controllable && m_isPlaying);
    m_stopBtn->setEnabled(hasMedia);
    m_volumeSlider->setEnabled(controllable);
    m_openPlayerBtn->setEnabled(hasMedia);
    m_showBtn->setEnabled(m_browserSource != nullptr);
    m_hideBtn->setEnabled(m_browserSource != nullptr);
    m_queuePlayBtn->setEnabled(hasQueueSelection);
    m_queueRemoveBtn->setEnabled(hasQueueSelection);
    m_queueClearBtn->setEnabled(hasQueueItems);
    m_exportQueueBtn->setEnabled(hasQueueItems);
}

// ── Queue persistence ────────────────────────────────────────────────────────

QString StudioPlayerDock::queueConfigPath() const
{
    char *rawPath = obs_module_get_config_path(obs_current_module(), "queue.json");
    if (!rawPath)
        return {};
    const QString path = QString::fromUtf8(rawPath);
    bfree(rawPath);
    return path;
}

QString StudioPlayerDock::playlistsDirPath() const
{
    char *rawPath = obs_module_get_config_path(obs_current_module(), "playlists");
    if (!rawPath)
        return {};
    const QString path = QString::fromUtf8(rawPath);
    bfree(rawPath);
    return path;
}

void StudioPlayerDock::saveQueueToFile(const QString &path)
{
    if (path.isEmpty() || !m_queueList)
        return;

    QJsonArray arr;
    for (int i = 0; i < m_queueList->count(); ++i) {
        const QListWidgetItem *item = m_queueList->item(i);
        QJsonObject obj;
        obj["source"]   = item->data(QueueSourceRole).toString();
        obj["rawInput"] = item->data(QueueRawInputRole).toString();
        obj["title"]    = item->data(QueueTitleRole).toString();
        obj["subtitle"] = item->data(QueueSubtitleRole).toString();
        obj["duration"] = item->data(QueueDurationRole).toString();
        arr.append(obj);
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

bool StudioPlayerDock::loadQueueFromFile(const QString &path)
{
    if (path.isEmpty() || !m_queueList)
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray())
        return false;

    for (const QJsonValue &val : doc.array()) {
        if (!val.isObject())
            continue;
        const QJsonObject obj = val.toObject();
        const QString source   = obj["source"].toString();
        const QString rawInput = obj["rawInput"].toString();
        if (source.isEmpty())
            continue;

        addQueueItem(source, rawInput.isEmpty() ? source : rawInput);

        const QString itemId = m_queueList->item(m_queueList->count() - 1)
                                   ->data(QueueIdRole).toString();
        updateQueueItemMetadata(itemId,
                                obj["title"].toString(),
                                obj["subtitle"].toString(),
                                obj["duration"].toString());
    }

    refreshQueueLabels();
    return true;
}

void StudioPlayerDock::saveQueue()
{
    saveQueueToFile(queueConfigPath());
}

void StudioPlayerDock::loadQueue()
{
    loadQueueFromFile(queueConfigPath());
    updateButtons();
}

// ── Playlist management ──────────────────────────────────────────────────────

void StudioPlayerDock::refreshPlaylistCombo()
{
    if (!m_playlistCombo)
        return;

    const QString dir = playlistsDirPath();
    const QSignalBlocker blocker(m_playlistCombo);
    m_playlistCombo->clear();

    if (!dir.isEmpty()) {
        const QStringList files = QDir(dir).entryList({"*.json"}, QDir::Files, QDir::Name);
        for (const QString &f : files)
            m_playlistCombo->addItem(QFileInfo(f).baseName());
    }

    const bool hasItems = m_playlistCombo->count() > 0;
    if (m_loadPlaylistBtn) m_loadPlaylistBtn->setEnabled(hasItems);
    if (m_deletePlaylistBtn) m_deletePlaylistBtn->setEnabled(hasItems);
}

void StudioPlayerDock::onSavePlaylistClicked()
{
    bool ok;
    const QString name = QInputDialog::getText(this, tr("Save Playlist"),
        tr("Playlist name:"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    QString safeName = name;
    safeName.replace(QRegularExpression(R"([/\\:*?"<>|])"), "_");

    const QString dir = playlistsDirPath();
    if (dir.isEmpty()) {
        setStatus(tr("Could not determine config path"), true);
        return;
    }

    QDir().mkpath(dir);
    saveQueueToFile(dir + "/" + safeName + ".json");
    setStatus(tr("Saved playlist \"%1\"").arg(safeName));

    refreshPlaylistCombo();
    const int idx = m_playlistCombo->findText(safeName);
    if (idx >= 0)
        m_playlistCombo->setCurrentIndex(idx);
}

void StudioPlayerDock::onLoadPlaylistClicked()
{
    if (!m_playlistCombo || m_playlistCombo->currentText().isEmpty())
        return;

    const QString name = m_playlistCombo->currentText();
    const QString dir  = playlistsDirPath();
    if (dir.isEmpty())
        return;

    m_queuePlayingId.clear();
    m_queueList->clear();
    if (!loadQueueFromFile(dir + "/" + name + ".json")) {
        setStatus(tr("Failed to load playlist \"%1\"").arg(name), true);
    } else {
        saveQueue();
        setStatus(tr("Loaded playlist \"%1\"").arg(name));
    }
    updateButtons();
}

void StudioPlayerDock::onDeletePlaylistClicked()
{
    if (!m_playlistCombo || m_playlistCombo->currentText().isEmpty())
        return;

    const QString name = m_playlistCombo->currentText();
    const QString dir  = playlistsDirPath();
    if (dir.isEmpty())
        return;

    QFile::remove(dir + "/" + name + ".json");
    setStatus(tr("Deleted playlist \"%1\"").arg(name));
    refreshPlaylistCombo();
}

// ── Import / Export queue ────────────────────────────────────────────────────

void StudioPlayerDock::onImportQueueClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Queue"), QString(),
        tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStatus(tr("Failed to open file for import"), true);
        return;
    }

    const QStringList lines = QString::fromUtf8(file.readAll())
        .split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);
    int added = 0;
    for (const QString &line : lines)
        added += enqueueVideosFromText(line.trimmed());

    if (added > 0) {
        saveQueue();
        setStatus(tr("Imported %1 item(s) from file").arg(added));
    } else {
        setStatus(tr("No valid items found in file"), true);
    }
    updateButtons();
}

void StudioPlayerDock::onExportQueueClicked()
{
    if (!m_queueList || m_queueList->count() == 0) {
        setStatus(tr("Queue is empty"), true);
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Queue"), QString(),
        tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        setStatus(tr("Failed to open file for export"), true);
        return;
    }

    for (int i = 0; i < m_queueList->count(); ++i) {
        const QString raw = m_queueList->item(i)->data(QueueRawInputRole).toString();
        file.write((raw.isEmpty()
                        ? m_queueList->item(i)->data(QueueSourceRole).toString()
                        : raw).toUtf8() + "\n");
    }

    setStatus(tr("Exported %1 item(s) to file").arg(m_queueList->count()));
}

// ── Hotkey slots ─────────────────────────────────────────────────────────────

void StudioPlayerDock::onNextHotkey()
{
    if (!playNextQueuedVideo(false))
        setStatus(tr("Queue is empty"));
}

void StudioPlayerDock::onVolumeUpHotkey()
{
    setVolumePercent(m_volumePercent + 5, true);
}

void StudioPlayerDock::onVolumeDownHotkey()
{
    setVolumePercent(m_volumePercent - 5, true);
}
