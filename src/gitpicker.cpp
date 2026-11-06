#include "gitpicker.h"

#include "delegates.h"
#include "fuzzy.h"
#include "projects.h"

#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

using namespace Lieve;

namespace
{
constexpr int kVisibleRows = 12;
constexpr int IndexRole = Qt::UserRole + 100; // index in m_rows, -1 for notes

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

QString joined(const QStringList &parts)
{
    QStringList kept = parts;
    kept.removeAll(QString());
    return kept.join(QStringLiteral(" · "));
}

// What the user typed, as a branch name: spaces become dashes, as in VS Code.
QString branchName(const QString &typed)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    return typed.trimmed().replace(spaces, QStringLiteral("-"));
}

QString elided(const QString &text, int length)
{
    return text.size() <= length ? text : text.left(length - 1).trimmed() + QStringLiteral("…");
}
}

GitPicker::GitPicker(GitRepo *repo, QWidget *parent)
    : QFrame(parent)
    , m_repo(repo)
{
    setObjectName(QStringLiteral("overlay")); // same look as Quick Open
    setFrameShape(QFrame::StyledPanel);
    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 140));
    setGraphicsEffect(shadow);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    m_edit = new QLineEdit(this);
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
    connect(m_edit, &QLineEdit::textChanged, this, &GitPicker::refilter);
    connect(m_list, &QListWidget::itemClicked, this, &GitPicker::accept);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (isVisible() && (!now || !isAncestorOf(now))) {
            hide();
        }
    });
    hide();
}

// ---------------------------------------------------------------------------
// Commands

void GitPicker::checkout()
{
    open(tr("Select a branch to check out"));
    const auto createRows = [this] {
        Row create;
        create.title = tr("+ Create new branch…");
        create.always = true;
        create.run = [this] {
            askBranchName([this](const QString &name) {
                finish([this, name] {
                    m_repo->createBranch(name, QString());
                });
            });
        };
        Row createFrom;
        createFrom.title = tr("+ Create new branch from…");
        createFrom.always = true;
        createFrom.run = [this] {
            askBranchName([this](const QString &name) {
                open(tr("Select a branch to create “%1” from").arg(name));
                const quint64 request = m_request;
                m_repo->branches([this, request, name](const QList<GitBranch> &branches) {
                    if (request != m_request) {
                        return;
                    }
                    m_branches = branches;
                    setRows(branchRows([this, name](const GitBranch &from) {
                                finish([this, name, from] {
                                    m_repo->createBranch(name, from.name);
                                });
                            }),
                            tr("There are no branches yet."));
                });
            });
        };
        return QList<Row>{create, createFrom};
    };
    setRows(createRows(), QString());
    const quint64 request = m_request;
    m_repo->branches([this, request, createRows](const QList<GitBranch> &branches) {
        if (request != m_request) {
            return;
        }
        m_branches = branches;
        setRows(createRows() + branchRows([this](const GitBranch &branch) {
                    finish([this, branch] {
                        switchTo(branch);
                    });
                }),
                QString());
    });
}

void GitPicker::stash(bool includeUntracked)
{
    if (!m_repo->canStash(includeUntracked)) {
        m_repo->stash(QString(), includeUntracked); // says there is nothing to stash
        return;
    }
    open(QString());
    ask(
        tr("Stash message"),
        QString(),
        [](const QString &, bool *ok) {
            *ok = true;
            return tr("Optionally provide a stash message (Press 'Enter' to confirm or 'Escape' to cancel)");
        },
        [this, includeUntracked](const QString &message) {
            finish([this, message, includeUntracked] {
                m_repo->stash(message.trimmed(), includeUntracked);
            });
        });
}

