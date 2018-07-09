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

#include "searchpluginmanager.h"

#include <memory>

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QList>
#include <QPointer>
#include <QProcess>

#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadhandler.h"
#include "base/net/downloadmanager.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/utils/foreignapps.h"
#include "base/utils/fs.h"
#include "base/utils/misc.h"
#include "searchdownloadhandler.h"
#include "searchhandler.h"

namespace
{
    void clearPythonCache(const QString &path)
    {
        // remove python cache artifacts in `path` and subdirs

        QStringList dirs = {path};
        QDirIterator iter {path, (QDir::AllDirs | QDir::NoDotAndDotDot), QDirIterator::Subdirectories};
        while (iter.hasNext())
            dirs += iter.next();

        for (const QString &dir : qAsConst(dirs)) {
            // python 3: remove "__pycache__" folders
            if (dir.endsWith("/__pycache__")) {
                Utils::Fs::removeDirRecursive(dir);
                continue;
            }

            // python 2: remove "*.pyc" files
            const QStringList files = QDir(dir).entryList(QDir::Files);
            for (const QString file : files) {
                if (file.endsWith(".pyc"))
                    Utils::Fs::forceRemove(file);
            }
        }
    }
}

QPointer<SearchPluginManager> SearchPluginManager::m_instance = nullptr;

SearchPluginManager::SearchPluginManager()
    : m_updateUrl(QString("http://searchplugins.qbittorrent.org/%1/engines/").arg(Utils::ForeignApps::pythonInfo().version.majorNumber() >= 3 ? "nova3" : "nova"))
{
    Q_ASSERT(!m_instance); // only one instance is allowed
    m_instance = this;

    updateNova();
    update();
}

SearchPluginManager::~SearchPluginManager()
{
    qDeleteAll(m_plugins);
}

SearchPluginManager *SearchPluginManager::instance()
{
    return m_instance;
}

QStringList SearchPluginManager::allPlugins() const
{
    return m_plugins.keys();
}

QStringList SearchPluginManager::enabledPlugins() const
{
    QStringList plugins;
    for (const PluginInfo *plugin : qAsConst(m_plugins)) {
        if (plugin->enabled)
            plugins << plugin->name;
    }

    return plugins;
}

QStringList SearchPluginManager::supportedCategories() const
{
    QStringList result;
    for (const PluginInfo *plugin : qAsConst(m_plugins)) {
        if (plugin->enabled) {
            foreach (QString cat, plugin->supportedCategories) {
                if (!result.contains(cat))
                    result << cat;
            }
        }
    }

    return result;
}

QStringList SearchPluginManager::getPluginCategories(const QString &pluginName) const
{
    QStringList plugins;
    if (pluginName == "all")
        plugins = allPlugins();
    else if ((pluginName == "enabled") || (pluginName == "multi"))
        plugins = enabledPlugins();
    else
        plugins << pluginName.trimmed();

    QSet<QString> categories;
    for (const QString &pluginName : qAsConst(plugins)) {
        const PluginInfo *plugin = pluginInfo(pluginName);
        if (!plugin) continue; // plugin wasn't found
        for (const QString &category : plugin->supportedCategories)
            categories << category;
    }

    return categories.toList();
}

PluginInfo *SearchPluginManager::pluginInfo(const QString &name) const
{
    return m_plugins.value(name);
}

void SearchPluginManager::enablePlugin(const QString &name, bool enabled)
{
    PluginInfo *plugin = m_plugins.value(name, 0);
    if (plugin) {
        plugin->enabled = enabled;
        // Save to Hard disk
        Preferences *const pref = Preferences::instance();
        QStringList disabledPlugins = pref->getSearchEngDisabled();
        if (enabled)
            disabledPlugins.removeAll(name);
        else if (!disabledPlugins.contains(name))
            disabledPlugins.append(name);
        pref->setSearchEngDisabled(disabledPlugins);

        emit pluginEnabled(name, enabled);
    }
}

// Updates shipped plugin
void SearchPluginManager::updatePlugin(const QString &name)
{
    installPlugin(QString("%1%2.py").arg(m_updateUrl, name));
}

