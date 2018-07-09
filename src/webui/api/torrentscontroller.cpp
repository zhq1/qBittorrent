/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2018  Vladimir Golovnev <glassez@yandex.ru>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "torrentscontroller.h"

#include <functional>

#include <QBitArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QRegularExpression>
#include <QUrl>

#include "base/bittorrent/session.h"
#include "base/bittorrent/torrenthandle.h"
#include "base/bittorrent/torrentinfo.h"
#include "base/bittorrent/trackerentry.h"
#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/torrentfilter.h"
#include "base/utils/fs.h"
#include "base/utils/string.h"
#include "apierror.h"
#include "serialize/serialize_torrent.h"

// Tracker keys
const char KEY_TRACKER_URL[] = "url";
const char KEY_TRACKER_STATUS[] = "status";
const char KEY_TRACKER_MSG[] = "msg";
const char KEY_TRACKER_PEERS[] = "num_peers";

// Web seed keys
const char KEY_WEBSEED_URL[] = "url";

// Torrent keys (Properties)
const char KEY_PROP_TIME_ELAPSED[] = "time_elapsed";
const char KEY_PROP_SEEDING_TIME[] = "seeding_time";
const char KEY_PROP_ETA[] = "eta";
const char KEY_PROP_CONNECT_COUNT[] = "nb_connections";
const char KEY_PROP_CONNECT_COUNT_LIMIT[] = "nb_connections_limit";
const char KEY_PROP_DOWNLOADED[] = "total_downloaded";
const char KEY_PROP_DOWNLOADED_SESSION[] = "total_downloaded_session";
const char KEY_PROP_UPLOADED[] = "total_uploaded";
const char KEY_PROP_UPLOADED_SESSION[] = "total_uploaded_session";
const char KEY_PROP_DL_SPEED[] = "dl_speed";
const char KEY_PROP_DL_SPEED_AVG[] = "dl_speed_avg";
const char KEY_PROP_UP_SPEED[] = "up_speed";
const char KEY_PROP_UP_SPEED_AVG[] = "up_speed_avg";
const char KEY_PROP_DL_LIMIT[] = "dl_limit";
const char KEY_PROP_UP_LIMIT[] = "up_limit";
const char KEY_PROP_WASTED[] = "total_wasted";
const char KEY_PROP_SEEDS[] = "seeds";
const char KEY_PROP_SEEDS_TOTAL[] = "seeds_total";
const char KEY_PROP_PEERS[] = "peers";
const char KEY_PROP_PEERS_TOTAL[] = "peers_total";
const char KEY_PROP_RATIO[] = "share_ratio";
const char KEY_PROP_REANNOUNCE[] = "reannounce";
const char KEY_PROP_TOTAL_SIZE[] = "total_size";
const char KEY_PROP_PIECES_NUM[] = "pieces_num";
const char KEY_PROP_PIECE_SIZE[] = "piece_size";
const char KEY_PROP_PIECES_HAVE[] = "pieces_have";
const char KEY_PROP_CREATED_BY[] = "created_by";
const char KEY_PROP_LAST_SEEN[] = "last_seen";
const char KEY_PROP_ADDITION_DATE[] = "addition_date";
const char KEY_PROP_COMPLETION_DATE[] = "completion_date";
const char KEY_PROP_CREATION_DATE[] = "creation_date";
const char KEY_PROP_SAVE_PATH[] = "save_path";
const char KEY_PROP_COMMENT[] = "comment";

// File keys
const char KEY_FILE_NAME[] = "name";
const char KEY_FILE_SIZE[] = "size";
const char KEY_FILE_PROGRESS[] = "progress";
const char KEY_FILE_PRIORITY[] = "priority";
const char KEY_FILE_IS_SEED[] = "is_seed";
const char KEY_FILE_PIECE_RANGE[] = "piece_range";
const char KEY_FILE_AVAILABILITY[] = "availability";

namespace
{
    using Utils::String::parseBool;
    using Utils::String::parseTriStateBool;