void GitPicker::pickStash(StashAction action)
{
    open(action == StashAction::Apply ? tr("Pick a stash to apply")
             : action == StashAction::Pop ? tr("Pick a stash to pop")
                                          : tr("Pick a stash to drop"));
    const quint64 request = m_request;
    m_repo->stashes([this, request, action](const QList<GitStash> &stashes) {
        if (request != m_request) {
            return;
        }
        QList<Row> rows;
        for (const GitStash &s : stashes) {
            Row row;
            row.title = s.message;
            const bool wip = s.message.startsWith(QLatin1String("WIP on ")); // "WIP on main: …" already names it
            row.detail = joined({wip ? QString() : s.branch, relativeAge(s.time)});
            row.note = QStringLiteral("stash@{%1}").arg(s.index);
            row.run = [this, s, action] {
                finish([this, s, action] {
                    if (action != StashAction::Drop) {
                        m_repo->applyStash(s.index, action == StashAction::Pop);
                        return;
                    }
                    QMessageBox box(QMessageBox::Warning,
                                    tr("Drop Stash", "confirmation dialog"),
                                    tr("Drop the stash “%1”?\nIts changes will be lost.").arg(s.message),
                                    QMessageBox::Cancel,
                                    window());
                    QPushButton *drop = box.addButton(tr("Drop Stash", "confirmation dialog"), QMessageBox::DestructiveRole);
                    box.setDefaultButton(QMessageBox::Cancel);
                    box.exec();
                    if (box.clickedButton() == drop) {
                        m_repo->dropStash(s.index);
                    }
                });
            };
            rows << row;
        }
        setRows(rows, tr("There are no stashes in the repository."));
    });
}

void GitPicker::history()
{
    open(tr("Select a commit to see its changes"));
    showLog(QString());
}

void GitPicker::fileHistory(const QString &file)
{
    if (file.isEmpty() || !file.startsWith(m_repo->root() + QLatin1Char('/'))) {
        open(tr("Select a commit"));
        setRows({}, file.isEmpty() ? tr("Open a file of the repository to see its history.") : tr("This file is not in the repository."));
        return;
    }
    open(tr("Select a commit to see its changes to %1").arg(QFileInfo(file).fileName()));
    showLog(file.mid(m_repo->root().size() + 1));
}

void GitPicker::showLog(const QString &relative)
{
    const quint64 request = m_request;
    m_repo->log(relative, [this, request, relative](const QList<GitCommit> &commits) {
        if (request != m_request) {
            return;
        }
        QList<Row> rows;
        for (const GitCommit &c : commits) {
            Row row;
            row.title = c.subject;
            row.detail = joined({c.author, relativeAge(c.time)});
            row.note = c.shortHash;
            row.extra = c.hash + QLatin1Char(' ') + c.author;
            row.run = [this, c, relative] {
                finish([this, c, relative] {
                    showCommit(c, relative);
                });
            };
            rows << row;
        }
        setRows(rows, relative.isEmpty() ? tr("There are no commits yet.") : tr("%1 has no commits yet.").arg(QFileInfo(relative).fileName()));
    });
}

void GitPicker::askBranchName(const std::function<void(const QString &)> &done)
{
    const QString typed = m_typed; // typed in the list: the name to start from
    ask(
        tr("Branch name"),
        typed,
        [this](const QString &text, bool *ok) {
            const QString name = branchName(text);
            *ok = false;
            if (name.isEmpty()) {
                return tr("Please provide a new branch name (Press 'Enter' to confirm or 'Escape' to cancel)");
            }
            if (!GitParse::validBranchName(name)) {
                return tr("“%1” is not a valid branch name").arg(name);
            }
            const bool exists = std::any_of(m_branches.cbegin(), m_branches.cend(), [&name](const GitBranch &b) {
                return !b.remote && b.name == name;
            });
            if (exists) {
                return tr("A branch named “%1” already exists").arg(name);
            }
            *ok = true;
            if (name != text.trimmed()) {
                return tr("The new branch will be “%1” (Press 'Enter' to confirm or 'Escape' to cancel)").arg(name);
            }
            return tr("Please provide a new branch name (Press 'Enter' to confirm or 'Escape' to cancel)");
        },
        [done](const QString &text) {
            done(branchName(text));
        });
}

