#include "searchpanel.h"

#include "delegates.h"
#include "settings.h"
#include "vscodeicons.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QProcess>
#include <QShortcut>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace Lieve;

namespace
{
QToolButton *toggle(const QString &text, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setFixedWidth(28);
    return b;
}

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sideTitle"));
    QFont f = label->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 0.85);
    label->setFont(f);
    return label;
}

// Files sorted by path (like VS Code), matches by line.
class ResultItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        if (data(0, KindRole).toInt() == FileItem) {
            return data(0, PathRole).toString().compare(other.data(0, PathRole).toString(), Qt::CaseInsensitive) < 0;
        }
        return data(0, LineRole).toInt() < other.data(0, LineRole).toInt();
    }
};
}

SearchPanel::SearchPanel(QWidget *parent)
    : QWidget(parent)
{
    m_maxResults = qMax(100, config().value(QStringLiteral("search/maxResults"), 5000).toInt());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 0);
    layout->setSpacing(6);
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(4, 0, 0, 0);
    titleRow->addWidget(sectionTitle(tr("SEARCH"), this));
    titleRow->addStretch(1);
    m_more = new QToolButton(this);
    m_more->setText(QStringLiteral("⋯"));
    m_more->setToolTip(tr("Toggle Search Details"));
    m_more->setCheckable(true);
    m_more->setAutoRaise(true);
    titleRow->addWidget(m_more);
    layout->addLayout(titleRow);

    auto *queryRow = new QHBoxLayout;
    queryRow->setSpacing(2);
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(tr("Search"));
    m_query->setClearButtonEnabled(true);
    m_case = toggle(QStringLiteral("Aa"), tr("Match Case"), this);
    m_word = toggle(QStringLiteral("ab"), tr("Match Whole Word"), this);
    QFont underlined = m_word->font();
    underlined.setUnderline(true);
    m_word->setFont(underlined);
    m_regex = toggle(QStringLiteral(".*"), tr("Use Regular Expression"), this);
    queryRow->addWidget(m_query, 1);
    queryRow->addWidget(m_case);
    queryRow->addWidget(m_word);
    queryRow->addWidget(m_regex);
    layout->addLayout(queryRow);

    m_filters = new QWidget(this);
    auto *fl = new QVBoxLayout(m_filters);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(4);
    m_include = new QLineEdit(m_filters);
    m_include->setPlaceholderText(tr("Files to include (e.g. src, *.ts, src/**/*.html)"));
    m_exclude = new QLineEdit(m_filters);
    m_exclude->setPlaceholderText(tr("Files to exclude (e.g. *.spec.ts, dist)"));
    fl->addWidget(m_include);
    fl->addWidget(m_exclude);
    m_filters->hide();
    layout->addWidget(m_filters);

    m_status = new QLabel(this);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setContentsMargins(4, 0, 0, 0);
    layout->addWidget(m_status);

    m_results = new QTreeWidget(this);
    m_results->setHeaderHidden(true);
    m_results->setColumnCount(1);
    m_results->setUniformRowHeights(true);
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setIndentation(14);
    m_results->setIconSize(QSize(16, 16));
    m_results->setItemDelegate(new FileItemDelegate(m_results));
    m_results->setExpandsOnDoubleClick(false);
    layout->addWidget(m_results, 1);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &SearchPanel::startSearch);
    connect(m_query, &QLineEdit::textChanged, this, [this] {
        m_debounce.start();
    });
    connect(m_query, &QLineEdit::returnPressed, this, &SearchPanel::startSearch);
    for (QLineEdit *edit : {m_include, m_exclude}) {
        connect(edit, &QLineEdit::textChanged, this, [this] {
            m_debounce.start();
        });
    }
    for (QToolButton *b : {m_case, m_word, m_regex}) {
        connect(b, &QToolButton::toggled, this, &SearchPanel::startSearch);
    }
    connect(m_more, &QToolButton::toggled, m_filters, &QWidget::setVisible);
    connect(m_results, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        activateItem(item);
    });
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *sc = new QShortcut(QKeySequence(key), m_results, nullptr, nullptr, Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this] {
            activateItem(m_results->currentItem());
        });
    }
    // Down arrow from the query box jumps into the results.
    auto *down = new QShortcut(QKeySequence(Qt::Key_Down), m_query, nullptr, nullptr, Qt::WidgetShortcut);
    connect(down, &QShortcut::activated, this, [this] {
        if (m_results->topLevelItemCount() > 0) {
            m_results->setFocus();
            if (!m_results->currentItem()) {
                m_results->setCurrentItem(m_results->topLevelItem(0));
            }
        }
    });
}

