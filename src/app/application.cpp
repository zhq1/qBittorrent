/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2015  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez
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

#include "application.h"

#include <algorithm>

#include <QAtomicInt>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QProcess>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <memory>
#include <Shellapi.h>
#endif

#ifndef DISABLE_GUI
#ifdef Q_OS_WIN
#include <QSessionManager>
#include <QSharedMemory>
#endif // Q_OS_WIN
#ifdef Q_OS_MAC
#include <QFileOpenEvent>
#endif // Q_OS_MAC
#include "addnewtorrentdialog.h"
#include "gui/guiiconprovider.h"
#include "mainwindow.h"
#include "shutdownconfirmdialog.h"
#else // DISABLE_GUI
#include <cstdio>
#endif // DISABLE_GUI

#include "base/bittorrent/session.h"
#include "base/bittorrent/torrenthandle.h"
#include "base/iconprovider.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/net/geoipmanager.h"
#include "base/net/proxyconfigurationmanager.h"
#include "base/net/smtp.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/rss/rss_autodownloader.h"
#include "base/rss/rss_session.h"
#include "base/scanfoldersmodel.h"
#include "base/settingsstorage.h"
#include "base/utils/fs.h"
#include "base/utils/misc.h"
#include "base/utils/string.h"
#include "filelogger.h"

#ifndef DISABLE_WEBUI
#include "webui/webui.h"
#endif

namespace
{
#define SETTINGS_KEY(name) "Application/" name

    // FileLogger properties keys
#define FILELOGGER_SETTINGS_KEY(name) QStringLiteral(SETTINGS_KEY("FileLogger/") name)
    const QString KEY_FILELOGGER_ENABLED = FILELOGGER_SETTINGS_KEY("Enabled");
    const QString KEY_FILELOGGER_PATH = FILELOGGER_SETTINGS_KEY("Path");
    const QString KEY_FILELOGGER_BACKUP = FILELOGGER_SETTINGS_KEY("Backup");
    const QString KEY_FILELOGGER_DELETEOLD = FILELOGGER_SETTINGS_KEY("DeleteOld");
    const QString KEY_FILELOGGER_MAXSIZEBYTES = FILELOGGER_SETTINGS_KEY("MaxSizeBytes");
    const QString KEY_FILELOGGER_AGE = FILELOGGER_SETTINGS_KEY("Age");
    const QString KEY_FILELOGGER_AGETYPE = FILELOGGER_SETTINGS_KEY("AgeType");

    // just a shortcut
    inline SettingsStorage *settings() { return  SettingsStorage::instance(); }

    const QString LOG_FOLDER("logs");
    const char PARAMS_SEPARATOR[] = "|";

    const QString DEFAULT_PORTABLE_MODE_PROFILE_DIR = QLatin1String("profile");

    const int MIN_FILELOG_SIZE = 1024; // 1KiB
    const int MAX_FILELOG_SIZE = 1000 * 1024 * 1024; // 1000MiB
    const int DEFAULT_FILELOG_SIZE = 65 * 1024; // 65KiB
}

Application::Application(const QString &id, int &argc, char **argv)
    : BaseApplication(id, argc, argv)
    , m_running(false)
    , m_shutdownAct(ShutdownDialogAction::Exit)
    , m_commandLineArgs(parseCommandLine(this->arguments()))
#ifndef DISABLE_WEBUI
    , m_webui(nullptr)
