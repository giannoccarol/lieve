#include "quickopen.h"

#include "delegates.h"
#include "fuzzy.h"
#include "settings.h"
#include "vscodeicons.h"

#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

using namespace Lieve;

// The search box; after a lone ">" it shows a dim hint, as VS Code does.
class HintLineEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    QString hint;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QLineEdit::paintEvent(event);
        if (hint.isEmpty() || text() != QLatin1String(">") || cursorPosition() != 1) {
            return;
        }
        QPainter p(this);
        p.setPen(palette().color(QPalette::PlaceholderText));
        const QRect cursor = cursorRect();
        const int x = cursor.center().x() + 4;
        const QRect r(x, cursor.top(), width() - x - 8, cursor.height());
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(hint, Qt::ElideRight, r.width()));
    }
};

namespace
{
constexpr int kMaxShown = 60;
constexpr int kVisibleRows = 12;
constexpr int IndexRole = Qt::UserRole + 100; // command or choice row: index in its list, -1 for notes

// Highlight ranges for the delegate from matched letter positions.
QVariantList rangesOf(const QList<int> &positions)
{
    QVariantList ranges;
    for (qsizetype i = 0; i < positions.size();) {
        qsizetype j = i + 1;
        while (j < positions.size() && positions.at(j) == positions.at(j - 1) + 1) {
            ++j;
        }
        ranges << QPoint(positions.at(i), int(j - i));
        i = j;
    }
    return ranges;
}
}

QuickOpen::QuickOpen(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("overlay"));
    setFrameShape(QFrame::StyledPanel);
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 140));
    setGraphicsEffect(shadow);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    m_edit = new HintLineEdit(this);
    m_edit->hint = tr("Type the name of a command to run.");
    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(QSize(16, 16));
    m_list->setItemDelegate(new FileItemDelegate(m_list));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_edit);
    layout->addWidget(m_list);

    m_edit->installEventFilter(this);
    parent->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, &QuickOpen::refilter);
    connect(m_list, &QListWidget::itemClicked, this, &QuickOpen::accept);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (isVisible() && (!now || !isAncestorOf(now))) {
            hide();
        }
    });
    hide();
}

void QuickOpen::setRoot(const QString &dir)
{
    m_root = dir;
    m_files.clear();
    m_filesLower.clear();
}

void QuickOpen::setCommandSource(const std::function<QList<Command>()> &source)
{
    m_commandSource = source;
}

void QuickOpen::setEditorSource(const std::function<KTextEditor::View *()> &source)
{
    m_editorSource = source;
}

void QuickOpen::popup(const QStringList &recentFiles, const QString &text)
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus || !isAncestorOf(focus)) {
        m_returnFocus = focus;
    }
    m_recent = recentFiles;
    m_filesStale = true;
    m_mode = Mode::Files; // so ">" gathers the commands afresh, while the focus is still where it was
    m_edit->setPlaceholderText(tr("Search files by name (append :line to go to a line)"));
    m_edit->blockSignals(true);
    m_edit->setText(text);
    m_edit->blockSignals(false);
    refilter();
    show();
    raise();
    m_edit->setFocus();
}

void QuickOpen::popupChoices(const QList<CommandChoice> &choices, const QString &placeholder)
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus || !isAncestorOf(focus)) {
        m_returnFocus = focus;
    }
    m_choices = choices;
    m_choiceCommand = Command();
    m_mode = Mode::Choices;
    m_edit->setPlaceholderText(placeholder);
    m_edit->blockSignals(true);
    m_edit->clear();
    m_edit->blockSignals(false);
    refilter();
    show();
    raise();
    m_edit->setFocus();
}