    void applyToTorrents(const QStringList &hashes, const std::function<void (BitTorrent::TorrentHandle *torrent)> &func)
    {
        if ((hashes.size() == 1) && (hashes[0] == QLatin1String("all"))) {
            foreach (BitTorrent::TorrentHandle *torrent, BitTorrent::Session::instance()->torrents())
                func(torrent);
        }
        else {
            for (const QString &hash : hashes) {
                BitTorrent::TorrentHandle *torrent = BitTorrent::Session::instance()->findTorrent(hash);
                if (torrent)
                    func(torrent);
            }
        }
    }
}

// Returns all the torrents in JSON format.
// The return value is a JSON-formatted list of dictionaries.
// The dictionary keys are:
//   - "hash": Torrent hash
//   - "name": Torrent name
//   - "size": Torrent size
//   - "progress": Torrent progress
//   - "dlspeed": Torrent download speed
//   - "upspeed": Torrent upload speed
//   - "priority": Torrent priority (-1 if queuing is disabled)
//   - "num_seeds": Torrent seeds connected to
//   - "num_complete": Torrent seeds in the swarm
//   - "num_leechs": Torrent leechers connected to
//   - "num_incomplete": Torrent leechers in the swarm
//   - "ratio": Torrent share ratio
//   - "eta": Torrent ETA
//   - "state": Torrent state
//   - "seq_dl": Torrent sequential download state
//   - "f_l_piece_prio": Torrent first last piece priority state
//   - "force_start": Torrent force start state
//   - "category": Torrent category
// GET params:
//   - filter (string): all, downloading, seeding, completed, paused, resumed, active, inactive
//   - category (string): torrent category for filtering by it (empty string means "uncategorized"; no "category" param presented means "any category")
//   - hashes (string): filter by hashes, can contain multiple hashes separated by |
//   - sort (string): name of column for sorting by its value
//   - reverse (bool): enable reverse sorting
//   - limit (int): set limit number of torrents returned (if greater than 0, otherwise - unlimited)
//   - offset (int): set offset (if less than 0 - offset from end)
void TorrentsController::infoAction()
{
    const QString filter {params()["filter"]};
    const QString category {params()["category"]};
    const QString sortedColumn {params()["sort"]};
    const bool reverse {parseBool(params()["reverse"], false)};
    int limit {params()["limit"].toInt()};
    int offset {params()["offset"].toInt()};
    const QStringSet hashSet {params()["hashes"].split('|', QString::SkipEmptyParts).toSet()};

    QVariantList torrentList;
    TorrentFilter torrentFilter(filter, (hashSet.isEmpty() ? TorrentFilter::AnyHash : hashSet), category);
    foreach (BitTorrent::TorrentHandle *const torrent, BitTorrent::Session::instance()->torrents()) {
        if (torrentFilter.match(torrent))
            torrentList.append(serialize(*torrent));
    }

    std::sort(torrentList.begin(), torrentList.end()
              , [sortedColumn, reverse](const QVariant &torrent1, const QVariant &torrent2)
    {
        return reverse
                ? (torrent1.toMap().value(sortedColumn) > torrent2.toMap().value(sortedColumn))
                : (torrent1.toMap().value(sortedColumn) < torrent2.toMap().value(sortedColumn));
    });

    const int size = torrentList.size();
    // normalize offset
    if (offset < 0)
        offset = size + offset;
    if ((offset >= size) || (offset < 0))
        offset = 0;
    // normalize limit
    if (limit <= 0)
        limit = -1; // unlimited

    if ((limit > 0) || (offset > 0))
        torrentList = torrentList.mid(offset, limit);

    setResult(QJsonArray::fromVariantList(torrentList));
}