#endif
{
    qRegisterMetaType<Log::Msg>("Log::Msg");

    setApplicationName("qBittorrent");
    validateCommandLineParameters();

    QString profileDir = m_commandLineArgs.portableMode
        ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(DEFAULT_PORTABLE_MODE_PROFILE_DIR)
        : m_commandLineArgs.profileDir;

    Profile::initialize(profileDir, m_commandLineArgs.configurationName,
                        m_commandLineArgs.relativeFastresumePaths || m_commandLineArgs.portableMode);

    Logger::initInstance();
    SettingsStorage::initInstance();
    Preferences::initInstance();

    if (m_commandLineArgs.webUiPort > 0) // it will be -1 when user did not set any value
        Preferences::instance()->setWebUiPort(m_commandLineArgs.webUiPort);

    initializeTranslation();

#if !defined(DISABLE_GUI)
    setAttribute(Qt::AA_UseHighDpiPixmaps, true);  // opt-in to the high DPI pixmap support
    setQuitOnLastWindowClosed(false);
#endif

#if defined(Q_OS_WIN) && !defined(DISABLE_GUI)
    connect(this, &QGuiApplication::commitDataRequest, this, &Application::shutdownCleanup, Qt::DirectConnection);
#endif

    connect(this, &Application::messageReceived, this, &Application::processMessage);
    connect(this, &QCoreApplication::aboutToQuit, this, &Application::cleanup);

    if (isFileLoggerEnabled())
        m_fileLogger = new FileLogger(fileLoggerPath(), isFileLoggerBackup(), fileLoggerMaxSize(), isFileLoggerDeleteOld(), fileLoggerAge(), static_cast<FileLogger::FileLogAgeType>(fileLoggerAgeType()));

    Logger::instance()->addMessage(tr("qBittorrent %1 started", "qBittorrent v3.2.0alpha started").arg(QBT_VERSION));
}

Application::~Application()
{
    // we still need to call cleanup()
    // in case the App failed to start
    cleanup();
}

#ifndef DISABLE_GUI
QPointer<MainWindow> Application::mainWindow()
{
    return m_window;
}
#endif

const QBtCommandLineParameters &Application::commandLineArgs() const
{
    return m_commandLineArgs;
}

bool Application::isFileLoggerEnabled() const
{
    return settings()->loadValue(KEY_FILELOGGER_ENABLED, true).toBool();
}

void Application::setFileLoggerEnabled(bool value)
{
    if (value && !m_fileLogger)
        m_fileLogger = new FileLogger(fileLoggerPath(), isFileLoggerBackup(), fileLoggerMaxSize(), isFileLoggerDeleteOld(), fileLoggerAge(), static_cast<FileLogger::FileLogAgeType>(fileLoggerAgeType()));
    else if (!value)
        delete m_fileLogger;
    settings()->storeValue(KEY_FILELOGGER_ENABLED, value);
}

QString Application::fileLoggerPath() const
{
    return settings()->loadValue(KEY_FILELOGGER_PATH,
            QVariant(specialFolderLocation(SpecialFolder::Data) + LOG_FOLDER)).toString();
}

void Application::setFileLoggerPath(const QString &path)
{
    if (m_fileLogger)
        m_fileLogger->changePath(path);
    settings()->storeValue(KEY_FILELOGGER_PATH, path);
}

bool Application::isFileLoggerBackup() const
{
    return settings()->loadValue(KEY_FILELOGGER_BACKUP, true).toBool();
}

void Application::setFileLoggerBackup(bool value)
{
    if (m_fileLogger)
        m_fileLogger->setBackup(value);
    settings()->storeValue(KEY_FILELOGGER_BACKUP, value);
}

bool Application::isFileLoggerDeleteOld() const
{
    return settings()->loadValue(KEY_FILELOGGER_DELETEOLD, true).toBool();
}

void Application::setFileLoggerDeleteOld(bool value)
{
    if (value && m_fileLogger)
        m_fileLogger->deleteOld(fileLoggerAge(), static_cast<FileLogger::FileLogAgeType>(fileLoggerAgeType()));
    settings()->storeValue(KEY_FILELOGGER_DELETEOLD, value);
}

int Application::fileLoggerMaxSize() const
{
    int val = settings()->loadValue(KEY_FILELOGGER_MAXSIZEBYTES, DEFAULT_FILELOG_SIZE).toInt();
    return std::min(std::max(val, MIN_FILELOG_SIZE), MAX_FILELOG_SIZE);
}

