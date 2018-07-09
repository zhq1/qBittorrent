/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2017  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2010  Christophe Dumez <chris@qbittorrent.org>
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

#include "rss_autodownloadrule.h"

#include <QDebug>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSharedData>
#include <QString>
#include <QStringList>

#include "../global.h"
#include "../preferences.h"
#include "../tristatebool.h"
#include "../utils/fs.h"
#include "../utils/string.h"
#include "rss_article.h"
#include "rss_autodownloader.h"
#include "rss_feed.h"

namespace
{
    TriStateBool jsonValueToTriStateBool(const QJsonValue &jsonVal)
    {
        if (jsonVal.isBool())
            return TriStateBool(jsonVal.toBool());

        if (!jsonVal.isNull())
            qDebug() << Q_FUNC_INFO << "Incorrect value" << jsonVal.toVariant();

        return TriStateBool::Undefined;
    }

    QJsonValue triStateBoolToJsonValue(const TriStateBool &triStateBool)
    {
        switch (static_cast<int>(triStateBool)) {
        case 0:  return false;
        case 1:  return true;
        default: return QJsonValue();
        }
    }

    TriStateBool addPausedLegacyToTriStateBool(int val)
    {
        switch (val) {
        case 1:  return TriStateBool::True; // always
        case 2:  return TriStateBool::False; // never
        default: return TriStateBool::Undefined; // default
        }
    }

    int triStateBoolToAddPausedLegacy(const TriStateBool &triStateBool)
    {
        switch (static_cast<int>(triStateBool)) {
        case 0:  return 2; // never
        case 1:  return 1; // always
        default: return 0; // default
        }
    }
}

const QString Str_Name(QStringLiteral("name"));
const QString Str_Enabled(QStringLiteral("enabled"));
const QString Str_UseRegex(QStringLiteral("useRegex"));
const QString Str_MustContain(QStringLiteral("mustContain"));
const QString Str_MustNotContain(QStringLiteral("mustNotContain"));
const QString Str_EpisodeFilter(QStringLiteral("episodeFilter"));
const QString Str_AffectedFeeds(QStringLiteral("affectedFeeds"));
const QString Str_SavePath(QStringLiteral("savePath"));
const QString Str_AssignedCategory(QStringLiteral("assignedCategory"));
const QString Str_LastMatch(QStringLiteral("lastMatch"));
const QString Str_IgnoreDays(QStringLiteral("ignoreDays"));
const QString Str_AddPaused(QStringLiteral("addPaused"));
const QString Str_SmartFilter(QStringLiteral("smartFilter"));
const QString Str_PreviouslyMatched(QStringLiteral("previouslyMatchedEpisodes"));

namespace RSS
{
    struct AutoDownloadRuleData : public QSharedData
    {
        QString name;
        bool enabled = true;

        QStringList mustContain;
        QStringList mustNotContain;
        QString episodeFilter;
        QStringList feedURLs;
        bool useRegex = false;
        int ignoreDays = 0;
        QDateTime lastMatch;

        QString savePath;
        QString category;
        TriStateBool addPaused = TriStateBool::Undefined;

        bool smartFilter = false;
        QStringList previouslyMatchedEpisodes;

        mutable QString lastComputedEpisode;
        mutable QHash<QString, QRegularExpression> cachedRegexes;

        bool operator==(const AutoDownloadRuleData &other) const
        {
            return (name == other.name)
                    && (enabled == other.enabled)
                    && (mustContain == other.mustContain)
                    && (mustNotContain == other.mustNotContain)
                    && (episodeFilter == other.episodeFilter)
                    && (feedURLs == other.feedURLs)
                    && (useRegex == other.useRegex)
                    && (ignoreDays == other.ignoreDays)
                    && (lastMatch == other.lastMatch)
                    && (savePath == other.savePath)
                    && (category == other.category)
                    && (addPaused == other.addPaused)
                    && (smartFilter == other.smartFilter);
        }
    };
}

using namespace RSS;

QString computeEpisodeName(const QString &article)
{
    const QRegularExpression episodeRegex = AutoDownloader::instance()->smartEpisodeRegex();
    const QRegularExpressionMatch match = episodeRegex.match(article);

    // See if we can extract an season/episode number or date from the title
    if (!match.hasMatch())
        return QString();

    QStringList ret;
    for (int i = 1; i <= match.lastCapturedIndex(); ++i) {
        QString cap = match.captured(i);

        if (cap.isEmpty())
            continue;

        bool isInt = false;
        int x = cap.toInt(&isInt);

        ret.append(isInt ? QString::number(x) : cap);
    }
    return ret.join('x');
}