// Local branches (the current one first), then the remote ones.
QList<GitPicker::Row> GitPicker::branchRows(const std::function<void(const GitBranch &)> &chosen) const
{
    QList<GitBranch> sorted = m_branches;
    std::stable_partition(sorted.begin(), sorted.end(), [](const GitBranch &b) {
        return b.current;
    });
    std::stable_partition(sorted.begin(), sorted.end(), [](const GitBranch &b) {
        return !b.remote;
    });
    QList<Row> rows;
    bool firstRemote = true;
    for (const GitBranch &b : std::as_const(sorted)) {
        Row row;
        row.title = b.name;
        QString sync;
        if (b.ahead > 0 || b.behind > 0) {
            sync = QStringLiteral("↓%1 ↑%2").arg(b.behind).arg(b.ahead); // as in the status bar
        }
        row.detail = joined({b.hash, relativeAge(b.time), sync});
        row.extra = b.hash;
        if (b.current) {
            row.note = tr("current", "the branch checked out");
        }
        if (b.remote && firstRemote) {
            row.section = tr("remote branches");
            firstRemote = false;
        }
        row.run = [chosen, b] {
            chosen(b);
        };
        rows << row;
    }
    return rows;
}

void GitPicker::switchTo(const GitBranch &branch)
{
    if (branch.current) {
        return;
    }
    if (!branch.remote) {
        m_repo->checkout(branch.name);
        return;
    }
    // origin/feature/x: the local feature/x if there is one, else a new one that tracks it.
    const QString local = branch.name.mid(branch.name.indexOf(QLatin1Char('/')) + 1);
    const bool exists = std::any_of(m_branches.cbegin(), m_branches.cend(), [&local](const GitBranch &b) {
        return !b.remote && b.name == local;
    });
    if (exists) {
        m_repo->checkout(local);
    } else {
        m_repo->checkoutTracking(branch.name);
    }
}

void GitPicker::showCommit(const GitCommit &commit, const QString &file)
{
    const QString path = file.isEmpty() ? QString() : (commit.path.isEmpty() ? file : commit.path);
    m_repo->show(commit.hash, path, [this, commit, path](const QString &text) {
        const QString key = QStringLiteral("commit:%1:%2").arg(commit.hash, path);
        const QString name = path.isEmpty() ? elided(commit.subject, 40) : QFileInfo(path).fileName();
        const QString title = tr("%1 (%2)", "file name or commit subject, then the short commit hash").arg(name, commit.shortHash);
        Q_EMIT diffReady(key, title, path.isEmpty() ? QStringLiteral("commit.diff") : QFileInfo(path).fileName(), text);
    });
}

// ---------------------------------------------------------------------------
// The box

void GitPicker::open(const QString &placeholder)
{
    ++m_request;
    if (!isVisible()) {
        QWidget *focus = QApplication::focusWidget();
        m_returnFocus = focus && !isAncestorOf(focus) ? focus : nullptr;
    }
    m_asking = false;
    m_rows.clear();
    m_empty.clear();
    m_edit->setPlaceholderText(placeholder);
    m_edit->blockSignals(true);
    m_edit->clear();
    m_edit->blockSignals(false);
    refilter();
    show();
    raise();
    m_edit->setFocus();
}

void GitPicker::setRows(const QList<Row> &rows, const QString &empty)
{
    m_rows = rows;
    m_empty = empty;
    refilter();
}

void GitPicker::ask(const QString &placeholder,
                    const QString &text,
                    const std::function<QString(const QString &, bool *)> &check,
                    const std::function<void(const QString &)> &done)
{
    ++m_request;
    m_asking = true;
    m_rows.clear();
    m_check = check;
    m_answer = done;
    m_edit->setPlaceholderText(placeholder);
    m_edit->blockSignals(true);
    m_edit->setText(text);
    m_edit->blockSignals(false);
    refilter();
}

void GitPicker::addInfo(const QString &text)
{
    auto *item = new QListWidgetItem(text, m_list);
    item->setData(KindRole, CommandItem);
    item->setData(IndexRole, -1);
}