// Returns the properties for a torrent in JSON format.
// The return value is a JSON-formatted dictionary.
// The dictionary keys are:
//   - "time_elapsed": Torrent elapsed time
//   - "seeding_time": Torrent elapsed time while complete
//   - "eta": Torrent ETA
//   - "nb_connections": Torrent connection count
//   - "nb_connections_limit": Torrent connection count limit
//   - "total_downloaded": Total data uploaded for torrent
//   - "total_downloaded_session": Total data downloaded this session
//   - "total_uploaded": Total data uploaded for torrent
//   - "total_uploaded_session": Total data uploaded this session
//   - "dl_speed": Torrent download speed
//   - "dl_speed_avg": Torrent average download speed
//   - "up_speed": Torrent upload speed
//   - "up_speed_avg": Torrent average upload speed
//   - "dl_limit": Torrent download limit
//   - "up_limit": Torrent upload limit
//   - "total_wasted": Total data wasted for torrent
//   - "seeds": Torrent connected seeds
//   - "seeds_total": Torrent total number of seeds
//   - "peers": Torrent connected peers
//   - "peers_total": Torrent total number of peers
//   - "share_ratio": Torrent share ratio
//   - "reannounce": Torrent next reannounce time
//   - "total_size": Torrent total size
//   - "pieces_num": Torrent pieces count
//   - "piece_size": Torrent piece size
//   - "pieces_have": Torrent pieces have
//   - "created_by": Torrent creator
//   - "last_seen": Torrent last seen complete
//   - "addition_date": Torrent addition date
//   - "completion_date": Torrent completion date
//   - "creation_date": Torrent creation date
//   - "save_path": Torrent save path
//   - "comment": Torrent comment
void TorrentsController::propertiesAction()
{
    checkParams({"hash"});

    const QString hash {params()["hash"]};
    QVariantMap dataDict;
    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    dataDict[KEY_PROP_TIME_ELAPSED] = torrent->activeTime();
    dataDict[KEY_PROP_SEEDING_TIME] = torrent->seedingTime();
    dataDict[KEY_PROP_ETA] = torrent->eta();
    dataDict[KEY_PROP_CONNECT_COUNT] = torrent->connectionsCount();
    dataDict[KEY_PROP_CONNECT_COUNT_LIMIT] = torrent->connectionsLimit();
    dataDict[KEY_PROP_DOWNLOADED] = torrent->totalDownload();
    dataDict[KEY_PROP_DOWNLOADED_SESSION] = torrent->totalPayloadDownload();
    dataDict[KEY_PROP_UPLOADED] = torrent->totalUpload();
    dataDict[KEY_PROP_UPLOADED_SESSION] = torrent->totalPayloadUpload();
    dataDict[KEY_PROP_DL_SPEED] = torrent->downloadPayloadRate();
    dataDict[KEY_PROP_DL_SPEED_AVG] = torrent->totalDownload() / (1 + torrent->activeTime() - torrent->finishedTime());
    dataDict[KEY_PROP_UP_SPEED] = torrent->uploadPayloadRate();
    dataDict[KEY_PROP_UP_SPEED_AVG] = torrent->totalUpload() / (1 + torrent->activeTime());
    dataDict[KEY_PROP_DL_LIMIT] = torrent->downloadLimit() <= 0 ? -1 : torrent->downloadLimit();
    dataDict[KEY_PROP_UP_LIMIT] = torrent->uploadLimit() <= 0 ? -1 : torrent->uploadLimit();
    dataDict[KEY_PROP_WASTED] = torrent->wastedSize();
    dataDict[KEY_PROP_SEEDS] = torrent->seedsCount();
    dataDict[KEY_PROP_SEEDS_TOTAL] = torrent->totalSeedsCount();
    dataDict[KEY_PROP_PEERS] = torrent->leechsCount();
    dataDict[KEY_PROP_PEERS_TOTAL] = torrent->totalLeechersCount();
    const qreal ratio = torrent->realRatio();
    dataDict[KEY_PROP_RATIO] = ratio > BitTorrent::TorrentHandle::MAX_RATIO ? -1 : ratio;
    dataDict[KEY_PROP_REANNOUNCE] = torrent->nextAnnounce();
    dataDict[KEY_PROP_TOTAL_SIZE] = torrent->totalSize();
    dataDict[KEY_PROP_PIECES_NUM] = torrent->piecesCount();
    dataDict[KEY_PROP_PIECE_SIZE] = torrent->pieceLength();
    dataDict[KEY_PROP_PIECES_HAVE] = torrent->piecesHave();
    dataDict[KEY_PROP_CREATED_BY] = torrent->creator();
    dataDict[KEY_PROP_ADDITION_DATE] = torrent->addedTime().toTime_t();
    if (torrent->hasMetadata()) {
        dataDict[KEY_PROP_LAST_SEEN] = torrent->lastSeenComplete().isValid() ? static_cast<int>(torrent->lastSeenComplete().toTime_t()) : -1;
        dataDict[KEY_PROP_COMPLETION_DATE] = torrent->completedTime().isValid() ? static_cast<int>(torrent->completedTime().toTime_t()) : -1;
        dataDict[KEY_PROP_CREATION_DATE] = torrent->creationDate().toTime_t();
    }
    else {
        dataDict[KEY_PROP_LAST_SEEN] = -1;
        dataDict[KEY_PROP_COMPLETION_DATE] = -1;
        dataDict[KEY_PROP_CREATION_DATE] = -1;
    }
    dataDict[KEY_PROP_SAVE_PATH] = Utils::Fs::toNativePath(torrent->savePath());
    dataDict[KEY_PROP_COMMENT] = torrent->comment();

    setResult(QJsonObject::fromVariantMap(dataDict));
}