AutoDownloadRule::AutoDownloadRule(const QString &name)
    : m_dataPtr(new AutoDownloadRuleData)
{
    setName(name);
}

AutoDownloadRule::AutoDownloadRule(const AutoDownloadRule &other)
    : m_dataPtr(other.m_dataPtr)
{
}

AutoDownloadRule::~AutoDownloadRule() {}

QRegularExpression AutoDownloadRule::cachedRegex(const QString &expression, bool isRegex) const
{
    // Use a cache of regexes so we don't have to continually recompile - big performance increase.
    // The cache is cleared whenever the regex/wildcard, must or must not contain fields or
    // episode filter are modified.
    Q_ASSERT(!expression.isEmpty());

    QRegularExpression &regex = m_dataPtr->cachedRegexes[expression];
    if (regex.pattern().isEmpty()) {
        regex = QRegularExpression {
                (isRegex ? expression : Utils::String::wildcardToRegex(expression))
                , QRegularExpression::CaseInsensitiveOption};
    }

    return regex;
}

bool AutoDownloadRule::matchesExpression(const QString &articleTitle, const QString &expression) const
{
    const QRegularExpression whitespace {"\\s+"};

    if (expression.isEmpty()) {
        // A regex of the form "expr|" will always match, so do the same for wildcards
        return true;
    }

    if (m_dataPtr->useRegex) {
        QRegularExpression reg(cachedRegex(expression));
        return reg.match(articleTitle).hasMatch();
    }

    // Only match if every wildcard token (separated by spaces) is present in the article name.
    // Order of wildcard tokens is unimportant (if order is important, they should have used *).
    const QStringList wildcards {expression.split(whitespace, QString::SplitBehavior::SkipEmptyParts)};
    for (const QString &wildcard : wildcards) {
        const QRegularExpression reg {cachedRegex(wildcard, false)};
        if (!reg.match(articleTitle).hasMatch())
            return false;
    }

    return true;
}

bool AutoDownloadRule::matchesMustContainExpression(const QString &articleTitle) const
{
    if (m_dataPtr->mustContain.empty())
        return true;

    // Each expression is either a regex, or a set of wildcards separated by whitespace.
    // Accept if any complete expression matches.
    for (const QString &expression : qAsConst(m_dataPtr->mustContain)) {
        // A regex of the form "expr|" will always match, so do the same for wildcards
        if (matchesExpression(articleTitle, expression))
            return true;
    }

    return false;
}

bool AutoDownloadRule::matchesMustNotContainExpression(const QString& articleTitle) const
{
    if (m_dataPtr->mustNotContain.empty())
        return true;

    // Each expression is either a regex, or a set of wildcards separated by whitespace.
    // Reject if any complete expression matches.
    for (const QString &expression : qAsConst(m_dataPtr->mustNotContain)) {
        // A regex of the form "expr|" will always match, so do the same for wildcards
        if (matchesExpression(articleTitle, expression))
            return false;
    }

    return true;
}

