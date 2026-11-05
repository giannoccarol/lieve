#include "searchpanel.h"

#include "delegates.h"
#include "settings.h"
#include "vscodeicons.h"

#include <KTextEditor/Document>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QShortcut>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

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

enum SearchRole {
    LineTextRole = Qt::UserRole + 40, // match rows: the whole line as ripgrep read it, without '\n'
    MatchRangesRole, // match rows: QVariantList of QPoint(start, length) in that line
    RelPathRole, // file rows: path relative to the root
};

enum RowAction {
    ReplaceAction,
    DismissAction,
};

// Thin line icon on a 16×16 grid (like Theme::glyph); `filled` is tinted.
QIcon lineIcon(const QPainterPath &path, const QColor &color, const QPainterPath &filled = QPainterPath())
{
    QIcon icon;
    for (const qreal dpr : {1.0, 2.0}) {
        QPixmap pixmap(QSize(16, 16) * dpr);
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        if (!filled.isEmpty()) {
            QColor tint = color;
            tint.setAlphaF(0.35f);
            p.fillPath(filled, tint);
        }
        p.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon chevronIcon(bool open, const QColor &color)
{
    QPainterPath path;
    if (open) {
        path.moveTo(4, 6);
        path.lineTo(8, 10);
        path.lineTo(12, 6);
    } else {
        path.moveTo(6, 4);
        path.lineTo(10, 8);
        path.lineTo(6, 12);
    }
    return lineIcon(path, color);
}

// A box, an arrow down and right to a filled box (a stack of them: Replace All).
QIcon replaceIcon(bool all, const QColor &color)
{
    QPainterPath path;
    path.addRoundedRect(QRectF(1.5, 2.5, 6, 4), 1, 1);
    path.moveTo(4.5, 7.5);
    path.lineTo(4.5, 10.5);
    path.lineTo(7.5, 10.5);
    path.moveTo(6, 9);
    path.lineTo(7.5, 10.5);
    path.lineTo(6, 12);
    QPainterPath target;
    target.addRoundedRect(QRectF(8.5, 8.5, 5, 4), 1, 1);
    path.addPath(target);
    if (all) {
        path.moveTo(10, 8.5);
        path.lineTo(10, 7);
        path.lineTo(15, 7);
        path.lineTo(15, 11);
        path.lineTo(13.5, 11);
    }
    return lineIcon(path, color, target);
}

QIcon dismissIcon(const QColor &color)
{
    QPainterPath path;
    path.moveTo(4.5, 4.5);
    path.lineTo(11.5, 11.5);
    path.moveTo(11.5, 4.5);
    path.lineTo(4.5, 11.5);
    return lineIcon(path, color);
}

bool isAsciiDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

// The replace text of a regular expression search, as in VS Code: $1..$99,
// $& or $0 (the whole match), $$, \n, \t, \\, and \u \l \U \L to change the
// case of the next group.
QString expandReplacement(const QString &pattern, const QRegularExpressionMatch &match, int groups)
{
    QString out;
    QChar caseChange;
    const auto addGroup = [&](int n) {
        QString text = match.captured(n);
        switch (caseChange.unicode()) {
        case 'u':
            text = text.left(1).toUpper() + text.mid(1);
            break;
        case 'l':
            text = text.left(1).toLower() + text.mid(1);
            break;
        case 'U':
            text = text.toUpper();
            break;
        case 'L':
            text = text.toLower();
            break;
        }
        caseChange = QChar();
        out += text;
    };
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        const QChar next = i + 1 < pattern.size() ? pattern.at(i + 1) : QChar();
        if (c == QLatin1Char('$')) {
            if (next == QLatin1Char('$')) {
                out += c;
                ++i;
                continue;
            }
            if (next == QLatin1Char('&')) {
                addGroup(0);
                ++i;
                continue;
            }
            if (isAsciiDigit(next)) {
                int n = next.digitValue();
                int digits = 1;
                const QChar third = i + 2 < pattern.size() ? pattern.at(i + 2) : QChar();
                if (isAsciiDigit(third) && n * 10 + third.digitValue() <= groups) {
                    n = n * 10 + third.digitValue();
                    digits = 2;
                }
                if (n <= groups) {
                    addGroup(n);
                    i += digits;
                    continue;
                }
            }
        } else if (c == QLatin1Char('\\')) {
            switch (next.unicode()) {
            case 'n':
                out += QLatin1Char('\n');
                ++i;
                continue;
            case 't':
                out += QLatin1Char('\t');
                ++i;
                continue;
            case '\\':
                out += QLatin1Char('\\');
                ++i;
                continue;
            case 'u':
            case 'l':
            case 'U':
            case 'L':
                caseChange = next;
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// Result rows with hover buttons (Replace, Dismiss) and the replace preview.
class SearchDelegate : public FileItemDelegate
{
public:
    using FileItemDelegate::FileItemDelegate;
    std::function<QStringList(const QModelIndex &)> previewFor;
    std::function<QList<int>(const QModelIndex &)> actionsFor;
    std::function<void(const QModelIndex &, int)> onAction;
    QIcon replaceIcon;
    QIcon replaceAllIcon;
    QIcon dismissIcon;
    bool actionClicked = false; // the last click hit a button, not the row

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        FileItemDelegate::paint(p, option, index);
        if (option.state & QStyle::State_MouseOver) {
            const bool file = index.data(KindRole).toInt() == FileItem;
            for (const auto &[action, rect] : buttons(option, index)) {
                const QIcon &icon = action == DismissAction ? dismissIcon : file ? replaceAllIcon : replaceIcon;
                icon.paint(p, rect.adjusted(2, 2, -2, -2));
            }
        }
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            for (const auto &[action, rect] : buttons(option, index)) {
                if (rect.contains(mouse->position().toPoint())) {
                    if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton && onAction) {
                        actionClicked = true;
                        onAction(index, action);
                    }
                    return true;
                }
            }
        }
        return FileItemDelegate::editorEvent(event, model, option, index);
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const bool file = index.data(KindRole).toInt() == FileItem;
        for (const auto &[action, rect] : buttons(option, index)) {
            if (rect.contains(event->pos())) {
                const QString tip = action == DismissAction ? QCoreApplication::translate("SearchPanel", "Dismiss")
                    : file                                  ? QCoreApplication::translate("SearchPanel", "Replace All in File")
                                                            : QCoreApplication::translate("SearchPanel", "Replace");
                QToolTip::showText(event->globalPos(), tip, view);
                return true;
            }
        }
        return FileItemDelegate::helpEvent(event, view, option, index);
    }

protected:
    QStringList replacements(const QModelIndex &index) const override
    {
        return previewFor ? previewFor(index) : QStringList();
    }
    int buttonsWidth(const QModelIndex &index) const override
    {
        return actionsFor ? int(actionsFor(index).size()) * 22 : 0;
    }

private:
    QList<QPair<int, QRect>> buttons(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        QList<QPair<int, QRect>> out;
        const QRect area = buttonsRect(option, index);
        if (area.isNull()) {
            return out;
        }
        int x = area.left() + 1;
        for (const int action : actionsFor(index)) {
            out.append({action, QRect(x, area.center().y() - 10, 20, 20)});
            x += 22;
        }
        return out;
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

    // [>] [search........] Aa ab .*
    //     [replace.......] [Replace All]     (the chevron shows the second row)
    auto *queryRows = new QGridLayout;
    queryRows->setHorizontalSpacing(2);
    queryRows->setVerticalSpacing(4);
    m_toggleReplace = new QToolButton(this);
    m_toggleReplace->setToolTip(tr("Toggle Replace"));
    m_toggleReplace->setAutoRaise(true);
    m_toggleReplace->setFixedWidth(20);
    m_toggleReplace->setIconSize(QSize(16, 16));
    m_toggleReplace->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(tr("Search"));
    m_query->setClearButtonEnabled(true);
    m_case = toggle(QStringLiteral("Aa"), tr("Match Case"), this);
    m_word = toggle(QStringLiteral("ab"), tr("Match Whole Word"), this);
    QFont underlined = m_word->font();
    underlined.setUnderline(true);
    m_word->setFont(underlined);
    m_regex = toggle(QStringLiteral(".*"), tr("Use Regular Expression"), this);
    m_replace = new QLineEdit(this);
    m_replace->setObjectName(QStringLiteral("replaceInput"));
    m_replace->setPlaceholderText(tr("Replace"));
    m_replace->setClearButtonEnabled(true);
    m_replaceAll = new QToolButton(this);
    m_replaceAll->setObjectName(QStringLiteral("replaceAllButton"));
    m_replaceAll->setToolTip(tr("Replace All (Ctrl+Alt+Enter)"));
    m_replaceAll->setAutoRaise(true);
    m_replaceAll->setFixedWidth(28);
    QColor iconColor = palette().color(QPalette::Text);
    iconColor.setAlphaF(0.85f);
    m_replaceAll->setIcon(replaceIcon(true, iconColor));
    queryRows->addWidget(m_toggleReplace, 0, 0, 2, 1);
    queryRows->addWidget(m_query, 0, 1);
    queryRows->addWidget(m_case, 0, 2);
    queryRows->addWidget(m_word, 0, 3);
    queryRows->addWidget(m_regex, 0, 4);
    queryRows->addWidget(m_replace, 1, 1);
    queryRows->addWidget(m_replaceAll, 1, 2);
    queryRows->setColumnStretch(1, 1);
    layout->addLayout(queryRows);
    setTabOrder(m_query, m_replace);

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
    m_status->setObjectName(QStringLiteral("searchStatus"));
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setContentsMargins(4, 0, 0, 0);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_results = new QTreeWidget(this);
    m_results->setHeaderHidden(true);
    m_results->setColumnCount(1);
    m_results->setUniformRowHeights(true);
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setIndentation(14);
    m_results->setIconSize(QSize(16, 16));
    m_results->setMouseTracking(true); // hover buttons
    auto *delegate = new SearchDelegate(m_results);
    delegate->replaceIcon = replaceIcon(false, iconColor);
    delegate->replaceAllIcon = replaceIcon(true, iconColor);
    delegate->dismissIcon = dismissIcon(iconColor);
    delegate->previewFor = [this](const QModelIndex &index) {
        return preview(index);
    };
    delegate->actionsFor = [this](const QModelIndex &) {
        return replacing() && canReplace() ? QList<int>{ReplaceAction, DismissAction} : QList<int>{DismissAction};
    };
    // Later: the action may delete the row the view is still handling.
    delegate->onAction = [this](const QModelIndex &index, int action) {
        const QPersistentModelIndex row(index);
        QTimer::singleShot(0, this, [this, row, action] {
            if (row.isValid()) {
                runRowAction(m_results->itemFromIndex(row), action);
            }
        });
    };
    m_results->setItemDelegate(delegate);
    m_results->setExpandsOnDoubleClick(false);
    layout->addWidget(m_results, 1);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &SearchPanel::startSearch);
    connect(m_query, &QLineEdit::textChanged, this, [this] {
        m_debounce.start();
        updateReplaceAll();
    });
    connect(m_query, &QLineEdit::returnPressed, this, &SearchPanel::startSearch);
    for (QLineEdit *edit : {m_include, m_exclude}) {
        connect(edit, &QLineEdit::textChanged, this, [this] {
            m_debounce.start();
            updateReplaceAll();
        });
    }
    for (QToolButton *b : {m_case, m_word, m_regex}) {
        connect(b, &QToolButton::toggled, this, &SearchPanel::startSearch);
    }
    connect(m_more, &QToolButton::toggled, m_filters, &QWidget::setVisible);
    connect(m_toggleReplace, &QToolButton::clicked, this, [this] {
        setReplaceVisible(!replacing());
        (replacing() ? m_replace : m_query)->setFocus();
    });
    connect(m_replace, &QLineEdit::textChanged, m_results->viewport(), qOverload<>(&QWidget::update)); // the preview
    connect(m_replace, &QLineEdit::returnPressed, this, &SearchPanel::startSearch);
    connect(m_replaceAll, &QToolButton::clicked, this, &SearchPanel::replaceAll);
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *all = new QShortcut(QKeySequence(Qt::CTRL | Qt::ALT | key), this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
        connect(all, &QShortcut::activated, this, &SearchPanel::replaceAll);
    }
    connect(m_results, &QTreeWidget::itemClicked, this, [this, delegate](QTreeWidgetItem *item) {
        if (delegate->actionClicked) {
            delegate->actionClicked = false;
            return;
        }
        activateItem(item);
    });
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *sc = new QShortcut(QKeySequence(key), m_results, nullptr, nullptr, Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this] {
            activateItem(m_results->currentItem());
        });
    }
    auto *dismiss = new QShortcut(QKeySequence(Qt::Key_Delete), m_results, nullptr, nullptr, Qt::WidgetShortcut);
    connect(dismiss, &QShortcut::activated, this, [this] {
        runRowAction(m_results->currentItem(), DismissAction);
    });
    // Down arrow from the query or replace box jumps into the results.
    for (QLineEdit *edit : {m_query, m_replace}) {
        auto *down = new QShortcut(QKeySequence(Qt::Key_Down), edit, nullptr, nullptr, Qt::WidgetShortcut);
        connect(down, &QShortcut::activated, this, [this] {
            if (m_results->topLevelItemCount() > 0) {
                m_results->setFocus();
                if (!m_results->currentItem()) {
                    m_results->setCurrentItem(m_results->topLevelItem(0));
                }
            }
        });
    }
    setReplaceVisible(false);
}