// Returns the trackers for a torrent in JSON format.
// The return value is a JSON-formatted list of dictionaries.
// The dictionary keys are:
//   - "url": Tracker URL
//   - "status": Tracker status
//   - "num_peers": Tracker peer count
//   - "msg": Tracker message (last)
void TorrentsController::trackersAction()
{
    checkParams({"hash"});

    const QString hash {params()["hash"]};
    QVariantList trackerList;
    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    QHash<QString, BitTorrent::TrackerInfo> trackersData = torrent->trackerInfos();
    foreach (const BitTorrent::TrackerEntry &tracker, torrent->trackers()) {
        QVariantMap trackerDict;
        trackerDict[KEY_TRACKER_URL] = tracker.url();
        const BitTorrent::TrackerInfo data = trackersData.value(tracker.url());
        QString status;
        switch (tracker.status()) {
        case BitTorrent::TrackerEntry::NotContacted:
            status = tr("Not contacted yet"); break;
        case BitTorrent::TrackerEntry::Updating:
            status = tr("Updating..."); break;
        case BitTorrent::TrackerEntry::Working:
            status = tr("Working"); break;
        case BitTorrent::TrackerEntry::NotWorking:
            status = tr("Not working"); break;
        }
        trackerDict[KEY_TRACKER_STATUS] = status;
        trackerDict[KEY_TRACKER_PEERS] = data.numPeers;
        trackerDict[KEY_TRACKER_MSG] = data.lastMessage.trimmed();

        trackerList.append(trackerDict);
    }

    setResult(QJsonArray::fromVariantList(trackerList));
}

// Returns the web seeds for a torrent in JSON format.
// The return value is a JSON-formatted list of dictionaries.
// The dictionary keys are:
//   - "url": Web seed URL
void TorrentsController::webseedsAction()
{
    checkParams({"hash"});

    const QString hash {params()["hash"]};
    QVariantList webSeedList;
    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    foreach (const QUrl &webseed, torrent->urlSeeds()) {
        QVariantMap webSeedDict;
        webSeedDict[KEY_WEBSEED_URL] = webseed.toString();
        webSeedList.append(webSeedDict);
    }

    setResult(QJsonArray::fromVariantList(webSeedList));
}