bool AutoDownloadRule::matchesEpisodeFilterExpression(const QString& articleTitle) const
{
    // Reset the lastComputedEpisode, we don't want to leak it between matches
    m_dataPtr->lastComputedEpisode.clear();

    if (m_dataPtr->episodeFilter.isEmpty())
        return true;

    const QRegularExpression filterRegex {cachedRegex("(^\\d{1,4})x(.*;$)")};
    const QRegularExpressionMatch matcher {filterRegex.match(m_dataPtr->episodeFilter)};
    if (!matcher.hasMatch())
        return false;

    const QString season {matcher.captured(1)};
    const QStringList episodes {matcher.captured(2).split(';')};
    const int seasonOurs {season.toInt()};

    for (QString episode : episodes) {
        if (episode.isEmpty())
            continue;

        // We need to trim leading zeroes, but if it's all zeros then we want episode zero.
        while ((episode.size() > 1) && episode.startsWith('0'))
            episode = episode.right(episode.size() - 1);

        if (episode.indexOf('-') != -1) { // Range detected
            const QString partialPattern1 {"\\bs0?(\\d{1,4})[ -_\\.]?e(0?\\d{1,4})(?:\\D|\\b)"};
            const QString partialPattern2 {"\\b(\\d{1,4})x(0?\\d{1,4})(?:\\D|\\b)"};

            // Extract partial match from article and compare as digits
            QRegularExpressionMatch matcher = cachedRegex(partialPattern1).match(articleTitle);
            bool matched = matcher.hasMatch();

            if (!matched) {
                matcher = cachedRegex(partialPattern2).match(articleTitle);
                matched = matcher.hasMatch();
            }

            if (matched) {
                const int seasonTheirs {matcher.captured(1).toInt()};
                const int episodeTheirs {matcher.captured(2).toInt()};

                if (episode.endsWith('-')) { // Infinite range
                    const int episodeOurs {episode.leftRef(episode.size() - 1).toInt()};
                    if (((seasonTheirs == seasonOurs) && (episodeTheirs >= episodeOurs)) || (seasonTheirs > seasonOurs))
                        return true;
                }
                else { // Normal range
                    const QStringList range {episode.split('-')};
                    Q_ASSERT(range.size() == 2);
                    if (range.first().toInt() > range.last().toInt())
                        continue; // Ignore this subrule completely

                    const int episodeOursFirst {range.first().toInt()};
                    const int episodeOursLast {range.last().toInt()};
                    if ((seasonTheirs == seasonOurs) && ((episodeOursFirst <= episodeTheirs) && (episodeOursLast >= episodeTheirs)))
                        return true;
                }
            }
        }
        else { // Single number
            const QString expStr {QString("\\b(?:s0?%1[ -_\\.]?e0?%2|%1x0?%2)(?:\\D|\\b)").arg(season, episode)};
            if (cachedRegex(expStr).match(articleTitle).hasMatch())
                return true;
        }
    }

    return false;
}

bool AutoDownloadRule::matchesSmartEpisodeFilter(const QString& articleTitle) const
{
    if (!useSmartFilter())
        return true;

    const QString episodeStr = computeEpisodeName(articleTitle);
    if (episodeStr.isEmpty())
        return true;

    // See if this episode has been downloaded before
    const bool previouslyMatched = m_dataPtr->previouslyMatchedEpisodes.contains(episodeStr);
    const bool isRepack = articleTitle.contains("REPACK", Qt::CaseInsensitive) || articleTitle.contains("PROPER", Qt::CaseInsensitive);
    if (previouslyMatched && !isRepack)
        return false;

    m_dataPtr->lastComputedEpisode = episodeStr;
    return true;
}

bool AutoDownloadRule::matches(const QVariantHash &articleData) const
{
    const QDateTime articleDate {articleData[Article::KeyDate].toDateTime()};
    if (ignoreDays() > 0) {
        if (lastMatch().isValid() && (articleDate < lastMatch().addDays(ignoreDays())))
            return false;
    }

    const QString articleTitle {articleData[Article::KeyTitle].toString()};
    if (!matchesMustContainExpression(articleTitle))
        return false;
    if (!matchesMustNotContainExpression(articleTitle))
        return false;
    if (!matchesEpisodeFilterExpression(articleTitle))
        return false;
    if (!matchesSmartEpisodeFilter(articleTitle))
        return false;

    return true;
}

bool AutoDownloadRule::accepts(const QVariantHash &articleData)
{
    if (!matches(articleData))
        return false;

    setLastMatch(articleData[Article::KeyDate].toDateTime());

    if (!m_dataPtr->lastComputedEpisode.isEmpty()) {
        // TODO: probably need to add a marker for PROPER/REPACK to avoid duplicate downloads
        m_dataPtr->previouslyMatchedEpisodes.append(m_dataPtr->lastComputedEpisode);
        m_dataPtr->lastComputedEpisode.clear();
    }

    return true;
}

AutoDownloadRule &AutoDownloadRule::operator=(const AutoDownloadRule &other)
{
    m_dataPtr = other.m_dataPtr;
    return *this;
}

bool AutoDownloadRule::operator==(const AutoDownloadRule &other) const
{
    return (m_dataPtr == other.m_dataPtr) // optimization
            || (*m_dataPtr == *other.m_dataPtr);
}

bool AutoDownloadRule::operator!=(const AutoDownloadRule &other) const
{
    return !operator==(other);
}

