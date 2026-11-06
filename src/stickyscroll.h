#pragma once

#include <QFont>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <vector>

class QTextLayout;
namespace KTextEditor
{
class View;
}
namespace Lieve
{
class DocumentScopes;
}

// VS Code's sticky scroll: while scrolling, the first lines of the scopes
// the top of the view is inside (class, function, if...) stay pinned at the
// top of the editor, at most five, with their line numbers and highlighting.
// Clicking one goes there. Hidden, and idle, while nothing sticks (at the
// top of the file, outside any scope).
class StickyScroll : public QWidget
{
    Q_OBJECT
public:
    StickyScroll(KTextEditor::View *view, Lieve::DocumentScopes *scopes);
    ~StickyScroll() override;

    // The document lines shown, outermost first.
    QList<int> lines() const
    {
        return m_lines;
    }
    void refresh();
    // For the automated checks: a click on a row.
    void clickRow(int row);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct Metrics {
        QFont font;
        int rowHeight = 0;
        qreal ascent = 0; // baseline in a row
    };
    Metrics metrics() const;
    void setLines(const QList<int> &lines);
    void buildLayouts();
    int textLeft() const; // where the text area starts, in this widget
    void goTo(int row, int x);
    void revealCursor();

    QPointer<KTextEditor::View> m_view;
    Lieve::DocumentScopes *m_scopes;
    QPointer<QWidget> m_border; // KTextEditor's line number column
    QPointer<QWidget> m_shadow; // under the last line
    QList<int> m_lines;
    std::vector<std::unique_ptr<QTextLayout>> m_layouts; // one per line
    bool m_layoutsStale = true;
    Metrics m_metrics;
    int m_hovered = -1;
    int m_top = -1; // the view's first line at the last refresh
    QTimer m_editTimer;
};