void QuickOpen::reloadFiles()
{
    if (m_root.isEmpty() || m_proc) {
        return;
    }
    QStringList args{QStringLiteral("--files"), QStringLiteral("--hidden"), QStringLiteral("--glob"), QStringLiteral("!.git")};
    const QStringList excluded = config()
                                     .value(QStringLiteral("search/exclude"),
                                            QStringList{QStringLiteral("**/node_modules"), QStringLiteral("**/bower_components")})
                                     .toStringList();
    for (const QString &e : excluded) {
        if (!e.trimmed().isEmpty()) {
            args << QStringLiteral("--glob") << QLatin1Char('!') + e.trimmed();
        }
    }
    args << QStringLiteral(".");

    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(m_root);
    m_proc->setStandardInputFile(QProcess::nullDevice());
    const QString root = m_root;
    connect(m_proc, &QProcess::finished, this, [this, root] {
        const QList<QByteArray> lines = m_proc->readAllStandardOutput().split('\n');
        m_proc->deleteLater();
        m_proc = nullptr;
        if (root != m_root) {
            return; // folder changed meanwhile
        }
        QStringList files;
        files.reserve(lines.size());
        for (const QByteArray &l : lines) {
            if (l.isEmpty()) {
                continue;
            }
            QString f = QString::fromUtf8(l);
            if (f.startsWith(QLatin1String("./"))) {
                f = f.mid(2);
            }
            files << f;
        }
        std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        });
        m_files = files;
        m_filesLower.clear();
        m_filesLower.reserve(files.size());
        for (const QString &f : std::as_const(m_files)) {
            m_filesLower << f.toLower();
        }
        if (isVisible() && m_mode == Mode::Files) {
            refilter();
        }
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_proc) {
            m_proc->deleteLater();
            m_proc = nullptr;
        }
    });
    m_proc->start(QStringLiteral("rg"), args);
}

void QuickOpen::refilter()
{
    const QString text = m_edit->text();
    if (m_mode != Mode::Choices) {
        const Mode mode = text.startsWith(QLatin1Char('>')) ? Mode::Commands
            : text.startsWith(QLatin1Char(':'))             ? Mode::Line
                                                            : Mode::Files;
        if (mode == Mode::Commands && m_mode != Mode::Commands) {
            m_commands = m_commandSource ? m_commandSource() : QList<Command>();
            m_labels.clear();
            for (const Command &c : std::as_const(m_commands)) {
                m_labels << c.label();
            }
        }
        m_mode = mode;
    }
    m_list->clear();
    switch (m_mode) {
    case Mode::Files:
        refilterFiles(text);
        if (m_filesStale) {
            m_filesStale = false;
            reloadFiles();
        }
        break;
    case Mode::Commands:
        refilterCommands(text.mid(1));
        break;
    case Mode::Choices:
        refilterChoices(text);
        break;
    case Mode::Line:
        refilterLine(text);
        break;
    }
    if (m_list->count() > 0 && m_list->currentRow() < 0) {
        m_list->setCurrentRow(0);
    }
    m_edit->update(); // the hint after ">"
    place();
}

void QuickOpen::refilterFiles(QString query)
{
    query = query.trimmed();
    m_line = -1;
    static const QRegularExpression lineSuffix(QStringLiteral("^(.*?):(\\d+)$"));
    if (const auto match = lineSuffix.match(query); match.hasMatch()) {
        query = match.captured(1);
        m_line = match.captured(2).toInt() - 1;
    }
    query.remove(QLatin1Char(' '));
    const QString lowerQuery = query.toLower();

    QStringList chosen;
    if (lowerQuery.isEmpty()) {
        // Recently opened first, like VS Code.
        for (const QString &abs : std::as_const(m_recent)) {
            if (!m_root.isEmpty() && abs.startsWith(m_root + QLatin1Char('/'))) {
                chosen << abs.mid(m_root.size() + 1);
            }
        }
        for (const QString &f : std::as_const(m_files)) {
            if (chosen.size() >= kMaxShown) {
                break;
            }
            if (!chosen.contains(f)) {
                chosen << f;
            }
        }
    } else {
        QList<QPair<int, int>> scored; // score, index
        for (int i = 0; i < m_files.size(); ++i) {
            const int s = fuzzyScore(m_files.at(i), m_filesLower.at(i), lowerQuery);
            if (s != INT_MIN) {
                scored.append({s, i});
            }
        }
        const auto middle = scored.begin() + std::min<qsizetype>(kMaxShown, scored.size());
        std::partial_sort(scored.begin(), middle, scored.end(), [](const auto &a, const auto &b) {
            return a.first > b.first;
        });
        for (auto it = scored.begin(); it != middle; ++it) {
            chosen << m_files.at(it->second);
        }
    }

    for (const QString &rel : std::as_const(chosen)) {
        const QFileInfo fi(rel);
        const QString name = fi.fileName();
        auto *item = new QListWidgetItem(VsCodeIcons::instance().fileIcon(name), name, m_list);
        item->setData(KindRole, FileItem);
        item->setData(PathRole, QDir(m_root).filePath(rel));
        item->setData(DirRole, fi.path() == QLatin1String(".") ? QString() : fi.path());
        item->setData(RangesRole, rangesOf(fuzzyPositions(name, name.toLower(), lowerQuery)));
        item->setToolTip(rel);
    }
}