void SearchPanel::setRoot(const QString &dir)
{
    m_root = dir;
    stopProcess();
    m_results->clear();
    m_fileItems.clear();
    m_status->clear();
    if (!m_query->text().isEmpty()) {
        m_debounce.start();
    }
}

void SearchPanel::focusQuery(const QString &text)
{
    if (!text.isEmpty() && !text.contains(QLatin1Char('\n'))) {
        m_query->setText(text);
    }
    m_query->setFocus();
    m_query->selectAll();
}

QStringList SearchPanel::globs(const QString &text, bool exclude) const
{
    QStringList out;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString p : parts) {
        p = p.trimmed();
        if (p.startsWith(QLatin1String("./"))) {
            p = p.mid(2);
        }
        if (p.isEmpty()) {
            continue;
        }
        const QString prefix = exclude ? QStringLiteral("!") : QString();
        out << QStringLiteral("--glob") << prefix + p;
        // "src" or "src/app" also means everything inside that folder.
        const bool hasWildcard = p.contains(QLatin1Char('*')) || p.contains(QLatin1Char('?')) || p.contains(QLatin1Char('['));
        if (!hasWildcard) {
            out << QStringLiteral("--glob") << prefix + p + QStringLiteral("/**");
        }
    }
    return out;
}

void SearchPanel::stopProcess()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_buffer.clear();
}

void SearchPanel::startSearch()
{
    m_debounce.stop();
    stopProcess();
    m_results->clear();
    m_fileItems.clear();
    m_matchCount = 0;
    m_truncated = false;

    const QString query = m_query->text();
    if (query.isEmpty() || m_root.isEmpty()) {
        m_status->clear();
        return;
    }

    QStringList args{QStringLiteral("--json"), QStringLiteral("--hidden"), QStringLiteral("--no-messages")};
    args << (m_case->isChecked() ? QStringLiteral("--case-sensitive") : QStringLiteral("--ignore-case"));
    if (m_word->isChecked()) {
        args << QStringLiteral("--word-regexp");
    }
    if (!m_regex->isChecked()) {
        args << QStringLiteral("--fixed-strings");
    }
    args << QStringLiteral("--glob") << QStringLiteral("!.git");
    const QStringList configured = config()
                                       .value(QStringLiteral("search/exclude"),
                                              QStringList{QStringLiteral("**/node_modules"), QStringLiteral("**/bower_components")})
                                       .toStringList();
    args << globs(configured.join(QLatin1Char(',')), true);
    args << globs(m_exclude->text(), true);
    args << globs(m_include->text(), false);
    args << QStringLiteral("--regexp") << query << QStringLiteral("--") << QStringLiteral(".");

    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(m_root);
    m_proc->setStandardInputFile(QProcess::nullDevice());
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &SearchPanel::readOutput);
    connect(m_proc, &QProcess::finished, this, [this](int exitCode) {
        finished(exitCode);
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_status->setText(tr("ripgrep (rg) not found: install it with “pacman -S ripgrep”."));
        }
    });
    m_status->setText(tr("Searching…"));
    m_proc->start(QStringLiteral("rg"), args);
}

void SearchPanel::readOutput()
{
    if (!m_proc) {
        return;
    }
    m_buffer += m_proc->readAllStandardOutput();
    qsizetype start = 0;
    qsizetype newline = 0;
    m_results->setUpdatesEnabled(false);
    while ((newline = m_buffer.indexOf('\n', start)) >= 0) {
        const QByteArray line = m_buffer.mid(start, newline - start);
        start = newline + 1;
        if (!line.startsWith("{\"type\":\"match\"")) {
            continue;
        }
        addMatch(QJsonDocument::fromJson(line).object().value(QLatin1String("data")).toObject());
        if (m_matchCount >= m_maxResults) {
            m_truncated = true;
            break;
        }
    }
    m_buffer.remove(0, start);
    m_results->setUpdatesEnabled(true);

    if (m_truncated) {
        QProcess *proc = m_proc;
        proc->disconnect(this);
        proc->kill();
        proc->deleteLater();
        m_proc = nullptr;
        m_buffer.clear();
        finished(0);
    }
}