// Install or update plugin from file or url
void SearchPluginManager::installPlugin(const QString &source)
{
    qDebug("Asked to install plugin at %s", qUtf8Printable(source));

    clearPythonCache(engineLocation());

    if (Utils::Misc::isUrl(source)) {
        using namespace Net;
        DownloadHandler *handler = DownloadManager::instance()->download(DownloadRequest(source).saveToFile(true));
        connect(handler, static_cast<void (DownloadHandler::*)(const QString &, const QString &)>(&DownloadHandler::downloadFinished)
                , this, &SearchPluginManager::pluginDownloaded);
        connect(handler, &DownloadHandler::downloadFailed, this, &SearchPluginManager::pluginDownloadFailed);
    }
    else {
        QString path = source;
        if (path.startsWith("file:", Qt::CaseInsensitive))
            path = QUrl(path).toLocalFile();

        QString pluginName = Utils::Fs::fileName(path);
        pluginName.chop(pluginName.size() - pluginName.lastIndexOf("."));

        if (!path.endsWith(".py", Qt::CaseInsensitive))
            emit pluginInstallationFailed(pluginName, tr("Unknown search engine plugin file format."));
        else
            installPlugin_impl(pluginName, path);
    }
}

void SearchPluginManager::installPlugin_impl(const QString &name, const QString &path)
{
    PluginVersion newVersion = getPluginVersion(path);
    qDebug() << "Version to be installed:" << newVersion;

    PluginInfo *plugin = pluginInfo(name);
    if (plugin && !(plugin->version < newVersion)) {
        qDebug("Apparently update is not needed, we have a more recent version");
        emit pluginUpdateFailed(name, tr("A more recent version of this plugin is already installed."));
        return;
    }

    // Process with install
    QString destPath = pluginPath(name);
    bool updated = false;
    if (QFile::exists(destPath)) {
        // Backup in case install fails
        QFile::copy(destPath, destPath + ".bak");
        Utils::Fs::forceRemove(destPath);
        updated = true;
    }
    // Copy the plugin
    QFile::copy(path, destPath);
    // Update supported plugins
    update();
    // Check if this was correctly installed
    if (!m_plugins.contains(name)) {
        // Remove broken file
        Utils::Fs::forceRemove(destPath);
        if (updated) {
            // restore backup
            QFile::copy(destPath + ".bak", destPath);
            Utils::Fs::forceRemove(destPath + ".bak");
            // Update supported plugins
            update();
            emit pluginUpdateFailed(name, tr("Plugin is not supported."));
        }
        else {
            emit pluginInstallationFailed(name, tr("Plugin is not supported."));
        }
    }
    else {
        // Install was successful, remove backup
        if (updated)
            Utils::Fs::forceRemove(destPath + ".bak");
    }
}

bool SearchPluginManager::uninstallPlugin(const QString &name)
{
    clearPythonCache(engineLocation());

    // remove it from hard drive
    QDir pluginsFolder(pluginsLocation());
    QStringList filters;
    filters << name + ".*";
    QStringList files = pluginsFolder.entryList(filters, QDir::Files, QDir::Unsorted);
    QString file;
    foreach (file, files)
        Utils::Fs::forceRemove(pluginsFolder.absoluteFilePath(file));
    // Remove it from supported engines
    delete m_plugins.take(name);

    emit pluginUninstalled(name);
    return true;
}

void SearchPluginManager::updateIconPath(PluginInfo *const plugin)
{
    if (!plugin) return;
    QString iconPath = QString("%1/%2.png").arg(pluginsLocation(), plugin->name);
    if (QFile::exists(iconPath)) {
        plugin->iconPath = iconPath;
    }
    else {
        iconPath = QString("%1/%2.ico").arg(pluginsLocation(), plugin->name);
        if (QFile::exists(iconPath))
            plugin->iconPath = iconPath;
    }
}

void SearchPluginManager::checkForUpdates()
{
    // Download version file from update server
    using namespace Net;
    DownloadHandler *handler = DownloadManager::instance()->download({m_updateUrl + "versions.txt"});
    connect(handler, static_cast<void (DownloadHandler::*)(const QString &, const QByteArray &)>(&DownloadHandler::downloadFinished)
            , this, &SearchPluginManager::versionInfoDownloaded);
    connect(handler, &DownloadHandler::downloadFailed, this, &SearchPluginManager::versionInfoDownloadFailed);
}

SearchDownloadHandler *SearchPluginManager::downloadTorrent(const QString &siteUrl, const QString &url)
{
    return new SearchDownloadHandler {siteUrl, url, this};
}

SearchHandler *SearchPluginManager::startSearch(const QString &pattern, const QString &category, const QStringList &usedPlugins)
{
    // No search pattern entered
    Q_ASSERT(!pattern.isEmpty());

    return new SearchHandler {pattern, category, usedPlugins, this};
}