void GitPicker::refilter()
{
    m_list->clear();
    const QString text = m_edit->text();
    if (m_asking) {
        bool ok = false;
        addInfo(m_check(text, &ok));
        m_list->setCurrentRow(0);
        place();
        return;
    }
    QString query = text.toLower();
    query.remove(QLatin1Char(' '));
    struct Match {
        int index;
        int score;
        QList<int> positions;
    };
    QList<Match> matches;
    QList<int> always;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        if (query.isEmpty()) {
            matches.append({i, 0, {}});
            continue;
        }
        if (row.always) {
            always << i;
            continue;
        }
        QString plain = row.title;
        plain.replace(QLatin1Char('/'), QLatin1Char(' ')); // "/" would start a file name for the scorer
        int score = fuzzyScore(plain, plain.toLower(), query);
        QList<int> positions;
        if (score != INT_MIN) {
            positions = fuzzyPositions(row.title, row.title.toLower(), query);
        } else if (!row.extra.isEmpty()) {
            const QString extra = row.extra.toLower();
            score = extra.contains(query) ? -1000 : INT_MIN; // a hash or an author: after the subjects
        }
        if (score != INT_MIN) {
            matches.append({i, score, positions});
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        return a.score > b.score;
    });
    // While filtering, "Create new branch…" comes after the matches: Enter
    // takes the best match, and the typed text when nothing matches.
    for (const int i : std::as_const(always)) {
        matches.append({i, 0, {}});
    }
    for (const Match &m : std::as_const(matches)) {
        const Row &row = m_rows.at(m.index);
        auto *item = new QListWidgetItem(row.title, m_list);
        item->setData(KindRole, CommandItem);
        item->setData(IndexRole, m.index);
        item->setData(DirRole, row.detail);
        item->setData(RangesRole, rangesOf(m.positions));
        if (query.isEmpty() && !row.section.isEmpty()) {
            item->setData(NoteRole, row.section);
            item->setData(SeparatorRole, true);
        } else {
            item->setData(NoteRole, row.note);
        }
        if (!row.detail.isEmpty()) {
            item->setToolTip(row.title + QStringLiteral("\n") + row.detail);
        }
    }
    if (matches.isEmpty() && !(m_rows.isEmpty() && m_empty.isEmpty())) {
        addInfo(m_rows.isEmpty() ? m_empty : tr("No matching results"));
    }
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
    place();
}

void GitPicker::accept()
{
    if (m_asking) {
        bool ok = false;
        m_check(m_edit->text(), &ok);
        if (ok && m_answer) {
            const auto answer = m_answer;
            answer(m_edit->text());
        }
        return;
    }
    QListWidgetItem *item = m_list->currentItem();
    const int index = item ? item->data(IndexRole).toInt() : -1;
    if (index < 0 || index >= m_rows.size()) {
        return; // a note, not something to run
    }
    m_typed = m_edit->text().trimmed();
    const std::function<void()> run = m_rows.at(index).run; // may replace the rows
    if (run) {
        run();
    }
}

void GitPicker::finish(const std::function<void()> &action)
{
    ++m_request;
    dismiss();
    // Later, out of this key event: it may ask with a dialog.
    QTimer::singleShot(0, this, action);
}

// Hides and gives the focus back to where it was before opening.
void GitPicker::dismiss()
{
    QWidget *back = m_returnFocus;
    if (back && back->isVisible() && back->window() == window()) {
        back->setFocus(Qt::OtherFocusReason); // this hides the overlay (focusChanged)
    }
    hide();
}

void GitPicker::place()
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

void GitPicker::testStep(const QString &step)
{
    if (step == QLatin1String("rows")) {
        qInfo().noquote() << "picker" << (isVisible() ? "visible" : "hidden") << "|" << m_edit->placeholderText() << "|" << m_edit->text();
        for (int r = 0; r < m_list->count(); ++r) {
            const QListWidgetItem *item = m_list->item(r);
            qInfo().noquote() << (r == m_list->currentRow() ? " >" : "  ") << item->text() << "|" << item->data(DirRole).toString() << "|"
                              << item->data(NoteRole).toString() << (item->data(SeparatorRole).toBool() ? "| ---" : "");
        }
        return;
    }
    if (step.startsWith(QLatin1String("type:"))) {
        m_edit->setText(step.mid(5));
        return;
    }
    const QHash<QString, int> keys = {{QStringLiteral("down"), Qt::Key_Down},
                                      {QStringLiteral("up"), Qt::Key_Up},
                                      {QStringLiteral("enter"), Qt::Key_Return},
                                      {QStringLiteral("esc"), Qt::Key_Escape}};
    if (keys.contains(step)) {
        QKeyEvent key(QEvent::KeyPress, keys.value(step), Qt::NoModifier);
        QCoreApplication::sendEvent(m_edit, &key);
    }
}

bool GitPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        place();
    }
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int row = m_list->currentRow();
        switch (key->key()) {
        case Qt::Key_Escape:
            ++m_request;
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
