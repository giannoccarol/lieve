#pragma once

#include <QColor>
#include <QList>
#include <QPointer>
#include <QWidget>

namespace KTextEditor
{
class MovingRange;
class View;
}

// Whole-line backgrounds in a KTextEditor view, like VS Code's diff colours.
// KTextEditor paints a range's background only behind the text (it ignores
// QTextFormat::FullWidthSelection on ranges), so this see-through layer over
// the text area paints the rest of each tinted line's row in the same
// colour: from the end of its text to the right edge, and the margin before
// its first column. The text keeps its range background and is never painted
// over. The layer sits under KTextEditor's own floating widgets (messages),
// takes no mouse events and paints only when the view does.
//
// It can also draw a hairline between two lines, where the other side of a
// diff has lines that this one lacks.
class LineTint : public QWidget
{
public:
    struct Span {
        int first = 0; // lines first..last
        int last = 0;
        QColor color;
    };
    struct Edge {
        int line = 0; // the hairline is on top of this line (lines() = below the last one)
        QColor color;
    };

    // The layer of `view`, made the first time (null if the view has no text area).
    static LineTint *of(KTextEditor::View *view);
    void set(const QList<Span> &spans, const QList<Edge> &edges = {});
    // The lines a background range covers (a range that ends at the start of
    // a line stops on the line above), in its background colour.
    static Span spanOf(const KTextEditor::MovingRange *range);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    LineTint(KTextEditor::View *view, QWidget *area);

    QPointer<KTextEditor::View> m_view;
    QList<Span> m_spans;
    QList<Edge> m_edges;
};