void Application::setFileLoggerMaxSize(const int bytes)
{
    int clampedValue = std::min(std::max(bytes, MIN_FILELOG_SIZE), MAX_FILELOG_SIZE);
    if (m_fileLogger)
        m_fileLogger->setMaxSize(clampedValue);
    settings()->storeValue(KEY_FILELOGGER_MAXSIZEBYTES, clampedValue);
}

int Application::fileLoggerAge() const
{
    int val = settings()->loadValue(KEY_FILELOGGER_AGE, 1).toInt();
    return std::min(std::max(val, 1), 365);
}

void Application::setFileLoggerAge(const int value)
{
    settings()->storeValue(KEY_FILELOGGER_AGE, std::min(std::max(value, 1), 365));
}

int Application::fileLoggerAgeType() const
{
    int val = settings()->loadValue(KEY_FILELOGGER_AGETYPE, 1).toInt();
    return ((val < 0) || (val > 2)) ? 1 : val;
}

void Application::setFileLoggerAgeType(const int value)
{
    settings()->storeValue(KEY_FILELOGGER_AGETYPE, ((value < 0) || (value > 2)) ? 1 : value);
}

void Application::processMessage(const QString &message)
{
    QStringList params = message.split(QLatin1String(PARAMS_SEPARATOR), QString::SkipEmptyParts);
    // If Application is not running (i.e., other
    // components are not ready) store params
    if (m_running)
        processParams(params);
    else
        m_paramsQueue.append(params);
}

void Application::runExternalProgram(const BitTorrent::TorrentHandle *torrent) const
{
    QString program = Preferences::instance()->getAutoRunProgram().trimmed();
    program.replace("%N", torrent->name());
    program.replace("%L", torrent->category());

    QStringList tags = torrent->tags().toList();
    std::sort(tags.begin(), tags.end(), Utils::String::naturalLessThan<Qt::CaseInsensitive>);
    program.replace("%G", tags.join(','));

#if defined(Q_OS_WIN)
    const auto chopPathSep = [](const QString &str) -> QString
    {
        if (str.endsWith('\\'))
            return str.mid(0, (str.length() -1));
        return str;
    };
    program.replace("%F", chopPathSep(Utils::Fs::toNativePath(torrent->contentPath())));
    program.replace("%R", chopPathSep(Utils::Fs::toNativePath(torrent->rootPath())));
    program.replace("%D", chopPathSep(Utils::Fs::toNativePath(torrent->savePath())));
#else
    program.replace("%F", Utils::Fs::toNativePath(torrent->contentPath()));
    program.replace("%R", Utils::Fs::toNativePath(torrent->rootPath()));
    program.replace("%D", Utils::Fs::toNativePath(torrent->savePath()));
#endif
    program.replace("%C", QString::number(torrent->filesCount()));
    program.replace("%Z", QString::number(torrent->totalSize()));
    program.replace("%T", torrent->currentTracker());
    program.replace("%I", torrent->hash());

    Logger *logger = Logger::instance();
    logger->addMessage(tr("Torrent: %1, running external program, command: %2").arg(torrent->name(), program));

#if defined(Q_OS_WIN)
    std::unique_ptr<wchar_t[]> programWchar(new wchar_t[program.length() + 1] {});
    program.toWCharArray(programWchar.get());

    // Need to split arguments manually because QProcess::startDetached(QString)
    // will strip off empty parameters.
    // E.g. `python.exe "1" "" "3"` will become `python.exe "1" "3"`
    int argCount = 0;
    LPWSTR *args = ::CommandLineToArgvW(programWchar.get(), &argCount);

    QStringList argList;
    for (int i = 1; i < argCount; ++i)
        argList += QString::fromWCharArray(args[i]);

    QProcess::startDetached(QString::fromWCharArray(args[0]), argList);

    ::LocalFree(args);
#else
    QProcess::startDetached(QLatin1String("/bin/sh"), {QLatin1String("-c"), program});
#endif
}

