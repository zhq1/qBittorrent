/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2015, 2018  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
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

#ifndef NET_DOWNLOADMANAGER_H
#define NET_DOWNLOADMANAGER_H

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QQueue>
#include <QSet>

class QNetworkReply;
class QNetworkCookie;
class QSslError;
class QUrl;

namespace Net
{
    class DownloadHandler;

    class DownloadRequest
    {
    public:
        DownloadRequest(const QString &url);
        DownloadRequest(const DownloadRequest &other) = default;

        QString url() const;
        DownloadRequest &url(const QString &value);

        QString userAgent() const;
        DownloadRequest &userAgent(const QString &value);

        qint64 limit() const;
        DownloadRequest &limit(qint64 value);

        bool saveToFile() const;
        DownloadRequest &saveToFile(bool value);

        bool handleRedirectToMagnet() const;
        DownloadRequest &handleRedirectToMagnet(bool value);

    private:
        QString m_url;
        QString m_userAgent;
        qint64 m_limit = 0;
        bool m_saveToFile = false;
        bool m_handleRedirectToMagnet = false;
    };

    struct ServiceID
    {
        QString hostName;
        int port;

        static ServiceID fromURL(const QUrl &url);
    };

    class DownloadManager : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY(DownloadManager)

    public:
        static void initInstance();
        static void freeInstance();
        static DownloadManager *instance();

        DownloadHandler *download(const DownloadRequest &downloadRequest);

        void registerSequentialService(const ServiceID &serviceID);

        QList<QNetworkCookie> cookiesForUrl(const QUrl &url) const;
        bool setCookiesFromUrl(const QList<QNetworkCookie> &cookieList, const QUrl &url);
        QList<QNetworkCookie> allCookies() const;
        void setAllCookies(const QList<QNetworkCookie> &cookieList);
        bool deleteCookie(const QNetworkCookie &cookie);

    private slots:
    #ifndef QT_NO_OPENSSL
        void ignoreSslErrors(QNetworkReply *, const QList<QSslError> &);
    #endif

    private:
        explicit DownloadManager(QObject *parent = nullptr);

        void applyProxySettings();
        void handleReplyFinished(QNetworkReply *reply);

        static DownloadManager *m_instance;
        QNetworkAccessManager m_networkManager;

        QSet<ServiceID> m_sequentialServices;
        QSet<ServiceID> m_busyServices;
        QHash<ServiceID, QQueue<DownloadHandler *>> m_waitingJobs;
    };

    uint qHash(const ServiceID &serviceID, uint seed);
    bool operator==(const ServiceID &lhs, const ServiceID &rhs);
}

#endif // NET_DOWNLOADMANAGER_H