QJsonObject AutoDownloadRule::toJsonObject() const
{
    return {{Str_Enabled, isEnabled()}
        , {Str_UseRegex, useRegex()}
        , {Str_MustContain, mustContain()}
        , {Str_MustNotContain, mustNotContain()}
        , {Str_EpisodeFilter, episodeFilter()}
        , {Str_AffectedFeeds, QJsonArray::fromStringList(feedURLs())}
        , {Str_SavePath, savePath()}
        , {Str_AssignedCategory, assignedCategory()}
        , {Str_LastMatch, lastMatch().toString(Qt::RFC2822Date)}
        , {Str_IgnoreDays, ignoreDays()}
        , {Str_AddPaused, triStateBoolToJsonValue(addPaused())}
        , {Str_SmartFilter, useSmartFilter()}
        , {Str_PreviouslyMatched, QJsonArray::fromStringList(previouslyMatchedEpisodes())}};
}

AutoDownloadRule AutoDownloadRule::fromJsonObject(const QJsonObject &jsonObj, const QString &name)
{
    AutoDownloadRule rule(name.isEmpty() ? jsonObj.value(Str_Name).toString() : name);

    rule.setUseRegex(jsonObj.value(Str_UseRegex).toBool(false));
    rule.setMustContain(jsonObj.value(Str_MustContain).toString());
    rule.setMustNotContain(jsonObj.value(Str_MustNotContain).toString());
    rule.setEpisodeFilter(jsonObj.value(Str_EpisodeFilter).toString());
    rule.setEnabled(jsonObj.value(Str_Enabled).toBool(true));
    rule.setSavePath(jsonObj.value(Str_SavePath).toString());
    rule.setCategory(jsonObj.value(Str_AssignedCategory).toString());
    rule.setAddPaused(jsonValueToTriStateBool(jsonObj.value(Str_AddPaused)));
    rule.setLastMatch(QDateTime::fromString(jsonObj.value(Str_LastMatch).toString(), Qt::RFC2822Date));
    rule.setIgnoreDays(jsonObj.value(Str_IgnoreDays).toInt());
    rule.setUseSmartFilter(jsonObj.value(Str_SmartFilter).toBool(false));

    const QJsonValue feedsVal = jsonObj.value(Str_AffectedFeeds);
    QStringList feedURLs;
    if (feedsVal.isString())
        feedURLs << feedsVal.toString();
    else foreach (const QJsonValue &urlVal, feedsVal.toArray())
        feedURLs << urlVal.toString();
    rule.setFeedURLs(feedURLs);

    const QJsonValue previouslyMatchedVal = jsonObj.value(Str_PreviouslyMatched);
    QStringList previouslyMatched;
    if (previouslyMatchedVal.isString()) {
        previouslyMatched << previouslyMatchedVal.toString();
    }
    else {
        foreach (const QJsonValue &val, previouslyMatchedVal.toArray())
            previouslyMatched << val.toString();
    }
    rule.setPreviouslyMatchedEpisodes(previouslyMatched);

    return rule;
}

QVariantHash AutoDownloadRule::toLegacyDict() const
{
    return {{"name", name()},
        {"must_contain", mustContain()},
        {"must_not_contain", mustNotContain()},
        {"save_path", savePath()},
        {"affected_feeds", feedURLs()},
        {"enabled", isEnabled()},
        {"category_assigned", assignedCategory()},
        {"use_regex", useRegex()},
        {"add_paused", triStateBoolToAddPausedLegacy(addPaused())},
        {"episode_filter", episodeFilter()},
        {"last_match", lastMatch()},
        {"ignore_days", ignoreDays()}};
}

AutoDownloadRule AutoDownloadRule::fromLegacyDict(const QVariantHash &dict)
{
    AutoDownloadRule rule(dict.value("name").toString());

    rule.setUseRegex(dict.value("use_regex", false).toBool());
    rule.setMustContain(dict.value("must_contain").toString());
    rule.setMustNotContain(dict.value("must_not_contain").toString());
    rule.setEpisodeFilter(dict.value("episode_filter").toString());
    rule.setFeedURLs(dict.value("affected_feeds").toStringList());
    rule.setEnabled(dict.value("enabled", false).toBool());
    rule.setSavePath(dict.value("save_path").toString());
    rule.setCategory(dict.value("category_assigned").toString());
    rule.setAddPaused(addPausedLegacyToTriStateBool(dict.value("add_paused").toInt()));
    rule.setLastMatch(dict.value("last_match").toDateTime());
    rule.setIgnoreDays(dict.value("ignore_days").toInt());

    return rule;
}