void SearchPanel::addMatch(const QJsonObject &data)
{
    QString rel = data.value(QLatin1String("path")).toObject().value(QLatin1String("text")).toString();
    if (rel.startsWith(QLatin1String("./"))) {
        rel = rel.mid(2);
    }
    QString text = data.value(QLatin1String("lines")).toObject().value(QLatin1String("text")).toString();
    if (rel.isEmpty() || text.isEmpty()) {
        return; // non UTF-8 path or binary line
    }
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r'))) {
        text.chop(1);
    }
    const int line = data.value(QLatin1String("line_number")).toInt() - 1;

    // ripgrep gives byte offsets; convert them to character positions.
    const QByteArray bytes = text.toUtf8();
    QList<QPoint> ranges;
    const QJsonArray submatches = data.value(QLatin1String("submatches")).toArray();
    for (const QJsonValue &sv : submatches) {
        const QJsonObject sm = sv.toObject();
        const int startByte = sm.value(QLatin1String("start")).toInt();
        const int endByte = sm.value(QLatin1String("end")).toInt();
        const int startChar = QString::fromUtf8(bytes.left(startByte)).size();
        const int endChar = QString::fromUtf8(bytes.left(endByte)).size();
        ranges << QPoint(startChar, endChar - startChar);
    }
    if (ranges.isEmpty()) {
        return;
    }

    QTreeWidgetItem *fileItem = m_fileItems.value(rel);
    if (!fileItem) {
        const QFileInfo fi(rel);
        fileItem = new ResultItem(m_results);
        fileItem->setText(0, fi.fileName());
        fileItem->setIcon(0, VsCodeIcons::instance().fileIcon(fi.fileName()));
        fileItem->setToolTip(0, rel);
        fileItem->setData(0, KindRole, FileItem);
        fileItem->setData(0, PathRole, QDir(m_root).filePath(rel));
        fileItem->setData(0, DirRole, fi.path() == QLatin1String(".") ? QString() : fi.path());
        fileItem->setExpanded(true);
        m_fileItems.insert(rel, fileItem);
    }

    // Shown text: no indentation, and long lines start a bit before the match.
    int shift = 0;
    while (shift < text.size() && text.at(shift).isSpace()) {
        ++shift;
    }
    QString shown = text.mid(shift);
    // The side bar is narrow: keep only a few characters before the match.
    const int firstStart = ranges.first().x() - shift;
    if (firstStart > 14) {
        const int cut = firstStart - 10;
        shown = QStringLiteral("…") + shown.mid(cut);
        shift += cut - 1;
    }
    if (shown.size() > 300) {
        shown.truncate(300);
    }
    shown.replace(QLatin1Char('\t'), QLatin1Char(' '));
    QVariantList shownRanges;
    for (const QPoint &r : std::as_const(ranges)) {
        shownRanges << QPoint(r.x() - shift, r.y());
    }

    auto *item = new ResultItem(fileItem);
    item->setText(0, shown);
    item->setToolTip(0, QStringLiteral("%1:%2").arg(rel).arg(line + 1));
    item->setData(0, KindRole, MatchItem);
    item->setData(0, PathRole, fileItem->data(0, PathRole));
    item->setData(0, LineRole, line);
    item->setData(0, ColumnRole, ranges.first().x());
    item->setData(0, LengthRole, ranges.first().y());
    item->setData(0, RangesRole, shownRanges);
    fileItem->setData(0, CountRole, fileItem->childCount());
    m_matchCount += int(ranges.size());
}

void SearchPanel::finished(int exitCode)
{
    QString error;
    if (m_proc) {
        readOutput(); // drain what is left (may hit the limit and stop the process)
        if (m_proc) {
            if (exitCode == 2) {
                error = QString::fromUtf8(m_proc->readAllStandardError()).trimmed();
            }
            m_proc->deleteLater();
            m_proc = nullptr;
        }
    }
    if (m_matchCount == 0) {
        // Exit code 2 with no results usually means an invalid regular expression.
        m_status->setText(error.isEmpty() ? tr("No results found.") : error.section(QLatin1Char('\n'), 0, 2));
        return;
    }
    m_results->sortItems(0, Qt::AscendingOrder);
    const int files = int(m_fileItems.size());
    QString status;
    if (m_truncated) {
        status = files == 1 ? tr("%1 results in 1 file (limit reached, narrow your search)").arg(m_matchCount)
                            : tr("%1 results in %2 files (limit reached, narrow your search)").arg(m_matchCount).arg(files);
    } else if (m_matchCount == 1) {
        status = tr("1 result in 1 file");
    } else if (files == 1) {
        status = tr("%1 results in 1 file").arg(m_matchCount);
    } else {
        status = tr("%1 results in %2 files").arg(m_matchCount).arg(files);
    }
    m_status->setText(status);
}

void SearchPanel::activateItem(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    if (item->data(0, KindRole).toInt() == FileItem) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    Q_EMIT matchActivated(item->data(0, PathRole).toString(),
                          item->data(0, LineRole).toInt(),
                          item->data(0, ColumnRole).toInt(),
                          item->data(0, LengthRole).toInt());
}
