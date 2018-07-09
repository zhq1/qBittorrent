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

#include "serialize_torrent.h"

#include "base/bittorrent/session.h"
#include "base/bittorrent/torrenthandle.h"
#include "base/utils/fs.h"
#include "base/utils/string.h"

namespace
{
    QString torrentStateToString(const BitTorrent::TorrentState state)
    {
        switch (state) {
        case BitTorrent::TorrentState::Error:
            return QLatin1String("error");
        case BitTorrent::TorrentState::MissingFiles:
            return QLatin1String("missingFiles");
        case BitTorrent::TorrentState::Uploading:
            return QLatin1String("uploading");
        case BitTorrent::TorrentState::PausedUploading:
            return QLatin1String("pausedUP");
        case BitTorrent::TorrentState::QueuedUploading:
            return QLatin1String("queuedUP");
        case BitTorrent::TorrentState::StalledUploading:
            return QLatin1String("stalledUP");
        case BitTorrent::TorrentState::CheckingUploading:
            return QLatin1String("checkingUP");
        case BitTorrent::TorrentState::ForcedUploading:
            return QLatin1String("forcedUP");
        case BitTorrent::TorrentState::Allocating:
            return QLatin1String("allocating");
        case BitTorrent::TorrentState::Downloading:
            return QLatin1String("downloading");
        case BitTorrent::TorrentState::DownloadingMetadata:
            return QLatin1String("metaDL");
        case BitTorrent::TorrentState::PausedDownloading:
            return QLatin1String("pausedDL");
        case BitTorrent::TorrentState::QueuedDownloading:
            return QLatin1String("queuedDL");
        case BitTorrent::TorrentState::StalledDownloading:
            return QLatin1String("stalledDL");
        case BitTorrent::TorrentState::CheckingDownloading:
            return QLatin1String("checkingDL");
        case BitTorrent::TorrentState::ForcedDownloading:
            return QLatin1String("forcedDL");
#if LIBTORRENT_VERSION_NUM < 10100
        case BitTorrent::TorrentState::QueuedForChecking:
            return QLatin1String("queuedForChecking");
#endif
        case BitTorrent::TorrentState::CheckingResumeData:
            return QLatin1String("checkingResumeData");
        case BitTorrent::TorrentState::Moving:
            return QLatin1String("moving");
        default:
            return QLatin1String("unknown");
        }
    }
}

QVariantMap serialize(const BitTorrent::TorrentHandle &torrent)
{
    QVariantMap ret;
    ret[KEY_TORRENT_HASH] = QString(torrent.hash());
    ret[KEY_TORRENT_NAME] = torrent.name();
    ret[KEY_TORRENT_MAGNET_URI] = torrent.toMagnetUri();
    ret[KEY_TORRENT_SIZE] = torrent.wantedSize();
    ret[KEY_TORRENT_PROGRESS] = torrent.progress();
    ret[KEY_TORRENT_DLSPEED] = torrent.downloadPayloadRate();
    ret[KEY_TORRENT_UPSPEED] = torrent.uploadPayloadRate();
    ret[KEY_TORRENT_PRIORITY] = torrent.queuePosition();
    ret[KEY_TORRENT_SEEDS] = torrent.seedsCount();
    ret[KEY_TORRENT_NUM_COMPLETE] = torrent.totalSeedsCount();
    ret[KEY_TORRENT_LEECHS] = torrent.leechsCount();
    ret[KEY_TORRENT_NUM_INCOMPLETE] = torrent.totalLeechersCount();
    const qreal ratio = torrent.realRatio();
    ret[KEY_TORRENT_RATIO] = (ratio > BitTorrent::TorrentHandle::MAX_RATIO) ? -1 : ratio;
    ret[KEY_TORRENT_STATE] = torrentStateToString(torrent.state());
    ret[KEY_TORRENT_ETA] = torrent.eta();
    ret[KEY_TORRENT_SEQUENTIAL_DOWNLOAD] = torrent.isSequentialDownload();
    if (torrent.hasMetadata())
        ret[KEY_TORRENT_FIRST_LAST_PIECE_PRIO] = torrent.hasFirstLastPiecePriority();
    ret[KEY_TORRENT_CATEGORY] = torrent.category();
    ret[KEY_TORRENT_TAGS] = torrent.tags().toList().join(", ");
    ret[KEY_TORRENT_SUPER_SEEDING] = torrent.superSeeding();
    ret[KEY_TORRENT_FORCE_START] = torrent.isForced();
    ret[KEY_TORRENT_SAVE_PATH] = Utils::Fs::toNativePath(torrent.savePath());
    ret[KEY_TORRENT_ADDED_ON] = torrent.addedTime().toTime_t();
    ret[KEY_TORRENT_COMPLETION_ON] = torrent.completedTime().toTime_t();
    ret[KEY_TORRENT_TRACKER] = torrent.currentTracker();
    ret[KEY_TORRENT_DL_LIMIT] = torrent.downloadLimit();
    ret[KEY_TORRENT_UP_LIMIT] = torrent.uploadLimit();
    ret[KEY_TORRENT_AMOUNT_DOWNLOADED] = torrent.totalDownload();
    ret[KEY_TORRENT_AMOUNT_UPLOADED] = torrent.totalUpload();
    ret[KEY_TORRENT_AMOUNT_DOWNLOADED_SESSION] = torrent.totalPayloadDownload();
    ret[KEY_TORRENT_AMOUNT_UPLOADED_SESSION] = torrent.totalPayloadUpload();
    ret[KEY_TORRENT_AMOUNT_LEFT] = torrent.incompletedSize();
    ret[KEY_TORRENT_AMOUNT_COMPLETED] = torrent.completedSize();
    ret[KEY_TORRENT_MAX_RATIO] = torrent.maxRatio();
    ret[KEY_TORRENT_MAX_SEEDING_TIME] = torrent.maxSeedingTime();
    ret[KEY_TORRENT_RATIO_LIMIT] = torrent.ratioLimit();
    ret[KEY_TORRENT_SEEDING_TIME_LIMIT] = torrent.seedingTimeLimit();
    ret[KEY_TORRENT_LAST_SEEN_COMPLETE_TIME] = torrent.lastSeenComplete().toTime_t();
    ret[KEY_TORRENT_AUTO_TORRENT_MANAGEMENT] = torrent.isAutoTMMEnabled();
    ret[KEY_TORRENT_TIME_ACTIVE] = torrent.activeTime();

    if (torrent.isPaused() || torrent.isChecking()) {
        ret[KEY_TORRENT_LAST_ACTIVITY_TIME] = 0;
    }
    else {
        QDateTime dt = QDateTime::currentDateTime();
        dt = dt.addSecs(-torrent.timeSinceActivity());
        ret[KEY_TORRENT_LAST_ACTIVITY_TIME] = dt.toTime_t();
    }

    ret[KEY_TORRENT_TOTAL_SIZE] = torrent.totalSize();

    return ret;
}