void Application::sendNotificationEmail(const BitTorrent::TorrentHandle *torrent)
{
    // Prepare mail content
    const QString content = tr("Torrent name: %1").arg(torrent->name()) + '\n'
        + tr("Torrent size: %1").arg(Utils::Misc::friendlyUnit(torrent->wantedSize())) + '\n'
        + tr("Save path: %1").arg(torrent->savePath()) + "\n\n"
        + tr("The torrent was downloaded in %1.", "The torrent was downloaded in 1 hour and 20 seconds")
            .arg(Utils::Misc::userFriendlyDuration(torrent->activeTime())) + "\n\n\n"
        + tr("Thank you for using qBittorrent.") + '\n';

    // Send the notification email
    const Preferences *pref = Preferences::instance();
    Net::Smtp *smtp = new Net::Smtp(this);
    smtp->sendMail(pref->getMailNotificationSender(),
                     pref->getMailNotificationEmail(),
                     tr("[qBittorrent] '%1' has finished downloading").arg(torrent->name()),
                     content);
}

void Application::torrentFinished(BitTorrent::TorrentHandle *const torrent)
{
    Preferences *const pref = Preferences::instance();

    // AutoRun program
    if (pref->isAutoRunEnabled())
        runExternalProgram(torrent);

    // Mail notification
    if (pref->isMailNotificationEnabled()) {
        Logger::instance()->addMessage(tr("Torrent: %1, sending mail notification").arg(torrent->name()));
        sendNotificationEmail(torrent);
    }
}

void Application::allTorrentsFinished()
{
    Preferences *const pref = Preferences::instance();
    bool isExit = pref->shutdownqBTWhenDownloadsComplete();
    bool isShutdown = pref->shutdownWhenDownloadsComplete();
    bool isSuspend = pref->suspendWhenDownloadsComplete();
    bool isHibernate = pref->hibernateWhenDownloadsComplete();

    bool haveAction = isExit || isShutdown || isSuspend || isHibernate;
    if (!haveAction) return;

    ShutdownDialogAction action = ShutdownDialogAction::Exit;
    if (isSuspend)
        action = ShutdownDialogAction::Suspend;
    else if (isHibernate)
        action = ShutdownDialogAction::Hibernate;
    else if (isShutdown)
        action = ShutdownDialogAction::Shutdown;

#ifndef DISABLE_GUI
    // ask confirm
    if ((action == ShutdownDialogAction::Exit) && (pref->dontConfirmAutoExit())) {
        // do nothing & skip confirm
    }
    else {
        if (!ShutdownConfirmDialog::askForConfirmation(m_window, action)) return;
    }
#endif // DISABLE_GUI

    // Actually shut down
    if (action != ShutdownDialogAction::Exit) {
        qDebug("Preparing for auto-shutdown because all downloads are complete!");
        // Disabling it for next time
        pref->setShutdownWhenDownloadsComplete(false);
        pref->setSuspendWhenDownloadsComplete(false);
        pref->setHibernateWhenDownloadsComplete(false);
        // Make sure preferences are synced before exiting
        m_shutdownAct = action;
    }

    qDebug("Exiting the application");
    exit();
}

bool Application::sendParams(const QStringList &params)
{
    return sendMessage(params.join(QLatin1String(PARAMS_SEPARATOR)));
}