// Returns the files in a torrent in JSON format.
// The return value is a JSON-formatted list of dictionaries.
// The dictionary keys are:
//   - "name": File name
//   - "size": File size
//   - "progress": File progress
//   - "priority": File priority
//   - "is_seed": Flag indicating if torrent is seeding/complete
//   - "piece_range": Piece index range, the first number is the starting piece index
//        and the second number is the ending piece index (inclusive)
void TorrentsController::filesAction()
{
    checkParams({"hash"});

    const QString hash {params()["hash"]};
    QVariantList fileList;
    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    if (torrent->hasMetadata()) {
        const QVector<int> priorities = torrent->filePriorities();
        const QVector<qreal> fp = torrent->filesProgress();
        const QVector<qreal> fileAvailability = torrent->availableFileFractions();
        const BitTorrent::TorrentInfo info = torrent->info();
        for (int i = 0; i < torrent->filesCount(); ++i) {
            QVariantMap fileDict;
            fileDict[KEY_FILE_PROGRESS] = fp[i];
            fileDict[KEY_FILE_PRIORITY] = priorities[i];
            fileDict[KEY_FILE_SIZE] = torrent->fileSize(i);
            fileDict[KEY_FILE_AVAILABILITY] = fileAvailability[i];

            QString fileName = torrent->filePath(i);
            if (fileName.endsWith(QB_EXT, Qt::CaseInsensitive))
                fileName.chop(QB_EXT.size());
            fileDict[KEY_FILE_NAME] = Utils::Fs::toNativePath(fileName);

            const BitTorrent::TorrentInfo::PieceRange idx = info.filePieces(i);
            fileDict[KEY_FILE_PIECE_RANGE] = QVariantList {idx.first(), idx.last()};

            if (i == 0)
                fileDict[KEY_FILE_IS_SEED] = torrent->isSeed();

            fileList.append(fileDict);
        }
    }

    setResult(QJsonArray::fromVariantList(fileList));
}

// Returns an array of hashes (of each pieces respectively) for a torrent in JSON format.
// The return value is a JSON-formatted array of strings (hex strings).
void TorrentsController::pieceHashesAction()
{
    checkParams({"hash"});

    const QString hash {params()["hash"]};
    QVariantList pieceHashes;
    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    const QVector<QByteArray> hashes = torrent->info().pieceHashes();
    pieceHashes.reserve(hashes.size());
    foreach (const QByteArray &hash, hashes)
        pieceHashes.append(hash.toHex());

    setResult(QJsonArray::fromVariantList(pieceHashes));
}

// Returns an array of states (of each pieces respectively) for a torrent in JSON format.
// The return value is a JSON-formatted array of ints.
// 0: piece not downloaded
// 1: piece requested or downloading
// 2: piece already downloaded
void TorrentsController::pieceStatesAction()
{
    checkParams({"hash"});

    const QString hash {params()["hash"]};
    QVariantList pieceStates;
    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    const QBitArray states = torrent->pieces();
    pieceStates.reserve(states.size());
    for (int i = 0; i < states.size(); ++i)
        pieceStates.append(static_cast<int>(states[i]) * 2);

    const QBitArray dlstates = torrent->downloadingPieces();
    for (int i = 0; i < states.size(); ++i) {
        if (dlstates[i])
            pieceStates[i] = 1;
    }

    setResult(QJsonArray::fromVariantList(pieceStates));
}