void SearchPanel::setRoot(const QString &dir)
{
    m_root = dir;
    stopProcess();
    m_results->clear();
    m_fileItems.clear();
    m_matchCount = 0;
    m_status->clear();
    if (!m_query->text().isEmpty()) {
        m_debounce.start();
    }
    updateReplaceAll();
}

void SearchPanel::focusQuery(const QString &text, bool replace)
{
    const bool seeded = !text.isEmpty() && !text.contains(QLatin1Char('\n'));
    if (seeded) {
        m_query->setText(text);
    }
    if (replace) {
        setReplaceVisible(true);
        if (seeded) { // like VS Code: the search is given, type the replacement
            m_replace->setFocus();
            m_replace->selectAll();
            return;
        }
    }
    m_query->setFocus();
    m_query->selectAll();
}

void SearchPanel::setDocumentProvider(const std::function<KTextEditor::Document *(const QString &)> &provider)
{
    m_documentFor = provider;
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
    m_searchedRegex = m_regex->isChecked();
    // The same search in Qt's syntax. ripgrep matches line by line, so do we;
    // -w is ripgrep's \b{start-half} and \b{end-half}.
    QString pattern = m_searchedRegex ? query : QRegularExpression::escape(query);
    if (m_word->isChecked()) {
        pattern = QStringLiteral("(?<!\\w)(?:") + pattern + QStringLiteral(")(?!\\w)");
    }
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (!m_case->isChecked()) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    m_matcher = QRegularExpression(pattern, options);
    m_matcher.optimize();

    if (query.isEmpty() || m_root.isEmpty()) {
        m_status->clear();
        updateReplaceAll();
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
    updateReplaceAll();
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
    QString raw = data.value(QLatin1String("lines")).toObject().value(QLatin1String("text")).toString();
    if (rel.isEmpty() || raw.isEmpty()) {
        return; // non UTF-8 path or binary line
    }
    if (raw.endsWith(QLatin1Char('\n'))) {
        raw.chop(1); // a CR stays: replacing compares it with the file
    }
    QString text = raw;
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r'))) {
        text.chop(1);
    }
    const int line = data.value(QLatin1String("line_number")).toInt() - 1;

    // ripgrep gives byte offsets; convert them to character positions.
    const QByteArray bytes = raw.toUtf8();
    QList<QPoint> ranges;
    QVariantList fullRanges;
    int byte = 0;
    int character = 0;
    const auto charAt = [&](int offset) {
        offset = qBound(byte, offset, int(bytes.size()));
        character += int(QString::fromUtf8(bytes.mid(byte, offset - byte)).size());
        byte = offset;
        return character;
    };
    const QJsonArray submatches = data.value(QLatin1String("submatches")).toArray();
    for (const QJsonValue &sv : submatches) {
        const QJsonObject sm = sv.toObject();
        const int startChar = charAt(sm.value(QLatin1String("start")).toInt());
        const int endChar = charAt(sm.value(QLatin1String("end")).toInt());
        ranges << QPoint(startChar, endChar - startChar);
        fullRanges << QPoint(startChar, endChar - startChar);
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
        fileItem->setData(0, RelPathRole, rel);
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
    item->setData(0, LineTextRole, raw);
    item->setData(0, MatchRangesRole, fullRanges);
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
        updateReplaceAll();
        return;
    }
    m_results->sortItems(0, Qt::AscendingOrder);
    updateStatus();
    updateReplaceAll();
}