QString SearchPluginManager::categoryFullName(const QString &categoryName)
{
    static const QHash<QString, QString> categoryTable {
        {"all", tr("All categories")},
        {"movies", tr("Movies")},
        {"tv", tr("TV shows")},
        {"music", tr("Music")},
        {"games", tr("Games")},
        {"anime", tr("Anime")},
        {"software", tr("Software")},
        {"pictures", tr("Pictures")},
        {"books", tr("Books")}
    };
    return categoryTable.value(categoryName);
}

QString SearchPluginManager::pluginFullName(const QString &pluginName)
{
    return pluginInfo(pluginName) ? pluginInfo(pluginName)->fullName : QString();
}

QString SearchPluginManager::pluginsLocation()
{
    return QString("%1/engines").arg(engineLocation());
}

QString SearchPluginManager::engineLocation()
{
    static QString location;
    if (location.isEmpty()) {
        const QString folder = (Utils::ForeignApps::pythonInfo().version.majorNumber() >= 3)
            ? "nova3" : "nova";
        location = Utils::Fs::expandPathAbs(specialFolderLocation(SpecialFolder::Data) + folder);

        const QDir locationDir(location);
        locationDir.mkpath(locationDir.absolutePath());
    }

    return location;
}

void SearchPluginManager::versionInfoDownloaded(const QString &url, const QByteArray &data)
{
    Q_UNUSED(url)
    parseVersionInfo(data);
}

void SearchPluginManager::versionInfoDownloadFailed(const QString &url, const QString &reason)
{
    Q_UNUSED(url)
    emit checkForUpdatesFailed(tr("Update server is temporarily unavailable. %1").arg(reason));
}

void SearchPluginManager::pluginDownloaded(const QString &url, QString filePath)
{
    filePath = Utils::Fs::fromNativePath(filePath);

    QString pluginName = Utils::Fs::fileName(url);
    pluginName.chop(pluginName.size() - pluginName.lastIndexOf(".")); // Remove extension
    installPlugin_impl(pluginName, filePath);
    Utils::Fs::forceRemove(filePath);
}

void SearchPluginManager::pluginDownloadFailed(const QString &url, const QString &reason)
{
    QString pluginName = url.split('/').last();
    pluginName.replace(".py", "", Qt::CaseInsensitive);
    if (pluginInfo(pluginName))
        emit pluginUpdateFailed(pluginName, tr("Failed to download the plugin file. %1").arg(reason));
    else
        emit pluginInstallationFailed(pluginName, tr("Failed to download the plugin file. %1").arg(reason));
}

// Update nova.py search plugin if necessary
void SearchPluginManager::updateNova()
{
    // create nova directory if necessary
    const QDir searchDir(engineLocation());
    const QString novaFolder = Utils::ForeignApps::pythonInfo().version.majorNumber() >= 3
        ? "searchengine/nova3" : "searchengine/nova";

    QFile packageFile(searchDir.absoluteFilePath("__init__.py"));
    packageFile.open(QIODevice::WriteOnly);
    packageFile.close();

    searchDir.mkdir("engines");

    QFile packageFile2(searchDir.absolutePath() + "/engines/__init__.py");
    packageFile2.open(QIODevice::WriteOnly);
    packageFile2.close();

    // Copy search plugin files (if necessary)
    const auto updateFile = [&novaFolder](const QString &filename, const bool compareVersion)
    {
        const QString filePathBundled = ":/" + novaFolder + '/' + filename;
        const QString filePathDisk = QDir(engineLocation()).absoluteFilePath(filename);

        if (compareVersion && (getPluginVersion(filePathBundled) <= getPluginVersion(filePathDisk)))
            return;

        Utils::Fs::forceRemove(filePathDisk);
        QFile::copy(filePathBundled, filePathDisk);
    };

    updateFile("helpers.py", true);
    updateFile("nova2.py", true);
    updateFile("nova2dl.py", true);
    updateFile("novaprinter.py", true);
    updateFile("socks.py", false);

    if (Utils::ForeignApps::pythonInfo().version.majorNumber() >= 3)
        updateFile("sgmllib3.py", false);
    else
        updateFile("fix_encoding.py", false);
}