void QuickOpen::addInfo(const QString &text)
{
    auto *item = new QListWidgetItem(text, m_list);
    item->setData(KindRole, CommandItem);
    item->setData(IndexRole, -1);
}

void QuickOpen::refilterCommands(const QString &typed)
{
    QString query = typed.toLower();
    query.remove(QLatin1Char(' '));
    const QStringList recent = recentCommands();
    struct Row {
        int index;
        int score;
        qsizetype recent; // position in the recently used list, -1 if not there
        QList<int> positions;
    };
    QList<Row> rows;
    for (int i = 0; i < m_commands.size(); ++i) {
        const QString &label = m_labels.at(i);
        Row row{i, 0, recent.indexOf(m_commands.at(i).id), {}};
        if (!query.isEmpty()) {
            QString plain = label;
            plain.replace(QLatin1Char('/'), QLatin1Char(' ')); // "/" would start a file name for the scorer
            row.score = fuzzyScore(plain, plain.toLower(), query);
            if (row.score == INT_MIN) {
                continue;
            }
            bool wordsOnly = false;
            row.positions = fuzzyPositions(label, label.toLower(), query, &wordsOnly);
            if (!wordsOnly) {
                row.recent = -1; // a loose match is not pulled to the top
            }
        }
        rows << row;
    }
    // Recently used first (newest first), then the best matches, then by name.
    std::sort(rows.begin(), rows.end(), [this](const Row &a, const Row &b) {
        if ((a.recent >= 0) != (b.recent >= 0)) {
            return a.recent >= 0;
        }
        if (a.recent >= 0) {
            return a.recent < b.recent;
        }
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return QString::localeAwareCompare(m_labels.at(a.index), m_labels.at(b.index)) < 0;
    });

    for (qsizetype j = 0; j < rows.size(); ++j) {
        const Row &row = rows.at(j);
        auto *item = new QListWidgetItem(m_labels.at(row.index), m_list);
        item->setData(KindRole, CommandItem);
        item->setData(IndexRole, row.index);
        item->setData(RangesRole, rangesOf(row.positions));
        item->setData(ShortcutRole, m_commands.at(row.index).shortcut);
        if (row.recent >= 0 && j == 0) {
            item->setData(NoteRole, tr("recently used", "commands"));
        } else if (row.recent < 0 && j > 0 && rows.at(j - 1).recent >= 0) {
            item->setData(NoteRole, tr("other commands"));
            item->setData(SeparatorRole, true);
        }
    }
    if (rows.isEmpty()) {
        addInfo(tr("No matching commands"));
    }
}