void SearchPanel::updateStatus()
{
    const int files = m_results->topLevelItemCount();
    QString status;
    if (m_matchCount == 0) {
        status = tr("No results found.");
    } else if (m_truncated) {
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

// ---------------------------------------------------------------------------
// Replace

void SearchPanel::setReplaceVisible(bool visible)
{
    QColor color = palette().color(QPalette::Text);
    color.setAlphaF(0.85f);
    m_toggleReplace->setIcon(chevronIcon(visible, color));
    m_replace->setVisible(visible);
    m_replaceAll->setVisible(visible);
    m_results->viewport()->update();
}

bool SearchPanel::replacing() const
{
    return !m_replace->isHidden();
}

// The results are complete and still those of the search in the box.
bool SearchPanel::canReplace() const
{
    return !m_proc && !m_debounce.isActive() && m_matchCount > 0;
}

void SearchPanel::updateReplaceAll()
{
    m_replaceAll->setEnabled(canReplace());
    m_results->viewport()->update(); // hover buttons
}

QStringList SearchPanel::preview(const QModelIndex &index) const
{
    if (!replacing() || m_replace->text().isEmpty() || index.data(KindRole).toInt() != MatchItem) {
        return {};
    }
    QStringList out;
    if (!replacementsFor(index.data(LineTextRole).toString(), index.data(MatchRangesRole).toList(), &out)) {
        return {};
    }
    for (QString &text : out) {
        text.replace(QLatin1Char('\n'), QStringLiteral("↵"));
        text.replace(QLatin1Char('\t'), QLatin1Char(' '));
        text.remove(QLatin1Char('\r'));
    }
    return out;
}

// What replaces each of the matches ripgrep found in `line`. Each one is
// matched again at the same place with Qt's regular expression (for $1 and
// the like): false when one is not there with the same length (the line
// changed, or the two engines disagree).
bool SearchPanel::replacementsFor(const QString &line, const QVariantList &ranges, QStringList *out) const
{
    if (!m_matcher.isValid() || !line.isValidUtf16()) {
        return false;
    }
    const QString with = m_replace->text();
    out->clear();
    int end = 0;
    for (const QVariant &value : ranges) {
        const QPoint range = value.toPoint();
        if (range.x() < end) {
            return false;
        }
        const QRegularExpressionMatch match = m_matcher.match(line,
                                                              range.x(),
                                                              QRegularExpression::NormalMatch,
                                                              QRegularExpression::AnchorAtOffsetMatchOption
                                                                  | QRegularExpression::DontCheckSubjectStringMatchOption);
        if (!match.hasMatch() || match.capturedStart() != range.x() || match.capturedLength() != range.y()) {
            return false;
        }
        *out << (m_searchedRegex ? expandReplacement(with, match, m_matcher.captureCount()) : with);
        end = range.x() + range.y();
    }
    return true;
}

void SearchPanel::runRowAction(QTreeWidgetItem *item, int action)
{
    if (!item) {
        return;
    }
    const bool file = item->data(0, KindRole).toInt() == FileItem;
    if (action == DismissAction) {
        if (file) {
            removeFile(item);
        } else {
            removeRow(item);
        }
        updateStatus();
        updateReplaceAll();
        return;
    }
    if (!replacing() || !canReplace()) {
        return;
    }
    if (!m_matcher.isValid()) {
        QMessageBox::warning(this, tr("Replace"), tr("This regular expression cannot be used to replace: %1").arg(m_matcher.errorString()));
        return;
    }
    QList<QTreeWidgetItem *> rows;
    if (file) {
        for (int i = 0; i < item->childCount(); ++i) {
            rows << item->child(i);
        }
    } else {
        rows << item;
    }
    reportProblems(replaceRows(rows));
}

void SearchPanel::replaceAll()
{
    if (!replacing() || !canReplace()) {
        return;
    }
    if (!m_matcher.isValid()) {
        QMessageBox::warning(this, tr("Replace"), tr("This regular expression cannot be used to replace: %1").arg(m_matcher.errorString()));
        return;
    }
    const QString with = m_replace->text();
    QString shown = with;
    shown.replace(QLatin1Char('\n'), QStringLiteral("↵"));
    if (shown.size() > 40) {
        shown = shown.left(39) + QStringLiteral("…");
    }
    const int occurrences = m_matchCount;
    const int files = m_results->topLevelItemCount();
    const QString count = QString::number(occurrences);
    const QString fileCount = QString::number(files);
    QString question;
    if (with.isEmpty()) {
        question = occurrences == 1 ? tr("Replace 1 occurrence across 1 file?")
            : files == 1            ? tr("Replace %1 occurrences across 1 file?").arg(count)
                                    : tr("Replace %1 occurrences across %2 files?").arg(count, fileCount);
    } else {
        question = occurrences == 1 ? tr("Replace 1 occurrence across 1 file with '%1'?").arg(shown)
            : files == 1            ? tr("Replace %1 occurrences across 1 file with '%2'?").arg(count, shown)
                                    : tr("Replace %1 occurrences across %2 files with '%3'?").arg(count, fileCount, shown);
    }
    QMessageBox box(QMessageBox::Question, tr("Replace All"), question, QMessageBox::Cancel, this);
    QPushButton *confirm = box.addButton(tr("Replace"), QMessageBox::AcceptRole);
    box.setDefaultButton(confirm);
    box.exec();
    if (box.clickedButton() != confirm || !canReplace()) {
        return;
    }

    QList<QTreeWidgetItem *> rows;
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        QTreeWidgetItem *file = m_results->topLevelItem(i);
        for (int j = 0; j < file->childCount(); ++j) {
            rows << file->child(j);
        }
    }
    const Outcome outcome = replaceRows(rows);
    if (outcome.occurrences > 0) {
        const QString done = QString::number(outcome.occurrences);
        const QString doneFiles = QString::number(outcome.files);
        QString status;
        if (with.isEmpty()) {
            status = outcome.occurrences == 1 ? tr("Replaced 1 occurrence across 1 file.")
                : outcome.files == 1          ? tr("Replaced %1 occurrences across 1 file.").arg(done)
                                              : tr("Replaced %1 occurrences across %2 files.").arg(done, doneFiles);
        } else {
            status = outcome.occurrences == 1 ? tr("Replaced 1 occurrence across 1 file with '%1'.").arg(shown)
                : outcome.files == 1          ? tr("Replaced %1 occurrences across 1 file with '%2'.").arg(done, shown)
                                              : tr("Replaced %1 occurrences across %2 files with '%3'.").arg(done, doneFiles, shown);
        }
        m_status->setText(status);
    }
    reportProblems(outcome);
}

// Replaces the matches of `rows` and takes them out of the results; the
// ones that could not be replaced stay.
SearchPanel::Outcome SearchPanel::replaceRows(const QList<QTreeWidgetItem *> &rows)
{
    Outcome outcome;
    QList<QTreeWidgetItem *> files;
    QHash<QTreeWidgetItem *, QList<QTreeWidgetItem *>> byFile;
    for (QTreeWidgetItem *row : rows) {
        QTreeWidgetItem *file = row->parent();
        if (!byFile.contains(file)) {
            files << file;
        }
        byFile[file] << row;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_results->setUpdatesEnabled(false);
    bool written = false;
    for (QTreeWidgetItem *file : std::as_const(files)) {
        QList<QTreeWidgetItem *> fileRows = byFile.value(file);
        std::sort(fileRows.begin(), fileRows.end(), [](const QTreeWidgetItem *a, const QTreeWidgetItem *b) {
            return a->data(0, LineRole).toInt() < b->data(0, LineRole).toInt();
        });
        QString problem;
        bool fileWritten = false;
        const QList<QTreeWidgetItem *> doneList = replaceInFile(file->data(0, PathRole).toString(), fileRows, &problem, &fileWritten);
        const QSet<QTreeWidgetItem *> done(doneList.cbegin(), doneList.cend());
        written = written || fileWritten;
        if (!done.isEmpty()) {
            ++outcome.files;
        }
        if (!problem.isEmpty()) {
            outcome.problems << problem;
        }
        for (QTreeWidgetItem *row : std::as_const(fileRows)) {
            const int count = int(row->data(0, MatchRangesRole).toList().size());
            if (done.contains(row)) {
                outcome.occurrences += count;
            } else {
                outcome.skipped += count;
            }
        }
        if (done.size() == file->childCount()) {
            removeFile(file);
        } else {
            for (QTreeWidgetItem *row : doneList) {
                removeRow(row);
            }
        }
    }
    m_results->setUpdatesEnabled(true);
    QApplication::restoreOverrideCursor();
    if (written) {
        Q_EMIT filesWritten();
    }
    updateStatus();
    updateReplaceAll();
    return outcome;
}

// Replaces in one file (rows: its rows, top to bottom) and returns the rows
// done. An open file is changed in its document as one undo step and not
// saved, like VS Code; any other is rewritten on disk, only if it is UTF-8
// text, keeping its BOM and line ends.
QList<QTreeWidgetItem *> SearchPanel::replaceInFile(const QString &path, const QList<QTreeWidgetItem *> &rows, QString *problem, bool *written)
{
    QList<QTreeWidgetItem *> done;
    QList<QPair<int, int>> added; // (line, line breaks inserted) of the lines replaced
    const QString name = QDir(m_root).relativeFilePath(path);
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) {
        *problem = tr("%1 no longer exists.").arg(name);
        return done;
    }
    bool changed = false; // a line is no longer the one ripgrep read
    bool differs = false; // a match is not found again where ripgrep found it

    if (KTextEditor::Document *doc = m_documentFor ? m_documentFor(canonical) : nullptr) {
        if (!doc->isReadWrite()) {
            *problem = tr("%1 is read-only.").arg(name);
            return done;
        }
        KTextEditor::Document::EditingTransaction transaction(doc); // one undo step
        // Bottom up: the lines above keep their numbers.
        for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
            QTreeWidgetItem *row = *it;
            const int line = row->data(0, LineRole).toInt();
            QString expected = row->data(0, LineTextRole).toString();
            if (expected.endsWith(QLatin1Char('\r'))) {
                expected.chop(1); // documents keep line ends apart
            }
            if (line >= doc->lines() || doc->line(line) != expected) {
                changed = true;
                continue;
            }
            const QVariantList ranges = row->data(0, MatchRangesRole).toList();
            QStringList replacements;
            if (!replacementsFor(expected, ranges, &replacements)) {
                differs = true;
                continue;
            }
            // Match by match, right to left: cursors and marks elsewhere on the line stay put.
            int breaks = 0;
            for (qsizetype i = ranges.size() - 1; i >= 0; --i) {
                const QPoint range = ranges.at(i).toPoint();
                doc->replaceText(KTextEditor::Range(line, range.x(), line, range.x() + range.y()), replacements.at(i));
                breaks += int(replacements.at(i).count(QLatin1Char('\n')));
            }
            done << row;
            if (breaks > 0) {
                added.append({line, breaks});
            }
        }
    } else {
        QFile file(canonical);
        if (!file.open(QIODevice::ReadOnly)) {
            *problem = tr("%1 could not be read.").arg(name);
            return done;
        }
        const QByteArray bytes = file.readAll();
        file.close();
        // Other encodings (and binary files) would not survive the round trip.
        QStringDecoder decoder(QStringConverter::Utf8); // drops a BOM, added back below
        const QString text = decoder.decode(bytes);
        if (bytes.contains('\0') || decoder.hasError()) {
            *problem = tr("%1 is not a UTF-8 text file.").arg(name);
            return done;
        }
        QString out;
        out.reserve(text.size() + 64);
        qsizetype copied = 0; // text before this is in `out`
        qsizetype lineStart = 0;
        int lineNumber = 0;
        for (QTreeWidgetItem *row : rows) {
            const int line = row->data(0, LineRole).toInt();
            while (lineNumber < line) {
                const qsizetype newline = text.indexOf(QLatin1Char('\n'), lineStart);
                if (newline < 0) {
                    break;
                }
                lineStart = newline + 1;
                ++lineNumber;
            }
            if (lineNumber != line) {
                changed = true; // the file is shorter now
                break;
            }
            qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), lineStart);
            if (lineEnd < 0) {
                lineEnd = text.size();
            }
            const QString current = text.mid(lineStart, lineEnd - lineStart); // a CR included, as ripgrep reads it
            if (current != row->data(0, LineTextRole).toString()) {
                changed = true;
                continue;
            }
            const QVariantList ranges = row->data(0, MatchRangesRole).toList();
            QStringList replacements;
            if (!replacementsFor(current, ranges, &replacements)) {
                differs = true;
                continue;
            }
            const bool crlf = current.endsWith(QLatin1Char('\r'));
            out += QStringView(text).mid(copied, lineStart - copied);
            qsizetype pos = 0;
            int breaks = 0;
            for (qsizetype i = 0; i < ranges.size(); ++i) {
                const QPoint range = ranges.at(i).toPoint();
                QString replacement = replacements.at(i);
                breaks += int(replacement.count(QLatin1Char('\n')));
                if (crlf) {
                    replacement.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
                }
                out += QStringView(current).mid(pos, range.x() - pos);
                out += replacement;
                pos = range.x() + range.y();
            }
            out += QStringView(current).mid(pos);
            copied = lineEnd;
            done << row;
            if (breaks > 0) {
                added.append({line, breaks});
            }
        }
        if (!done.isEmpty()) {
            out += QStringView(text).mid(copied);
            QByteArray data = bytes.startsWith("\xEF\xBB\xBF") ? QByteArray("\xEF\xBB\xBF") : QByteArray();
            QStringEncoder encoder(QStringConverter::Utf8);
            data += encoder.encode(out);
            if (!QFileInfo(canonical).isWritable()) {
                *problem = tr("%1 is read-only.").arg(name);
                return {};
            }
            QSaveFile save(canonical);
            if (!save.open(QIODevice::WriteOnly) || save.write(data) != data.size() || !save.commit()) {
                *problem = tr("%1 could not be written: %2").arg(name, save.errorString());
                return {};
            }
            *written = true;
        }
    }

    if (changed) {
        *problem = tr("%1 has changed since the search.").arg(name);
    } else if (differs) {
        *problem = tr("%1: some matches could not be found again; replace them in the editor.").arg(name);
    }
    // Line breaks put in by a replacement move the rows below it.
    if (!added.isEmpty() && done.size() < rows.first()->parent()->childCount()) {
        std::sort(added.begin(), added.end());
        QList<int> lines;
        QList<int> shifts; // line breaks added above and on lines[i]
        for (const auto &[line, breaks] : std::as_const(added)) {
            lines << line;
            shifts << (shifts.isEmpty() ? 0 : shifts.last()) + breaks;
        }
        const QSet<QTreeWidgetItem *> replaced(done.cbegin(), done.cend());
        QTreeWidgetItem *file = rows.first()->parent();
        for (int i = 0; i < file->childCount(); ++i) {
            QTreeWidgetItem *row = file->child(i);
            const int line = row->data(0, LineRole).toInt();
            const auto above = std::lower_bound(lines.cbegin(), lines.cend(), line) - lines.cbegin();
            if (above > 0 && !replaced.contains(row)) {
                row->setData(0, LineRole, line + shifts.at(above - 1));
            }
        }
    }
    return done;
}

