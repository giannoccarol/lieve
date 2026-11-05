#include "symbolpicker.h"

#include "delegates.h"
#include "fuzzy.h"

#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

using namespace Lieve;

namespace
{
constexpr int kVisibleRows = 14;
}

SymbolPicker::SymbolPicker(QWidget *parent)
    : QFrame(parent)
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
    connect(m_edit, &QLineEdit::textChanged, this, &SymbolPicker::refilter);
    connect(m_list, &QListWidget::itemClicked, this, &SymbolPicker::accept);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (QListWidgetItem *item = m_list->item(row); item && isVisible()) {
            Q_EMIT highlighted(item->data(Qt::UserRole + 100).toInt());
        }
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (isVisible() && (!now || !isAncestorOf(now))) {
            cancel();
        }
    });
    hide();
}

void SymbolPicker::popup(const QString &placeholder, const QList<Entry> &entries)
{
    m_entries = entries;
    m_lower.clear();
    for (const Entry &e : std::as_const(m_entries)) {
        m_lower << e.name.toLower();
    }
    m_edit->setPlaceholderText(placeholder);
    m_edit->blockSignals(true);
    m_edit->clear();
    m_edit->blockSignals(false);
    m_closing = false;
    show();
    raise();
    refilter();
    m_edit->setFocus();
}

void SymbolPicker::refilter()
{
    const QString query = m_edit->text().trimmed().remove(QLatin1Char(' ')).toLower();
    QList<int> chosen;
    if (query.isEmpty()) {
        for (int i = 0; i < m_entries.size(); ++i) {
            chosen << i;
        }
    } else {
        QList<QPair<int, int>> scored; // score, index
        for (int i = 0; i < m_entries.size(); ++i) {
            const int s = fuzzyScore(m_entries.at(i).name, m_lower.at(i), query);
            if (s != INT_MIN) {
                scored.append({s, i});
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
            return a.first > b.first;
        });
        for (const auto &s : std::as_const(scored)) {
            chosen << s.second;
        }
    }
    m_list->blockSignals(true);
    m_list->clear();
    for (const int i : std::as_const(chosen)) {
        const Entry &e = m_entries.at(i);
        auto *item = new QListWidgetItem(e.icon, e.name, m_list);
        item->setData(KindRole, FileItem);
        item->setData(DirRole, e.detail);
        item->setData(Qt::UserRole + 100, e.index);
        item->setToolTip(e.detail.isEmpty() ? e.name : e.name + QStringLiteral(" — ") + e.detail);
    }
    m_list->blockSignals(false);
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
        Q_EMIT highlighted(m_list->item(0)->data(Qt::UserRole + 100).toInt());
    }
    place();
}

void SymbolPicker::accept()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) {
        return;
    }
    const int index = item->data(Qt::UserRole + 100).toInt();
    m_closing = true;
    hide();
    Q_EMIT chosen(index);
}

void SymbolPicker::cancel()
{
    if (m_closing) {
        return;
    }
    m_closing = true;
    hide();
    Q_EMIT cancelled();
}

void SymbolPicker::place()
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

bool SymbolPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        place();
    }
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int row = m_list->currentRow();
        switch (key->key()) {
        case Qt::Key_Escape:
            cancel();
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