void AutoDownloadRule::setMustContain(const QString &tokens)
{
    m_dataPtr->cachedRegexes.clear();

    if (m_dataPtr->useRegex)
        m_dataPtr->mustContain = QStringList() << tokens;
    else
        m_dataPtr->mustContain = tokens.split("|");

    // Check for single empty string - if so, no condition
    if ((m_dataPtr->mustContain.size() == 1) && m_dataPtr->mustContain[0].isEmpty())
        m_dataPtr->mustContain.clear();
}

void AutoDownloadRule::setMustNotContain(const QString &tokens)
{
    m_dataPtr->cachedRegexes.clear();

    if (m_dataPtr->useRegex)
        m_dataPtr->mustNotContain = QStringList() << tokens;
    else
        m_dataPtr->mustNotContain = tokens.split("|");

    // Check for single empty string - if so, no condition
    if ((m_dataPtr->mustNotContain.size() == 1) && m_dataPtr->mustNotContain[0].isEmpty())
        m_dataPtr->mustNotContain.clear();
}

QStringList AutoDownloadRule::feedURLs() const
{
    return m_dataPtr->feedURLs;
}

void AutoDownloadRule::setFeedURLs(const QStringList &urls)
{
    m_dataPtr->feedURLs = urls;
}

QString AutoDownloadRule::name() const
{
    return m_dataPtr->name;
}

void AutoDownloadRule::setName(const QString &name)
{
    m_dataPtr->name = name;
}

QString AutoDownloadRule::savePath() const
{
    return m_dataPtr->savePath;
}

void AutoDownloadRule::setSavePath(const QString &savePath)
{
    m_dataPtr->savePath = Utils::Fs::fromNativePath(savePath);
}

TriStateBool AutoDownloadRule::addPaused() const
{
    return m_dataPtr->addPaused;
}

void AutoDownloadRule::setAddPaused(const TriStateBool &addPaused)
{
    m_dataPtr->addPaused = addPaused;
}

QString AutoDownloadRule::assignedCategory() const
{
    return m_dataPtr->category;
}

void AutoDownloadRule::setCategory(const QString &category)
{
    m_dataPtr->category = category;
}

bool AutoDownloadRule::isEnabled() const
{
    return m_dataPtr->enabled;
}

void AutoDownloadRule::setEnabled(bool enable)
{
    m_dataPtr->enabled = enable;
}

QDateTime AutoDownloadRule::lastMatch() const
{
    return m_dataPtr->lastMatch;
}

void AutoDownloadRule::setLastMatch(const QDateTime &lastMatch)
{
    m_dataPtr->lastMatch = lastMatch;
}

void AutoDownloadRule::setIgnoreDays(int d)
{
    m_dataPtr->ignoreDays = d;
}

int AutoDownloadRule::ignoreDays() const
{
    return m_dataPtr->ignoreDays;
}

QString AutoDownloadRule::mustContain() const
{
    return m_dataPtr->mustContain.join("|");
}

QString AutoDownloadRule::mustNotContain() const
{
    return m_dataPtr->mustNotContain.join("|");
}

bool AutoDownloadRule::useSmartFilter() const
{
    return m_dataPtr->smartFilter;
}

void AutoDownloadRule::setUseSmartFilter(bool enabled)
{
    m_dataPtr->smartFilter = enabled;
}

bool AutoDownloadRule::useRegex() const
{
    return m_dataPtr->useRegex;
}

void AutoDownloadRule::setUseRegex(bool enabled)
{
    m_dataPtr->useRegex = enabled;
    m_dataPtr->cachedRegexes.clear();
}

QStringList AutoDownloadRule::previouslyMatchedEpisodes() const
{
    return m_dataPtr->previouslyMatchedEpisodes;
}

void AutoDownloadRule::setPreviouslyMatchedEpisodes(const QStringList &previouslyMatchedEpisodes)
{
    m_dataPtr->previouslyMatchedEpisodes = previouslyMatchedEpisodes;
}

QString AutoDownloadRule::episodeFilter() const
{
    return m_dataPtr->episodeFilter;
}

void AutoDownloadRule::setEpisodeFilter(const QString &e)
{
    m_dataPtr->episodeFilter = e;
    m_dataPtr->cachedRegexes.clear();
}