void SearchPluginManager::update()
{
    QProcess nova;
    nova.setProcessEnvironment(QProcessEnvironment::systemEnvironment());

    const QStringList params {Utils::Fs::toNativePath(engineLocation() + "/nova2.py"), "--capabilities"};
    nova.start(Utils::ForeignApps::pythonInfo().executableName, params, QIODevice::ReadOnly);
    nova.waitForFinished();

    QString capabilities = nova.readAll();
    QDomDocument xmlDoc;
    if (!xmlDoc.setContent(capabilities)) {
        qWarning() << "Could not parse Nova search engine capabilities, msg: " << capabilities.toLocal8Bit().data();
        qWarning() << "Error: " << nova.readAllStandardError().constData();
        return;
    }

    QDomElement root = xmlDoc.documentElement();
    if (root.tagName() != "capabilities") {
        qWarning() << "Invalid XML file for Nova search engine capabilities, msg: " << capabilities.toLocal8Bit().data();
        return;
    }

    for (QDomNode engineNode = root.firstChild(); !engineNode.isNull(); engineNode = engineNode.nextSibling()) {
        QDomElement engineElem = engineNode.toElement();
        if (!engineElem.isNull()) {
            QString pluginName = engineElem.tagName();

            std::unique_ptr<PluginInfo> plugin {new PluginInfo {}};
            plugin->name = pluginName;
            plugin->version = getPluginVersion(pluginPath(pluginName));
            plugin->fullName = engineElem.elementsByTagName("name").at(0).toElement().text();
            plugin->url = engineElem.elementsByTagName("url").at(0).toElement().text();

            const auto categories = engineElem.elementsByTagName("categories").at(0).toElement().text().split(' ');
            for (QString cat : categories) {
                cat = cat.trimmed();
                if (!cat.isEmpty())
                    plugin->supportedCategories << cat;
            }

            QStringList disabledEngines = Preferences::instance()->getSearchEngDisabled();
            plugin->enabled = !disabledEngines.contains(pluginName);

            updateIconPath(plugin.get());

            if (!m_plugins.contains(pluginName)) {
                m_plugins[pluginName] = plugin.release();
                emit pluginInstalled(pluginName);
            }
            else if (m_plugins[pluginName]->version != plugin->version) {
                delete m_plugins.take(pluginName);
                m_plugins[pluginName] = plugin.release();
                emit pluginUpdated(pluginName);
            }
        }
    }
}

void SearchPluginManager::parseVersionInfo(const QByteArray &info)
{
    qDebug("Checking if update is needed");

    QHash<QString, PluginVersion> updateInfo;
    bool dataCorrect = false;
    QList<QByteArray> lines = info.split('\n');
    foreach (QByteArray line, lines) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith("#")) continue;

        QList<QByteArray> list = line.split(' ');
        if (list.size() != 2) continue;

        QString pluginName = QString(list.first());
        if (!pluginName.endsWith(":")) continue;

        pluginName.chop(1); // remove trailing ':'
        PluginVersion version = PluginVersion::tryParse(list.last(), {});
        if (version == PluginVersion()) continue;

        dataCorrect = true;
        if (isUpdateNeeded(pluginName, version)) {
            qDebug("Plugin: %s is outdated", qUtf8Printable(pluginName));
            updateInfo[pluginName] = version;
        }
    }

    if (!dataCorrect)
        emit checkForUpdatesFailed(tr("An incorrect update info received."));
    else
        emit checkForUpdatesFinished(updateInfo);
}

bool SearchPluginManager::isUpdateNeeded(QString pluginName, PluginVersion newVersion) const
{
    PluginInfo *plugin = pluginInfo(pluginName);
    if (!plugin) return true;

    PluginVersion oldVersion = plugin->version;
    qDebug() << "IsUpdate needed? to be installed:" << newVersion << ", already installed:" << oldVersion;
    return (newVersion > oldVersion);
}

QString SearchPluginManager::pluginPath(const QString &name)
{
    return QString("%1/%2.py").arg(pluginsLocation(), name);
}

PluginVersion SearchPluginManager::getPluginVersion(const QString &filePath)
{
    QFile pluginFile(filePath);
    if (!pluginFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    while (!pluginFile.atEnd()) {
        const QString line = QString(pluginFile.readLine()).remove(' ');
        if (!line.startsWith("#VERSION:", Qt::CaseInsensitive)) continue;

        const QString versionStr = line.mid(9);
        const PluginVersion version = PluginVersion::tryParse(versionStr, {});
        if (version.isValid())
            return version;

        LogMsg(tr("Search plugin '%1' contains invalid version string ('%2')")
            .arg(Utils::Fs::fileName(filePath), versionStr), Log::MsgType::WARNING);
        break;
    }

    return {};
}