void TorrentsController::addAction()
{
    const QString urls = params()["urls"];

    const bool skipChecking = parseBool(params()["skip_checking"], false);
    const bool seqDownload = parseBool(params()["sequentialDownload"], false);
    const bool firstLastPiece = parseBool(params()["firstLastPiecePrio"], false);
    const TriStateBool addPaused = parseTriStateBool(params()["paused"]);
    const TriStateBool rootFolder = parseTriStateBool(params()["root_folder"]);
    const QString savepath = params()["savepath"].trimmed();
    const QString category = params()["category"].trimmed();
    const QString cookie = params()["cookie"];
    const QString torrentName = params()["rename"].trimmed();
    const int upLimit = params()["upLimit"].toInt();
    const int dlLimit = params()["dlLimit"].toInt();

    QList<QNetworkCookie> cookies;
    if (!cookie.isEmpty()) {
        const QStringList cookiesStr = cookie.split("; ");
        for (QString cookieStr : cookiesStr) {
            cookieStr = cookieStr.trimmed();
            int index = cookieStr.indexOf('=');
            if (index > 1) {
                QByteArray name = cookieStr.left(index).toLatin1();
                QByteArray value = cookieStr.right(cookieStr.length() - index - 1).toLatin1();
                cookies += QNetworkCookie(name, value);
            }
        }
    }

    BitTorrent::AddTorrentParams params;
    // TODO: Check if destination actually exists
    params.skipChecking = skipChecking;
    params.sequential = seqDownload;
    params.firstLastPiecePriority = firstLastPiece;
    params.addPaused = addPaused;
    params.createSubfolder = rootFolder;
    params.savePath = savepath;
    params.category = category;
    params.name = torrentName;
    params.uploadLimit = (upLimit > 0) ? upLimit : -1;
    params.downloadLimit = (dlLimit > 0) ? dlLimit : -1;

    bool partialSuccess = false;
    for (QString url : copyAsConst(urls.split('\n'))) {
        url = url.trimmed();
        if (!url.isEmpty()) {
            Net::DownloadManager::instance()->setCookiesFromUrl(cookies, QUrl::fromEncoded(url.toUtf8()));
            partialSuccess |= BitTorrent::Session::instance()->addTorrent(url, params);
        }
    }

    for (auto it = data().constBegin(); it != data().constEnd(); ++it) {
        const BitTorrent::TorrentInfo torrentInfo = BitTorrent::TorrentInfo::load(it.value());
        if (!torrentInfo.isValid()) {
            throw APIError(APIErrorType::BadData
                           , tr("Error: '%1' is not a valid torrent file.").arg(it.key()));
        }

        partialSuccess |= BitTorrent::Session::instance()->addTorrent(torrentInfo, params);
    }

    if (partialSuccess)
        setResult("Ok.");
    else
        setResult("Fails.");
}

void TorrentsController::addTrackersAction()
{
    checkParams({"hash", "urls"});

    const QString hash = params()["hash"];

    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (torrent) {
        QList<BitTorrent::TrackerEntry> trackers;
        foreach (QString url, params()["urls"].split('\n')) {
            url = url.trimmed();
            if (!url.isEmpty())
                trackers << url;
        }
        torrent->addTrackers(trackers);
    }
}

void TorrentsController::pauseAction()
{
    checkParams({"hashes"});

    const QStringList hashes = params()["hashes"].split('|');
    applyToTorrents(hashes, [](BitTorrent::TorrentHandle *torrent) { torrent->pause(); });
}

void TorrentsController::resumeAction()
{
    checkParams({"hashes"});

    const QStringList hashes = params()["hashes"].split('|');
    applyToTorrents(hashes, [](BitTorrent::TorrentHandle *torrent) { torrent->resume(); });
}

void TorrentsController::filePrioAction()
{
    checkParams({"hash", "id", "priority"});

    const QString hash = params()["hash"];
    int fileID = params()["id"].toInt();
    int priority = params()["priority"].toInt();
    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);

    if (torrent && torrent->hasMetadata())
        torrent->setFilePriority(fileID, priority);
}

void TorrentsController::uploadLimitAction()
{
    checkParams({"hashes"});

    const QStringList hashes {params()["hashes"].split('|')};
    QVariantMap map;
    foreach (const QString &hash, hashes) {
        int limit = -1;
        BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
        if (torrent)
            limit = torrent->uploadLimit();
        map[hash] = limit;
    }

    setResult(QJsonObject::fromVariantMap(map));
}

void TorrentsController::downloadLimitAction()
{
    checkParams({"hashes"});

    const QStringList hashes {params()["hashes"].split('|')};
    QVariantMap map;
    foreach (const QString &hash, hashes) {
        int limit = -1;
        BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
        if (torrent)
            limit = torrent->downloadLimit();
        map[hash] = limit;
    }

    setResult(QJsonObject::fromVariantMap(map));
}