// As program parameters, we can get paths or urls.
// This function parse the parameters and call
// the right addTorrent function, considering
// the parameter type.
void Application::processParams(const QStringList &params)
{
#ifndef DISABLE_GUI
    if (params.isEmpty()) {
        m_window->activate(); // show UI
        return;
    }
#endif
    BitTorrent::AddTorrentParams torrentParams;
    TriStateBool skipTorrentDialog;

    foreach (QString param, params) {
        param = param.trimmed();

        // Process strings indicating options specified by the user.

        if (param.startsWith(QLatin1String("@savePath="))) {
            torrentParams.savePath = param.mid(10);
            continue;
        }

        if (param.startsWith(QLatin1String("@addPaused="))) {
            torrentParams.addPaused = param.midRef(11).toInt() ? TriStateBool::True : TriStateBool::False;
            continue;
        }

        if (param == QLatin1String("@skipChecking")) {
            torrentParams.skipChecking = true;
            continue;
        }

        if (param.startsWith(QLatin1String("@category="))) {
            torrentParams.category = param.mid(10);
            continue;
        }

        if (param == QLatin1String("@sequential")) {
            torrentParams.sequential = true;
            continue;
        }

        if (param == QLatin1String("@firstLastPiecePriority")) {
            torrentParams.firstLastPiecePriority = true;
            continue;
        }

        if (param.startsWith(QLatin1String("@skipDialog="))) {
            skipTorrentDialog = param.midRef(12).toInt() ? TriStateBool::True : TriStateBool::False;
            continue;
        }

#ifndef DISABLE_GUI
        // There are two circumstances in which we want to show the torrent
        // dialog. One is when the application settings specify that it should
        // be shown and skipTorrentDialog is undefined. The other is when
        // skipTorrentDialog is false, meaning that the application setting
        // should be overridden.
        const bool showDialogForThisTorrent =
            ((AddNewTorrentDialog::isEnabled() && skipTorrentDialog == TriStateBool::Undefined)
             || skipTorrentDialog == TriStateBool::False);
        if (showDialogForThisTorrent)
            AddNewTorrentDialog::show(param, torrentParams, m_window);
        else
#endif
            BitTorrent::Session::instance()->addTorrent(param, torrentParams);
    }
}

int Application::exec(const QStringList &params)
{
    Net::ProxyConfigurationManager::initInstance();
    Net::DownloadManager::initInstance();
#ifdef DISABLE_GUI
    IconProvider::initInstance();
#else
    GuiIconProvider::initInstance();
#endif

    BitTorrent::Session::initInstance();
    connect(BitTorrent::Session::instance(), &BitTorrent::Session::torrentFinished, this, &Application::torrentFinished);
    connect(BitTorrent::Session::instance(), &BitTorrent::Session::allTorrentsFinished, this, &Application::allTorrentsFinished, Qt::QueuedConnection);

#ifndef DISABLE_COUNTRIES_RESOLUTION
    Net::GeoIPManager::initInstance();
#endif
    ScanFoldersModel::initInstance(this);

#ifndef DISABLE_WEBUI
    m_webui = new WebUI;
#ifdef DISABLE_GUI
    if (m_webui->isErrored())
        return 1;
    connect(m_webui, &WebUI::fatalError, this, []() { QCoreApplication::exit(1); });
#endif // DISABLE_GUI
#endif // DISABLE_WEBUI

    new RSS::Session; // create RSS::Session singleton
    new RSS::AutoDownloader; // create RSS::AutoDownloader singleton

#ifdef DISABLE_GUI
#ifndef DISABLE_WEBUI
    Preferences *const pref = Preferences::instance();
    // Display some information to the user
    const QString mesg = QString("\n******** %1 ********\n").arg(tr("Information"))
        + tr("To control qBittorrent, access the Web UI at %1")
            .arg(QString("http://localhost:") + QString::number(pref->getWebUiPort())) + '\n'
        + tr("The Web UI administrator user name is: %1").arg(pref->getWebUiUsername()) + '\n';
    printf("%s", qUtf8Printable(mesg));
    qDebug() << "Password:" << pref->getWebUiPassword();
    if (pref->getWebUiPassword() == "f6fdffe48c908deb0f4c3bd36c032e72") {
        const QString warning = tr("The Web UI administrator password is still the default one: %1").arg("adminadmin") + '\n'
            + tr("This is a security risk, please consider changing your password from program preferences.") + '\n';
        printf("%s", qUtf8Printable(warning));
    }
#endif // DISABLE_WEBUI
#else
    m_window = new MainWindow;
#endif // DISABLE_GUI

    m_running = true;

    // Now UI is ready to process signals from Session
    BitTorrent::Session::instance()->startUpTorrents();

    m_paramsQueue = params + m_paramsQueue;
    if (!m_paramsQueue.isEmpty()) {
        processParams(m_paramsQueue);
        m_paramsQueue.clear();
    }
    return BaseApplication::exec();
}