void QuickOpen::refilterChoices(const QString &typed)
{
    QString query = typed.toLower();
    query.remove(QLatin1Char(' '));
    QList<QPair<int, int>> scored; // score, index
    for (int i = 0; i < m_choices.size(); ++i) {
        const QString &title = m_choices.at(i).title;
        const int s = query.isEmpty() ? 0 : fuzzyScore(title, title.toLower(), query);
        if (s != INT_MIN) {
            scored.append({s, i});
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    int current = -1;
    for (const auto &[score, index] : std::as_const(scored)) {
        const CommandChoice &choice = m_choices.at(index);
        auto *item = new QListWidgetItem(choice.title, m_list);
        item->setData(KindRole, CommandItem);
        item->setData(IndexRole, index);
        item->setData(DirRole, choice.detail);
        item->setData(RangesRole, rangesOf(fuzzyPositions(choice.title, choice.title.toLower(), query)));
        if (choice.current) {
            item->setData(NoteRole, tr("current", "the language or encoding in use"));
            if (current < 0) {
                current = m_list->count() - 1;
            }
        }
    }
    if (scored.isEmpty()) {
        addInfo(tr("No matching results"));
    } else if (query.isEmpty() && current >= 0) {
        m_list->setCurrentRow(current);
    }
}

void QuickOpen::refilterLine(const QString &text)
{
    m_line = -1;
    m_column = 0;
    KTextEditor::View *view = m_editorSource ? m_editorSource() : nullptr;
    if (!view) {
        addInfo(tr("Open a text editor first to go to a line."));
        return;
    }
    // ":42", ":42:7" or ":42,7"
    static const QRegularExpression lineColumn(QStringLiteral("^:\\s*(\\d+)\\s*(?:[:,]\\s*(\\d*))?\\s*$"));
    const auto match = lineColumn.match(text);
    const int lines = view->document()->lines();
    if (!match.hasMatch()) {
        const KTextEditor::Cursor cursor = view->cursorPosition();
        addInfo(tr("Current Line: %1, Character: %2. Type a line number between 1 and %3 to navigate to.")
                    .arg(cursor.line() + 1)
                    .arg(cursor.column() + 1)
                    .arg(lines));
        return;
    }
    const int line = std::clamp(match.captured(1).toInt(), 1, std::max(1, lines));
    m_line = line - 1;
    QString message = tr("Go to line %1.").arg(line);
    if (!match.captured(2).isEmpty()) {
        const int column = std::max(1, match.captured(2).toInt());
        m_column = column - 1;
        message = tr("Go to line %1 and character %2.").arg(line).arg(column);
    }
    auto *item = new QListWidgetItem(message, m_list);
    item->setData(KindRole, CommandItem);
    item->setData(IndexRole, 0);
}

void QuickOpen::accept()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) {
        return;
    }
    const int index = item->data(IndexRole).isValid() ? item->data(IndexRole).toInt() : 0;
    if (index < 0) {
        return; // a note, not something to run
    }
    switch (m_mode) {
    case Mode::Files: {
        const QString path = item->data(PathRole).toString();
        const int line = m_line;
        hide();
        Q_EMIT fileChosen(path, line);
        break;
    }
    case Mode::Commands:
        runCommand(m_commands.at(index));
        break;
    case Mode::Choices: {
        QAction *choice = m_choices.at(index).action;
        const std::function<void()> prepare = m_choiceCommand.prepare;
        dismiss();
        if (choice) {
            QTimer::singleShot(0, choice, [choice, prepare] {
                if (prepare) {
                    prepare();
                }
                choice->trigger();
            });
        }
        break;
    }
    case Mode::Line: {
        KTextEditor::View *view = m_editorSource ? m_editorSource() : nullptr;
        if (!view || m_line < 0) {
            return;
        }
        const int line = m_line;
        const int column = std::min(m_column, view->document()->lineLength(line));
        dismiss();
        view->setCursorPosition(KTextEditor::Cursor(line, column));
        view->setFocus();
        break;
    }
    }
}

void QuickOpen::runCommand(const Command &command)
{
    QAction *action = command.action;
    if (!action) {
        dismiss();
        return;
    }
    rememberCommand(command.id);
    if (command.choices) {
        m_choices = commandChoices(action);
        if (!m_choices.isEmpty()) {
            // A second list in the same box, like VS Code's language picker.
            m_choiceCommand = command;
            m_mode = Mode::Choices;
            m_edit->setPlaceholderText(command.title);
            m_edit->blockSignals(true);
            m_edit->clear();
            m_edit->blockSignals(false);
            refilter();
            return;
        }
    }
    dismiss();
    // Later, out of this key event: the command may close the window.
    const std::function<void()> prepare = command.prepare;
    QTimer::singleShot(0, action, [action, prepare] {
        if (prepare) {
            prepare();
        }
        action->trigger();
    });
}

// Hides and gives the focus back to where it was before opening.
void QuickOpen::dismiss()
{
    QWidget *back = m_returnFocus;
    if (back && back->isVisible() && back->window() == window()) {
        back->setFocus(Qt::OtherFocusReason); // this hides the overlay (focusChanged)
    }
    hide();
}

void QuickOpen::place()
{
    QWidget *p = parentWidget();
    const int width = std::min(680, p->width() - 40);
    const int rowHeight = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0) : fontMetrics().height() + 8;
    const int rows = std::min<int>(kVisibleRows, m_list->count());
    m_list->setFixedHeight(rows * rowHeight + (rows > 0 ? 4 : 0));
    m_list->setVisible(rows > 0);
    adjustSize();
    setGeometry((p->width() - width) / 2, 8, width, sizeHint().height());
}

bool QuickOpen::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        place();
    }
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int row = m_list->currentRow();
        switch (key->key()) {
        case Qt::Key_Escape:
            dismiss();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            accept();
            return true;
        case Qt::Key_Down:
            m_list->setCurrentRow(std::min(row + 1, m_list->count() - 1));
            return true;
        case Qt::Key_Up:
            m_list->setCurrentRow(std::max(row - 1, 0));
            return true;
        case Qt::Key_PageDown:
            m_list->setCurrentRow(std::min(row + kVisibleRows, m_list->count() - 1));
            return true;
        case Qt::Key_PageUp:
            m_list->setCurrentRow(std::max(row - kVisibleRows, 0));
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}