void TorrentsController::setUploadLimitAction()
{
    checkParams({"hashes", "limit"});

    qlonglong limit = params()["limit"].toLongLong();
    if (limit == 0)
        limit = -1;

    const QStringList hashes {params()["hashes"].split('|')};
    applyToTorrents(hashes, [limit](BitTorrent::TorrentHandle *torrent) { torrent->setUploadLimit(limit); });
}

void TorrentsController::setDownloadLimitAction()
{
    checkParams({"hashes", "limit"});

    qlonglong limit = params()["limit"].toLongLong();
    if (limit == 0)
        limit = -1;

    const QStringList hashes {params()["hashes"].split('|')};
    applyToTorrents(hashes, [limit](BitTorrent::TorrentHandle *torrent) { torrent->setDownloadLimit(limit); });
}

void TorrentsController::setShareLimitsAction()
{
    checkParams({"hashes", "ratioLimit", "seedingTimeLimit"});

    const qreal ratioLimit = params()["ratioLimit"].toDouble();
    const qlonglong seedingTimeLimit = params()["seedingTimeLimit"].toLongLong();
    const QStringList hashes = params()["hashes"].split('|');

    applyToTorrents(hashes, [ratioLimit, seedingTimeLimit](BitTorrent::TorrentHandle *torrent)
    {
        torrent->setRatioLimit(ratioLimit);
        torrent->setSeedingTimeLimit(seedingTimeLimit);
    });
}

void TorrentsController::toggleSequentialDownloadAction()
{
    checkParams({"hashes"});

    const QStringList hashes {params()["hashes"].split('|')};
    applyToTorrents(hashes, [](BitTorrent::TorrentHandle *torrent) { torrent->toggleSequentialDownload(); });
}

void TorrentsController::toggleFirstLastPiecePrioAction()
{
    checkParams({"hashes"});

    const QStringList hashes {params()["hashes"].split('|')};
    applyToTorrents(hashes, [](BitTorrent::TorrentHandle *torrent) { torrent->toggleFirstLastPiecePriority(); });
}

void TorrentsController::setSuperSeedingAction()
{
    checkParams({"hashes", "value"});

    const bool value {parseBool(params()["value"], false)};
    const QStringList hashes {params()["hashes"].split('|')};
    applyToTorrents(hashes, [value](BitTorrent::TorrentHandle *torrent) { torrent->setSuperSeeding(value); });
}

void TorrentsController::setForceStartAction()
{
    checkParams({"hashes", "value"});

    const bool value {parseBool(params()["value"], false)};
    const QStringList hashes {params()["hashes"].split('|')};
    applyToTorrents(hashes, [value](BitTorrent::TorrentHandle *torrent) { torrent->resume(value); });
}

void TorrentsController::deleteAction()
{
    checkParams({"hashes", "deleteFiles"});

    const QStringList hashes {params()["hashes"].split('|')};
    const bool deleteFiles {parseBool(params()["deleteFiles"], false)};
    applyToTorrents(hashes, [deleteFiles](BitTorrent::TorrentHandle *torrent)
    {
        BitTorrent::Session::instance()->deleteTorrent(torrent->hash(), deleteFiles);
    });
}

void TorrentsController::increasePrioAction()
{
    checkParams({"hashes"});

    if (!BitTorrent::Session::instance()->isQueueingSystemEnabled())
        throw APIError(APIErrorType::Conflict, tr("Torrent queueing must be enabled"));

    const QStringList hashes {params()["hashes"].split('|')};
    BitTorrent::Session::instance()->increaseTorrentsPriority(hashes);
}

void TorrentsController::decreasePrioAction()
{
    checkParams({"hashes"});

    if (!BitTorrent::Session::instance()->isQueueingSystemEnabled())
        throw APIError(APIErrorType::Conflict, tr("Torrent queueing must be enabled"));

    const QStringList hashes {params()["hashes"].split('|')};
    BitTorrent::Session::instance()->decreaseTorrentsPriority(hashes);
}