#ifndef DISABLE_GUI
#ifdef Q_OS_WIN
bool Application::isRunning()
{
    bool running = BaseApplication::isRunning();
    QSharedMemory *sharedMem = new QSharedMemory(id() + QLatin1String("-shared-memory-key"), this);
    if (!running) {
        // First instance creates shared memory and store PID
        if (sharedMem->create(sizeof(DWORD)) && sharedMem->lock()) {
            *(static_cast<DWORD*>(sharedMem->data())) = ::GetCurrentProcessId();
            sharedMem->unlock();
        }
    }
    else {
        // Later instances attach to shared memory and retrieve PID
        if (sharedMem->attach() && sharedMem->lock()) {
            ::AllowSetForegroundWindow(*(static_cast<DWORD*>(sharedMem->data())));
            sharedMem->unlock();
        }
    }

    if (!sharedMem->isAttached())
        qWarning() << "Failed to initialize shared memory: " << sharedMem->errorString();

    return running;
}
#endif // Q_OS_WIN

#ifdef Q_OS_MAC
bool Application::event(QEvent *ev)
{
    if (ev->type() == QEvent::FileOpen) {
        QString path = static_cast<QFileOpenEvent *>(ev)->file();
        if (path.isEmpty())
            // Get the url instead
            path = static_cast<QFileOpenEvent *>(ev)->url().toString();
        qDebug("Received a mac file open event: %s", qUtf8Printable(path));
        if (m_running)
            processParams(QStringList(path));
        else
            m_paramsQueue.append(path);
        return true;
    }
    else {
        return BaseApplication::event(ev);
    }
}
#endif // Q_OS_MAC

bool Application::notify(QObject *receiver, QEvent *event)
{
    try {
        return QApplication::notify(receiver, event);
    }
    catch (const std::exception &e) {
        qCritical() << "Exception thrown:" << e.what() << ", receiver: " << receiver->objectName();
        receiver->dumpObjectInfo();
    }

    return false;
}
#endif // DISABLE_GUI