void SearchPanel::removeRow(QTreeWidgetItem *row)
{
    QTreeWidgetItem *file = row->parent();
    m_matchCount -= int(row->data(0, MatchRangesRole).toList().size());
    delete row;
    if (file->childCount() == 0) {
        m_fileItems.remove(file->data(0, RelPathRole).toString());
        delete file;
    } else {
        file->setData(0, CountRole, file->childCount());
    }
}

// All at once: much faster than row by row.
void SearchPanel::removeFile(QTreeWidgetItem *file)
{
    for (int i = 0; i < file->childCount(); ++i) {
        m_matchCount -= int(file->child(i)->data(0, MatchRangesRole).toList().size());
    }
    m_fileItems.remove(file->data(0, RelPathRole).toString());
    delete file;
}

void SearchPanel::reportProblems(const Outcome &outcome)
{
    if (outcome.problems.isEmpty()) {
        return;
    }
    const QString text = outcome.skipped == 1 ? tr("1 occurrence was not replaced.") : tr("%1 occurrences were not replaced.").arg(outcome.skipped);
    QStringList details = outcome.problems.mid(0, 10);
    if (outcome.problems.size() > 10) {
        details << QStringLiteral("…");
    }
    QMessageBox box(QMessageBox::Warning, tr("Replace"), text, QMessageBox::Ok, this);
    box.setInformativeText(details.join(QLatin1Char('\n')));
    box.exec();
}