void TorrentsController::topPrioAction()
{
    checkParams({"hashes"});

    if (!BitTorrent::Session::instance()->isQueueingSystemEnabled())
        throw APIError(APIErrorType::Conflict, tr("Torrent queueing must be enabled"));

    const QStringList hashes {params()["hashes"].split('|')};
    BitTorrent::Session::instance()->topTorrentsPriority(hashes);
}

void TorrentsController::bottomPrioAction()
{
    checkParams({"hashes"});

    if (!BitTorrent::Session::instance()->isQueueingSystemEnabled())
        throw APIError(APIErrorType::Conflict, tr("Torrent queueing must be enabled"));

    const QStringList hashes {params()["hashes"].split('|')};
    BitTorrent::Session::instance()->bottomTorrentsPriority(hashes);
}

void TorrentsController::setLocationAction()
{
    checkParams({"hashes", "location"});

    const QStringList hashes {params()["hashes"].split("|")};
    const QString newLocation {params()["location"].trimmed()};

    if (newLocation.isEmpty())
        throw APIError(APIErrorType::BadParams, tr("Save path is empty"));
        
    // try to create the location if it does not exist
    if (!QDir(newLocation).mkpath("."))
        throw APIError(APIErrorType::Conflict, tr("Cannot make save path"));
    
    // check permissions
    if (!QFileInfo(newLocation).isWritable())
        throw APIError(APIErrorType::AccessDenied, tr("Cannot write to directory"));

    applyToTorrents(hashes, [newLocation](BitTorrent::TorrentHandle *torrent)
    {
        LogMsg(tr("WebUI Set location: moving \"%1\", from \"%2\" to \"%3\"")
            .arg(torrent->name(), Utils::Fs::toNativePath(torrent->savePath()), Utils::Fs::toNativePath(newLocation)));
        torrent->move(Utils::Fs::expandPathAbs(newLocation));
    });
}

void TorrentsController::renameAction()
{
    checkParams({"hash", "name"});

    const QString hash = params()["hash"];
    QString name = params()["name"].trimmed();

    if (name.isEmpty())
        throw APIError(APIErrorType::Conflict, tr("Incorrect torrent name"));

    BitTorrent::TorrentHandle *const torrent = BitTorrent::Session::instance()->findTorrent(hash);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    name.replace(QRegularExpression("\r?\n|\r"), " ");
    torrent->setName(name);
}

void TorrentsController::setAutoManagementAction()
{
    checkParams({"hashes", "enable"});

    const QStringList hashes {params()["hashes"].split('|')};
    const bool isEnabled {parseBool(params()["enable"], false)};

    applyToTorrents(hashes, [isEnabled](BitTorrent::TorrentHandle *torrent)
    {
        torrent->setAutoTMMEnabled(isEnabled);
    });
}

void TorrentsController::recheckAction()
{
    checkParams({"hashes"});

    const QStringList hashes {params()["hashes"].split('|')};
    applyToTorrents(hashes, [](BitTorrent::TorrentHandle *torrent) { torrent->forceRecheck(); });
}

void TorrentsController::setCategoryAction()
{
    checkParams({"hashes", "category"});

    const QStringList hashes {params()["hashes"].split('|')};
    const QString category {params()["category"].trimmed()};
    applyToTorrents(hashes, [category](BitTorrent::TorrentHandle *torrent)
    {
        if (!torrent->setCategory(category))
            throw APIError(APIErrorType::Conflict, tr("Incorrect category name"));
    });
}

void TorrentsController::createCategoryAction()
{
    checkParams({"category"});

    const QString category {params()["category"].trimmed()};
    if (!BitTorrent::Session::isValidCategoryName(category) && !category.isEmpty())
        throw APIError(APIErrorType::Conflict, tr("Incorrect category name"));

    BitTorrent::Session::instance()->addCategory(category);
}

void TorrentsController::removeCategoriesAction()
{
    checkParams({"categories"});

    const QStringList categories {params()["categories"].split('\n')};
    for (const QString &category : categories)
        BitTorrent::Session::instance()->removeCategory(category);
}