void Application::initializeTranslation()
{
    Preferences *const pref = Preferences::instance();
    // Load translation
    QString localeStr = pref->getLocale();

    if (m_qtTranslator.load(QLatin1String("qtbase_") + localeStr, QLibraryInfo::location(QLibraryInfo::TranslationsPath)) ||
        m_qtTranslator.load(QLatin1String("qt_") + localeStr, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
            qDebug("Qt %s locale recognized, using translation.", qUtf8Printable(localeStr));
    else
        qDebug("Qt %s locale unrecognized, using default (en).", qUtf8Printable(localeStr));

    installTranslator(&m_qtTranslator);

    if (m_translator.load(QLatin1String(":/lang/qbittorrent_") + localeStr))
        qDebug("%s locale recognized, using translation.", qUtf8Printable(localeStr));
    else
        qDebug("%s locale unrecognized, using default (en).", qUtf8Printable(localeStr));
    installTranslator(&m_translator);

#ifndef DISABLE_GUI
    if (localeStr.startsWith("ar") || localeStr.startsWith("he")) {
        qDebug("Right to Left mode");
        setLayoutDirection(Qt::RightToLeft);
    }
    else {
        setLayoutDirection(Qt::LeftToRight);
    }
#endif
}

#if (!defined(DISABLE_GUI) && defined(Q_OS_WIN))
void Application::shutdownCleanup(QSessionManager &manager)
{
    Q_UNUSED(manager);

    // This is only needed for a special case on Windows XP.
    // (but is called for every Windows version)
    // If a process takes too much time to exit during OS
    // shutdown, the OS presents a dialog to the user.
    // That dialog tells the user that qbt is blocking the
    // shutdown, it shows a progress bar and it offers
    // a "Terminate Now" button for the user. However,
    // after the progress bar has reached 100% another button
    // is offered to the user reading "Cancel". With this the
    // user can cancel the **OS** shutdown. If we don't do
    // the cleanup by handling the commitDataRequest() signal
    // and the user clicks "Cancel", it will result in qbt being
    // killed and the shutdown proceeding instead. Apparently
    // aboutToQuit() is emitted too late in the shutdown process.
    cleanup();

    // According to the qt docs we shouldn't call quit() inside a slot.
    // aboutToQuit() is never emitted if the user hits "Cancel" in
    // the above dialog.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}
#endif

void Application::cleanup()
{
    // cleanup() can be called multiple times during shutdown. We only need it once.
    static QAtomicInt alreadyDone;
    if (!alreadyDone.testAndSetAcquire(0, 1))
        return;

#ifndef DISABLE_GUI
    if (m_window) {
        // Hide the window and don't leave it on screen as
        // unresponsive. Also for Windows take the WinId
        // after it's hidden, because hide() may cause a
        // WinId change.
        m_window->hide();

#ifdef Q_OS_WIN
        typedef BOOL (WINAPI *PSHUTDOWNBRCREATE)(HWND, LPCWSTR);
        const auto shutdownBRCreate = Utils::Misc::loadWinAPI<PSHUTDOWNBRCREATE>("User32.dll", "ShutdownBlockReasonCreate");
        // Only available on Vista+
        if (shutdownBRCreate)
            shutdownBRCreate((HWND)m_window->effectiveWinId(), tr("Saving torrent progress...").toStdWString().c_str());
#endif // Q_OS_WIN

        // Do manual cleanup in MainWindow to force widgets
        // to save their Preferences, stop all timers and
        // delete as many widgets as possible to leave only
        // a 'shell' MainWindow.
        // We need a valid window handle for Windows Vista+
        // otherwise the system shutdown will continue even
        // though we created a ShutdownBlockReason
        m_window->cleanup();
    }
#endif // DISABLE_GUI

#ifndef DISABLE_WEBUI
    delete m_webui;
#endif

    delete RSS::AutoDownloader::instance();
    delete RSS::Session::instance();

    ScanFoldersModel::freeInstance();
    BitTorrent::Session::freeInstance();
#ifndef DISABLE_COUNTRIES_RESOLUTION
    Net::GeoIPManager::freeInstance();
#endif
    Net::DownloadManager::freeInstance();
    Net::ProxyConfigurationManager::freeInstance();
    Preferences::freeInstance();
    SettingsStorage::freeInstance();
    delete m_fileLogger;
    Logger::freeInstance();
    IconProvider::freeInstance();
    Utils::Fs::removeDirRecursive(Utils::Fs::tempPath());

#ifndef DISABLE_GUI
    if (m_window) {
#ifdef Q_OS_WIN
        typedef BOOL (WINAPI *PSHUTDOWNBRDESTROY)(HWND);
        const auto shutdownBRDestroy = Utils::Misc::loadWinAPI<PSHUTDOWNBRDESTROY>("User32.dll", "ShutdownBlockReasonDestroy");
        // Only available on Vista+
        if (shutdownBRDestroy)
            shutdownBRDestroy((HWND)m_window->effectiveWinId());
#endif // Q_OS_WIN
        delete m_window;
    }
#endif // DISABLE_GUI

    if (m_shutdownAct != ShutdownDialogAction::Exit) {
        qDebug() << "Sending computer shutdown/suspend/hibernate signal...";
        Utils::Misc::shutdownComputer(m_shutdownAct);
    }
}

void Application::validateCommandLineParameters()
{
    if (m_commandLineArgs.portableMode && !m_commandLineArgs.profileDir.isEmpty())
        throw CommandLineParameterError(tr("Portable mode and explicit profile directory options are mutually exclusive"));

    if (m_commandLineArgs.portableMode && m_commandLineArgs.relativeFastresumePaths)
        Logger::instance()->addMessage(tr("Portable mode implies relative fastresume"), Log::WARNING);
}
