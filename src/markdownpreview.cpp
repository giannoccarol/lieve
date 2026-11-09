#include "markdownpreview.h"

#include "platform.h"
#include "settings.h"
#include "theme.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImageReader>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

using namespace Lieve;

namespace
{
// Marks left on the rendered document, for painting and for later passes.
enum Property {
    BlockKind = QTextFormat::UserProperty + 1, // block: see Kind
    CodeRun, // block: which code block a code line belongs to
    TaskState, // block: a task list item, 1 open, 2 done (its box is painted)
    CodeSpan, // char: inline code, and the narrow spaces around it
    ImageSource, // char: an image's src as written
};

enum Kind {
    Paragraph = 1,
    Heading,
    Code,
    Rule,
    Empty,
    TableCell,
};

constexpr QChar kCodePad(0x202F); // narrow no-break space: room around inline code
const QString kFloatMarker = QStringLiteral("#lieve-float-");
const QString kImageScheme = QStringLiteral("lieve-image");

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t),
                            float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

// Sizes of the headings, relative to the text (GitHub's).
qreal headingScale(int level)
{
    switch (level) {
    case 1:
        return 2.0;
    case 2:
        return 1.5;
    case 3:
        return 1.25;
    case 4:
        return 1.0;
    case 5:
        return 0.875;
    default:
        return 0.85;
    }
}

// Qt's importer counts HTML tags to know when an HTML block ends: a void
// element written the HTML way (<img ...>, <br>) never "closes", and all the
// text after it would be lost. They are written self-closing, outside code.
// <img align="right"> (a logo next to the title) floats, as on GitHub.
QString fixHtml(const QString &segment)
{
    static const QRegularExpression voidTag(QStringLiteral("<(img|br|hr|input|source|wbr)\\b([^>]*?)\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression align(QStringLiteral("\\balign\\s*=\\s*[\"']?(left|right)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression src(QStringLiteral("(\\bsrc\\s*=\\s*)([\"'])(.*?)\\2"), QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = voidTag.globalMatch(segment);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += segment.mid(last, m.capturedStart() - last);
        QString attributes = m.captured(2);
        if (m.captured(1).compare(QLatin1String("img"), Qt::CaseInsensitive) == 0) {
            const QRegularExpressionMatch side = align.match(attributes);
            const QRegularExpressionMatch source = src.match(attributes);
            if (side.hasMatch() && source.hasMatch()) {
                attributes.insert(source.capturedEnd(3), kFloatMarker + side.captured(1).toLower());
            }
        }
        out += QLatin1Char('<') + m.captured(1) + attributes + QStringLiteral(" />");
        last = m.capturedEnd();
    }
    return out + segment.mid(last);
}

QString prepareMarkdown(const QString &text)
{
    if (!text.contains(QLatin1Char('<'))) {
        return text;
    }
    QStringList lines = text.split(QLatin1Char('\n'));
    QString fence;
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!fence.isEmpty()) {
            if (trimmed.startsWith(fence) && trimmed.count(fence.at(0)) == trimmed.size()) {
                fence.clear();
            }
            continue;
        }
        if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
            qsizetype n = 0;
            while (n < trimmed.size() && trimmed.at(n) == trimmed.at(0)) {
                ++n;
            }
            fence = trimmed.left(n);
            continue;
        }
        if (!line.contains(QLatin1Char('<'))) {
            continue;
        }
        // Only outside `code spans` (runs of backticks of the same length).
        QString out;
        qsizetype i = 0;
        qsizetype plainStart = 0;
        while (i < line.size()) {
            if (line.at(i) != QLatin1Char('`')) {
                ++i;
                continue;
            }
            qsizetype run = 0;
            while (i + run < line.size() && line.at(i + run) == QLatin1Char('`')) {
                ++run;
            }
            const QString ticks(run, QLatin1Char('`'));
            const qsizetype close = line.indexOf(ticks, i + run);
            if (close < 0) {
                i += run;
                continue;
            }
            out += fixHtml(line.mid(plainStart, i - plainStart)) + line.mid(i, close + run - i);
            i = close + run;
            plainStart = i;
        }
        line = out + fixHtml(line.mid(plainStart));
    }
    return lines.join(QLatin1Char('\n'));
}

// Source lines of the headings, in order (ATX and setext, also in quotes),
// skipping fenced code.
std::vector<int> sourceHeadings(const QString &text)
{
    static const QRegularExpression atx(QStringLiteral("^#{1,6}(\\s|$)"));
    static const QRegularExpression html(QStringLiteral("^<h[1-6][\\s>]"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression setext(QStringLiteral("^(=+|-+)\\s*$"));
    static const QRegularExpression listItem(QStringLiteral("^([-*+]|\\d+[.)])\\s"));
    std::vector<int> headings;
    const QStringList lines = text.split(QLatin1Char('\n'));
    QString fence;
    QString previous; // the previous line, if it can be a setext heading's text
    for (qsizetype n = 0; n < lines.size(); ++n) {
        const QString &raw = lines.at(n);
        qsizetype indent = 0;
        while (indent < raw.size() && raw.at(indent) == QLatin1Char(' ')) {
            ++indent;
        }
        QString line = raw.trimmed();
        while (line.startsWith(QLatin1Char('>'))) { // in a quote
            line = line.mid(1).trimmed();
        }
        if (!fence.isEmpty()) {
            if (line.startsWith(fence) && line.count(fence.at(0)) == line.size()) {
                fence.clear();
            }
            previous.clear();
            continue;
        }
        if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
            qsizetype run = 0;
            while (run < line.size() && line.at(run) == line.at(0)) {
                ++run;
            }
            fence = line.left(run);
            previous.clear();
            continue;
        }
        if (indent >= 4) {
            previous.clear();
            continue;
        }
        if (atx.match(line).hasMatch() || html.match(line).hasMatch()) {
            headings.push_back(int(n));
            previous.clear();
            continue;
        }
        if (!previous.isEmpty() && setext.match(line).hasMatch()) {
            headings.push_back(int(n) - 1);
            previous.clear();
            continue;
        }
        previous = listItem.match(line).hasMatch() || line.startsWith(QLatin1Char('|')) ? QString() : line;
    }
    return headings;
}

// GitHub's anchor of a heading: "Install & run" -> "install--run".
QString slugOf(const QString &heading)
{
    QString slug;
    for (const QChar ch : heading.trimmed().toLower()) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_')) {
            slug += ch;
        } else if (ch.isSpace()) {
            slug += QLatin1Char('-');
        }
    }
    return slug;
}

bool isCodeLine(const QTextBlock &block)
{
    const QTextBlockFormat format = block.blockFormat();
    if (format.hasProperty(QTextFormat::BlockCodeFence) || format.hasProperty(QTextFormat::BlockCodeLanguage)) {
        return true;
    }
    // Indented code: monospace text that is not inline code.
    bool any = false;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextCharFormat cf = it.fragment().charFormat();
        if (cf.fontFixedPitch() || cf.isImageFormat() || !cf.fontFamilies().toStringList().contains(QLatin1String("monospace"))) {
            return false;
        }
        any = true;
    }
    return any;
}

bool isEmptyBlock(const QTextBlock &block)
{
    return block.length() <= 1;
}

// The image file a src refers to ("" for remote ones).
QString localImage(QString src, const QString &baseDir)
{
    const qsizetype marker = src.indexOf(kFloatMarker);
    if (marker >= 0) {
        src.truncate(marker);
    }
    const QUrl url(src);
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }
    if (url.scheme().size() > 1) { // http, https, data... (one letter would be a Windows drive)
        return {};
    }
    const QString path = QUrl::fromPercentEncoding(src.section(QLatin1Char('#'), 0, 0).section(QLatin1Char('?'), 0, 0).toUtf8());
    return path.isEmpty() ? QString() : QDir(baseDir).absoluteFilePath(path);
}

QFont editorFont()
{
    // As the editors have it (MainWindow::applySettings).
    QSettings &c = config();
    const QString family = c.value(QStringLiteral("editor/fontFamily")).toString().trimmed();
    const double size = c.value(QStringLiteral("editor/fontSize"), 0).toDouble();
    QFont font = KTextEditor::Editor::instance()->font();
    if (!family.isEmpty() || size > 0) {
        font = Lieve::Platform::fixedFont();
        if (!family.isEmpty()) {
            font.setFamily(family);
        }
        if (size > 0) {
            font.setPointSizeF(size);
        }
    }
    return font;
}
}

// ---------------------------------------------------------------------------
// MarkdownView

MarkdownView::MarkdownView(QWidget *parent)
    : QTextBrowser(parent)
{
    setFrameShape(QFrame::NoFrame);
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // everything wraps, long code lines too
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    // The editor's colours, as next to it (also with [ui] theme=system).
    const Colors &c = Theme::colors();
    QPalette p = palette();
    for (const auto group : {QPalette::Active, QPalette::Inactive}) {
        p.setColor(group, QPalette::Base, c.editor);
        p.setColor(group, QPalette::Window, c.editor);
        p.setColor(group, QPalette::Text, c.fg);
        p.setColor(group, QPalette::Link, c.accent);
        p.setColor(group, QPalette::LinkVisited, c.accent);
        p.setColor(group, QPalette::Highlight, c.selection);
        p.setColor(group, QPalette::HighlightedText, c.fg);
    }
    setPalette(p);

    // Body text in the interface font, a little larger, like VS Code's 14 px.
    m_bodyFont = QApplication::font();
    if (m_bodyFont.pointSizeF() > 0) {
        m_bodyFont.setPointSizeF(m_bodyFont.pointSizeF() * 1.08);
    } else {
        m_bodyFont.setPixelSize(14);
    }
    m_em = std::max(10, QFontInfo(m_bodyFont).pixelSize());
    m_codeFont = Lieve::Platform::fixedFont();
    QTextDocument *doc = document();
    doc->setDefaultFont(m_bodyFont);
    doc->setDocumentMargin(0);
    doc->setIndentWidth(std::round(1.75 * m_em));
    doc->setUndoRedoEnabled(false);

    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(150);
    connect(&m_resizeTimer, &QTimer::timeout, this, [this] {
        sizeImages();
        m_decorationsValid = false;
        viewport()->update();
    });
    connect(doc->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] {
        m_decorationsValid = false;
    });
    connect(this, &QTextBrowser::anchorClicked, this, &MarkdownView::openLink);
    connect(this, qOverload<const QUrl &>(&QTextBrowser::highlighted), this, [this](const QUrl &url) {
        setToolTip(url.isEmpty() || url.scheme() == kImageScheme ? QString() : url.toString());
    });
}

void MarkdownView::setCodeFont(const QFont &font)
{
    m_codeFont = font;
}

void MarkdownView::setMarkdownText(const QString &markdown, const QString &baseDir)
{
    m_baseDir = baseDir;
    QScrollBar *bar = verticalScrollBar();
    const int value = bar->value();
    QTextDocument *doc = document();
    doc->setMarkdown(prepareMarkdown(markdown), QTextDocument::MarkdownDialectGitHub);
    m_headingLines = sourceHeadings(markdown);
    applyStyle();
    updateColumn(true);
    doc->documentLayout()->documentSize(); // lays it out: the scroll range is right
    bar->setValue(value);
    m_decorationsValid = false;
    viewport()->update();
}

void MarkdownView::applyStyle()
{
    const Colors &c = Theme::colors();
    QTextDocument *doc = document();
    const qreal em = m_em;
    const qreal gap = std::round(0.8 * em); // below paragraphs, lists, code, tables (plus the line spacing)
    const qreal bodyPt = m_bodyFont.pointSizeF() > 0 ? m_bodyFont.pointSizeF() : 10.5;
    const qreal codePt = std::min(m_codeFont.pointSizeF() > 0 ? m_codeFont.pointSizeF() : bodyPt, bodyPt * 0.9);
    const qreal codePad = std::round(0.85 * em);
    const qreal quoteIndent = std::round(1.15 * em);

    std::vector<QTextBlock> blocks;
    std::vector<int> kinds;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        int kind = Paragraph;
        if (qobject_cast<QTextTable *>(doc->frameAt(block.position()))) {
            kind = TableCell;
        } else if (block.blockFormat().headingLevel() > 0) {
            kind = Heading;
        } else if (isCodeLine(block)) {
            kind = Code;
        } else if (block.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            kind = Rule;
        } else if (isEmptyBlock(block)) {
            kind = Empty;
        }
        blocks.push_back(block);
        kinds.push_back(kind);
    }
    // Blank lines inside indented code are code too.
    for (size_t i = 1; i < kinds.size(); ++i) {
        if (kinds[i] != Empty || kinds[i - 1] != Code) {
            continue;
        }
        size_t j = i;
        while (j < kinds.size() && kinds[j] == Empty) {
            ++j;
        }
        if (j < kinds.size() && kinds[j] == Code) {
            std::fill(kinds.begin() + qsizetype(i), kinds.begin() + qsizetype(j), int(Code));
        }
    }
    // Code blocks one after the other: a new box where fenced and indented
    // code meet, or the language changes.
    std::vector<int> runs(blocks.size(), 0);
    int run = 0;
    QString previousKind;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (kinds[i] != Code) {
            previousKind.clear();
            continue;
        }
        const QTextBlockFormat f = blocks[i].blockFormat();
        QString kind = f.hasProperty(QTextFormat::BlockCodeFence) || f.hasProperty(QTextFormat::BlockCodeLanguage)
            ? QStringLiteral("fenced:") + f.stringProperty(QTextFormat::BlockCodeLanguage)
            : QStringLiteral("indented");
        if (isEmptyBlock(blocks[i]) && !previousKind.isEmpty()) {
            kind = previousKind;
        }
        if (kind != previousKind) {
            ++run;
        }
        runs[i] = run;
        previousKind = kind;
    }
    // Bullets by depth, as on GitHub (Qt picks them by the character used);
    // task lists show their own boxes instead.
    for (const QTextBlock &block : blocks) {
        QTextList *list = block.textList();
        if (!list) {
            continue;
        }
        QTextListFormat lf = list->format();
        bool tasks = true;
        for (int i = 0; i < list->count() && tasks; ++i) {
            tasks = list->item(i).blockFormat().marker() != QTextBlockFormat::MarkerType::NoMarker;
        }
        QTextListFormat::Style style = lf.style();
        if (tasks) {
            style = QTextListFormat::ListStyleUndefined;
        } else if (style == QTextListFormat::ListDisc || style == QTextListFormat::ListCircle || style == QTextListFormat::ListSquare) {
            style = lf.indent() <= 1 ? QTextListFormat::ListDisc : lf.indent() == 2 ? QTextListFormat::ListCircle : QTextListFormat::ListSquare;
        }
        if (style != lf.style()) {
            lf.setStyle(style);
            list->setFormat(lf);
        }
    }

    struct Span {
        int start;
        int length;
        QTextCharFormat format;
    };
    std::vector<Span> spans;
    std::vector<std::pair<int, qreal>> pads; // where inline code needs its narrow spaces, and their size
    m_headingBlocks.clear();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (size_t i = 0; i < blocks.size(); ++i) {
        const QTextBlock &block = blocks[i];
        const int kind = kinds[i];
        QTextBlockFormat bf = block.blockFormat();
        const int quote = bf.intProperty(QTextFormat::BlockQuoteLevel);
        if (quote > 0) { // Qt indents quotes by 40 px a level
            bf.setLeftMargin(std::max<qreal>(0, bf.leftMargin() - 40 * quote) + quote * quoteIndent);
        }
        const bool firstOfRun = i == 0 || kinds[i - 1] != kind || runs[i - 1] != runs[i];
        const bool lastOfRun = i + 1 == blocks.size() || kinds[i + 1] != kind || runs[i + 1] != runs[i];
        const int level = bf.headingLevel();
        qreal pointSize = bodyPt;
        bf.setProperty(BlockKind, kind);
        // Line heights are relative to the font's own line spacing (about
        // 1.35 em for Noto Sans): 115% gives VS Code's 22 px at 14 px.
        switch (kind) {
        case Heading:
            pointSize = bodyPt * headingScale(level);
            bf.setTopMargin(i == 0 ? 0 : std::round(1.5 * em));
            // h1 and h2 have a thin rule under them (painted), as on GitHub.
            bf.setBottomMargin(level <= 2 ? std::round(0.3 * pointSize / bodyPt * em) + 1 + gap : gap);
            bf.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
            m_headingBlocks.push_back(block.blockNumber());
            break;
        case Code:
            bf.setNonBreakableLines(false);
            bf.setLeftMargin(bf.leftMargin() + codePad);
            bf.setRightMargin(codePad);
            // (margins collapse: a box right after another one needs both paddings)
            bf.setTopMargin(firstOfRun ? gap + (i > 0 && kinds[i - 1] == Code ? 2 : 1) * codePad : 0);
            bf.setBottomMargin(lastOfRun ? codePad + gap : 0);
            bf.setLineHeight(115, QTextBlockFormat::ProportionalHeight);
            bf.setProperty(CodeRun, runs[i]);
            break;
        case Rule:
            bf.clearProperty(QTextFormat::BlockTrailingHorizontalRulerWidth); // painted, in the theme's colour
            bf.setTopMargin(gap);
            bf.setBottomMargin(gap + 0.5 * em);
            bf.setLineHeight(em, QTextBlockFormat::FixedHeight);
            break;
        case Empty:
            bf.setTopMargin(0);
            bf.setBottomMargin(0);
            bf.setLineHeight(0, QTextBlockFormat::FixedHeight);
            break;
        case TableCell:
            bf.setTopMargin(0);
            bf.setBottomMargin(0);
            bf.setLineHeight(110, QTextBlockFormat::ProportionalHeight);
            break;
        default:
            bf.setTopMargin(0);
            if (QTextList *list = block.textList()) {
                // Items close together, the list as a whole spaced like a
                // paragraph: it ends where no item follows, or another list
                // starts at the same depth.
                const QTextBlock next = i + 1 < blocks.size() ? blocks[i + 1] : QTextBlock();
                const QTextList *nextList = next.isValid() && kinds[i + 1] == Paragraph ? next.textList() : nullptr;
                const bool lastItem = !nextList
                    || (nextList != list && nextList->itemNumber(next) == 0 && nextList->format().indent() <= list->format().indent());
                bf.setBottomMargin(lastItem ? gap : std::round(0.25 * em));
                if (bf.marker() != QTextBlockFormat::MarkerType::NoMarker) {
                    bf.setProperty(TaskState, bf.marker() == QTextBlockFormat::MarkerType::Checked ? 2 : 1);
                    if (list->format().style() == QTextListFormat::ListStyleUndefined) {
                        bf.setMarker(QTextBlockFormat::MarkerType::NoMarker); // the box is painted
                    }
                }
            } else {
                bf.setBottomMargin(gap);
            }
            // (not around a screenshot: the line is as tall as the image)
            bf.setLineHeight(block.text().contains(QChar::ObjectReplacementCharacter) ? 100 : 115, QTextBlockFormat::ProportionalHeight);
            break;
        }
        cursor.setPosition(block.position());
        cursor.setBlockFormat(bf);

        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            QTextCharFormat cf = fragment.charFormat();
            if (cf.isImageFormat()) {
                continue;
            }
            cf.clearProperty(QTextFormat::FontSizeAdjustment);
            const bool inlineCode = cf.fontFixedPitch() && kind != Code;
            if (kind == Code || inlineCode) {
                cf.setFontFamilies(QStringList{m_codeFont.family()});
                cf.setFontPointSize(kind == Heading ? pointSize * 0.9 : codePt);
                cf.setFontFixedPitch(true);
                if (inlineCode) {
                    cf.setProperty(CodeSpan, true);
                    pads.emplace_back(fragment.position(), pointSize);
                    pads.emplace_back(fragment.position() + fragment.length(), pointSize);
                }
            } else {
                cf.clearProperty(QTextFormat::FontFamilies);
                cf.setFontPointSize(pointSize);
            }
            if (kind == Heading) {
                cf.setFontWeight(QFont::DemiBold);
            }
            if (cf.isAnchor()) {
                cf.setForeground(c.accent);
                cf.setFontUnderline(false);
            } else if (quote > 0 || level == 6) {
                cf.setForeground(c.dim);
            } else {
                cf.clearForeground();
            }
            spans.push_back({fragment.position(), fragment.length(), cf});
        }
    }
    for (const Span &span : spans) {
        cursor.setPosition(span.start);
        cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(span.format);
    }
    // A narrow space on each side of inline code, inside its background
    // (from the end, so the positions still hold).
    std::sort(pads.begin(), pads.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    for (const auto &[position, size] : pads) {
        cursor.setPosition(position);
        QTextCharFormat pad = cursor.charFormat();
        pad.setProperty(CodeSpan, true);
        pad.clearProperty(QTextFormat::FontFamilies);
        pad.setFontFixedPitch(false);
        pad.setFontPointSize(size);
        pad.setAnchor(false);
        pad.clearProperty(QTextFormat::AnchorHref);
        cursor.insertText(QString(kCodePad), pad);
    }

    // Tables: thin borders, roomy cells (every other row is tinted when painted).
    const std::function<void(QTextFrame *)> styleTables = [&](QTextFrame *frame) {
        const QList<QTextFrame *> children = frame->childFrames();
        for (QTextFrame *child : children) {
            if (auto *table = qobject_cast<QTextTable *>(child)) {
                QTextTableFormat tf = table->format();
                tf.setBorder(1);
                tf.setBorderBrush(c.border);
                tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                tf.setBorderCollapse(true);
                tf.setCellSpacing(0);
                tf.setCellPadding(0);
                tf.setMargin(0);
                // The block before a table keeps its spacing (Qt drops it).
                const QTextBlock before = table->firstCursorPosition().block().previous();
                tf.setTopMargin(before.isValid() ? before.blockFormat().bottomMargin() : 0);
                tf.setBottomMargin(gap);
                table->setFormat(tf);
                for (int row = 0; row < table->rows(); ++row) {
                    for (int column = 0; column < table->columns(); ++column) {
                        QTextTableCell cell = table->cellAt(row, column);
                        QTextTableCellFormat format = cell.format().toTableCellFormat();
                        format.setTopPadding(std::round(0.4 * em));
                        format.setBottomPadding(std::round(0.25 * em));
                        format.setLeftPadding(std::round(0.9 * em));
                        format.setRightPadding(std::round(0.9 * em));
                        cell.setFormat(format);
                    }
                }
            }
            styleTables(child);
        }
    };
    styleTables(doc->rootFrame());
    cursor.endEditBlock();
}

// Images fit the column, scaled smoothly once (Qt would scale them roughly
// at every paint). Remote images are not downloaded: their text shows instead.
void MarkdownView::sizeImages()
{
    QTextDocument *doc = document();
    const qreal dpr = devicePixelRatioF();
    const Colors &c = Theme::colors();
    struct Change {
        int position;
        QTextImageFormat format;
        QString text; // replaces the image when it cannot be shown
        bool replace = false;
        QTextFrameFormat::Position floating = QTextFrameFormat::InFlow;
    };
    std::vector<Change> changes;
    QHash<QString, QImage> used;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.charFormat().isImageFormat()) {
                continue;
            }
            QTextImageFormat format = fragment.charFormat().toImageFormat();
            const QString src = format.hasProperty(ImageSource) ? format.stringProperty(ImageSource) : format.name();
            for (int i = 0; i < fragment.length(); ++i) {
                Change change{fragment.position() + i, format, QString()};
                const QString path = localImage(src, m_baseDir);
                QImageReader reader(path);
                QSize natural = path.isEmpty() ? QSize() : reader.size();
                if (!path.isEmpty() && !natural.isValid() && QFileInfo(path).isFile()) {
                    natural = QImageReader(path).read().size(); // formats that do not tell their size up front
                }
                if (!natural.isValid() || natural.isEmpty()) {
                    change.replace = true;
                    change.text = format.stringProperty(QTextFormat::ImageAltText);
                    changes.push_back(change);
                    continue;
                }
                QSizeF size = natural;
                const qreal w = format.width();
                const qreal h = format.height();
                if (w > 0 && h > 0) {
                    size = QSizeF(w, h);
                } else if (w > 0) {
                    size = QSizeF(w, natural.height() * w / natural.width());
                } else if (h > 0) {
                    size = QSizeF(natural.width() * h / natural.height(), h);
                }
                const QTextBlockFormat bf = block.blockFormat();
                const qreal indent = bf.leftMargin() + bf.indent() * doc->indentWidth() + (block.textList() ? block.textList()->format().indent() * doc->indentWidth() : 0);
                const qreal available = std::max<qreal>(32, m_column - indent);
                if (size.width() > available) {
                    size = QSizeF(available, size.height() * available / size.width());
                }
                const QSize pixels = (size * dpr).toSize().expandedTo(QSize(1, 1));
                const QString key = QStringLiteral("%1|%2|%3x%4")
                                        .arg(path)
                                        .arg(QFileInfo(path).lastModified().toMSecsSinceEpoch())
                                        .arg(pixels.width())
                                        .arg(pixels.height());
                QImage image = m_images.value(key);
                if (image.isNull()) {
                    reader.setScaledSize(pixels);
                    image = reader.read();
                    if (image.isNull()) {
                        image = QImageReader(path).read().scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    }
                    image.setDevicePixelRatio(dpr);
                }
                if (image.isNull()) {
                    change.replace = true;
                    change.text = format.stringProperty(QTextFormat::ImageAltText);
                    changes.push_back(change);
                    continue;
                }
                used.insert(key, image);
                QUrl name;
                name.setScheme(kImageScheme);
                name.setPath(QString::number(qHash(key), 16));
                doc->addResource(QTextDocument::ImageResource, name, image);
                change.format.setName(name.toString());
                change.format.setWidth(size.width());
                change.format.setHeight(size.height());
                change.format.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100)); // while a resize is pending
                change.format.setProperty(ImageSource, src);
                if (!format.hasProperty(ImageSource) && src.contains(kFloatMarker)) {
                    change.floating = src.endsWith(QLatin1String("left")) ? QTextFrameFormat::FloatLeft : QTextFrameFormat::FloatRight;
                }
                changes.push_back(change);
            }
        }
    }
    m_images = used;

    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    std::sort(changes.begin(), changes.end(), [](const Change &a, const Change &b) {
        return a.position > b.position; // from the end: replacing text moves what follows
    });
    for (const Change &change : changes) {
        cursor.setPosition(change.position);
        cursor.setPosition(change.position + 1, QTextCursor::KeepAnchor);
        if (change.replace) {
            QTextCharFormat text;
            text.setFontPointSize(m_bodyFont.pointSizeF());
            if (change.format.isAnchor()) { // a badge that links somewhere: its text is the link
                text.setAnchor(true);
                text.setAnchorHref(change.format.anchorHref());
                text.setForeground(c.accent);
            } else {
                text.setForeground(c.dim);
            }
            cursor.insertText(change.text, text);
        } else if (change.floating != QTextFrameFormat::InFlow) {
            // A logo floating next to the text. When Qt put it at the end of
            // the title, it goes to the start of the next paragraph, as on GitHub.
            cursor.removeSelectedText();
            QTextBlock block = cursor.block();
            if (block.blockFormat().headingLevel() > 0 && cursor.atBlockEnd() && block.next().isValid()) {
                cursor.setPosition(block.next().position());
            }
            cursor.insertImage(change.format, change.floating);
            cursor.setPosition(cursor.position() - 1);
            cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
            if (auto *frame = qobject_cast<QTextFrame *>(doc->object(cursor.charFormat().objectIndex()))) {
                QTextFrameFormat ff = frame->frameFormat();
                ff.setPosition(change.floating);
                ff.setMargin(0);
                (change.floating == QTextFrameFormat::FloatRight ? ff.setLeftMargin(m_em) : ff.setRightMargin(m_em));
                ff.setBottomMargin(0.5 * m_em);
                frame->setFrameFormat(ff);
            }
        } else {
            cursor.setCharFormat(change.format);
        }
    }
    cursor.endEditBlock();
}

qreal MarkdownView::sidePadding() const
{
    return std::round(1.85 * m_em); // VS Code's 26 px
}

// A readable column (about 64 em wide), centred, with VS Code's padding.
void MarkdownView::updateColumn(bool force)
{
    const qreal width = viewport()->width();
    const qreal side = std::max(sidePadding(), std::floor((width - 64 * m_em) / 2));
    QTextFrame *root = document()->rootFrame();
    QTextFrameFormat format = root->frameFormat();
    if (force || format.leftMargin() != side || format.rightMargin() != side) {
        format.setLeftMargin(side);
        format.setRightMargin(side);
        format.setTopMargin(std::round(1.1 * m_em));
        format.setBottomMargin(3 * m_em);
        root->setFrameFormat(format);
    }
    const int column = std::max(0, int(width - 2 * side));
    if (force) {
        m_column = column;
        m_resizeTimer.stop();
        sizeImages();
    } else if (column != m_column) {
        const bool first = m_column == 0 || !isVisible();
        m_column = column;
        if (first) {
            sizeImages();
        } else {
            m_resizeTimer.start();
        }
    }
    m_decorationsValid = false;
}

void MarkdownView::resizeEvent(QResizeEvent *event)
{
    QTextBrowser::resizeEvent(event);
    updateColumn(false);
}

QVariant MarkdownView::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource) {
        // Only the images sizeImages() added are shown; the others are
        // replaced right after the document is built.
        QImage blank(1, 1, QImage::Format_ARGB32_Premultiplied);
        blank.fill(Qt::transparent);
        return blank;
    }
    return {}; // nothing else is loaded (style sheets, other documents)
}

QMimeData *MarkdownView::createMimeDataFromSelection() const
{
    auto *data = new QMimeData;
    data->setText(textCursor().selection().toPlainText().remove(kCodePad));
    return data;
}

void MarkdownView::computeDecorations()
{
    m_decorations.clear();
    QTextDocument *doc = document();
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    layout->documentSize(); // the whole layout, before measuring it
    QTextFrame *root = doc->rootFrame();
    const QTextFrameFormat rootFormat = root->frameFormat();
    const QRectF rootRect = layout->frameBoundingRect(root);
    const qreal left = rootRect.left() + rootFormat.leftMargin();
    const qreal right = rootRect.right() - rootFormat.rightMargin();
    const qreal em = m_em;
    const qreal codePad = std::round(0.85 * em);
    const qreal quoteIndent = std::round(1.15 * em);
    const qreal barWidth = std::max<qreal>(3, std::round(0.25 * em));

    // Every other row of the tables tinted, under the text (and under its
    // inline code, which a cell background would cover).
    const std::function<void(QTextFrame *)> stripeTables = [&](QTextFrame *frame) {
        const QList<QTextFrame *> children = frame->childFrames();
        for (QTextFrame *child : children) {
            if (auto *table = qobject_cast<QTextTable *>(child)) {
                const auto rowTop = [&](int row) {
                    if (row >= table->rows()) { // the bottom of the last row
                        qreal bottom = 0;
                        for (int column = 0; column < table->columns(); ++column) {
                            const QTextTableCell cell = table->cellAt(row - 1, column);
                            const qreal padding = cell.format().toTableCellFormat().bottomPadding();
                            bottom = std::max(bottom, layout->blockBoundingRect(cell.lastCursorPosition().block()).bottom() + padding);
                        }
                        return bottom;
                    }
                    qreal top = std::numeric_limits<qreal>::max();
                    for (int column = 0; column < table->columns(); ++column) {
                        const QTextTableCell cell = table->cellAt(row, column);
                        const qreal padding = cell.format().toTableCellFormat().topPadding();
                        top = std::min(top, layout->blockBoundingRect(cell.firstCursorPosition().block()).top() - padding);
                    }
                    return top;
                };
                // Across the cells (the frame's rectangle can be wider).
                const QTextTableCell firstCell = table->cellAt(0, 0);
                const QTextTableCell lastCell = table->cellAt(0, table->columns() - 1);
                const qreal x1 = layout->blockBoundingRect(firstCell.firstCursorPosition().block()).left() - firstCell.format().toTableCellFormat().leftPadding();
                const qreal x2 = layout->blockBoundingRect(lastCell.firstCursorPosition().block()).right() + lastCell.format().toTableCellFormat().rightPadding();
                for (int row = 2; row < table->rows(); row += 2) {
                    m_decorations.push_back({Decoration::TableStripe, QRectF(QPointF(x1, rowTop(row)), QPointF(x2, rowTop(row + 1)))});
                }
            }
            stripeTables(child);
        }
    };
    stripeTables(root);

    QRectF code;
    std::vector<QRectF> bars; // the open quote bar of each level
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextLayout *tl = block.layout();
        const QTextBlockFormat bf = block.blockFormat();
        const int kind = bf.intProperty(BlockKind);
        if (!tl || tl->lineCount() == 0) {
            continue;
        }
        const QRectF rect = layout->blockBoundingRect(block);
        const QPointF origin = rect.topLeft(); // of the block's layout, in the document
        const QTextLine first = tl->lineAt(0);
        const QTextLine last = tl->lineAt(tl->lineCount() - 1);
        const qreal top = origin.y() + first.y();
        const qreal bottom = origin.y() + last.y() + last.ascent() + last.descent();
        const bool inTable = kind == TableCell;

        if (kind == Code) {
            if (code.isNull()) {
                code = QRectF(QPointF(left + bf.leftMargin() - codePad, top - codePad), QPointF(right, top));
            }
            code.setBottom(bottom + codePad);
            const QTextBlockFormat next = block.next().blockFormat();
            if (!block.next().isValid() || next.intProperty(BlockKind) != Code || next.intProperty(CodeRun) != bf.intProperty(CodeRun)) {
                m_decorations.push_back({Decoration::CodeBlock, code});
                code = QRectF();
            }
        } else if (kind == Heading && bf.headingLevel() <= 2) {
            const QFontMetricsF fm(block.begin().atEnd() ? m_bodyFont : block.begin().fragment().charFormat().font());
            const qreal y = std::round(bottom + 0.3 * fm.ascent());
            m_decorations.push_back({Decoration::Rule, QRectF(left, y, right - left, 1)});
        } else if (kind == Rule) {
            m_decorations.push_back({Decoration::Rule, QRectF(left, std::round(rect.center().y()), right - left, 1)});
        }
        if (const int task = bf.intProperty(TaskState); task > 0 && bf.marker() == QTextBlockFormat::MarkerType::NoMarker) {
            // In the list's indent, centred on the first line's lower-case letters.
            const qreal size = std::round(0.9 * em);
            const qreal centre = origin.y() + first.y() + first.ascent() - QFontMetricsF(m_bodyFont).xHeight() / 2;
            const QRectF box(std::round(origin.x() + first.x() - size - 0.5 * em), std::round(centre - size / 2), size, size);
            m_decorations.push_back({task == 2 ? Decoration::TaskDone : Decoration::TaskOpen, box});
        }

        // Quote bars, one per level, joined over the blocks of the quote.
        const int quote = inTable ? 0 : bf.intProperty(QTextFormat::BlockQuoteLevel);
        const qreal quoteTop = kind == Code ? top - codePad : top - 0.15 * em;
        const qreal quoteBottom = kind == Code ? bottom + codePad : bottom + 0.15 * em;
        for (int level = 0; level < std::max<int>(quote, int(bars.size())); ++level) {
            if (level >= quote) { // the quote ended
                if (!bars[level].isNull()) {
                    m_decorations.push_back({Decoration::QuoteBar, bars[level]});
                    bars[level] = QRectF();
                }
                continue;
            }
            if (bars.size() <= size_t(level)) {
                bars.resize(level + 1);
            }
            if (bars[level].isNull()) {
                bars[level] = QRectF(left + level * quoteIndent, quoteTop, barWidth, quoteBottom - quoteTop);
            } else {
                bars[level].setBottom(quoteBottom);
            }
        }

        if (kind == Code) {
            continue;
        }
        // Inline code: a rounded background on each line it spans.
        int start = -1;
        int end = -1;
        QFont font;
        const auto flush = [&] {
            if (start < 0) {
                return;
            }
            const QFontMetricsF fm(font);
            for (int i = 0; i < tl->lineCount(); ++i) {
                const QTextLine line = tl->lineAt(i);
                const int from = std::max(start, line.textStart());
                const int to = std::min(end, line.textStart() + line.textLength());
                if (from >= to) {
                    continue;
                }
                const qreal x1 = line.cursorToX(from);
                const qreal x2 = line.cursorToX(to);
                const qreal baseline = origin.y() + line.y() + line.ascent();
                m_decorations.push_back({Decoration::InlineCode,
                                         QRectF(origin.x() + std::min(x1, x2), baseline - fm.ascent() - 1, std::abs(x2 - x1), fm.ascent() + fm.descent() + 2)});
            }
            start = -1;
        };
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat cf = fragment.charFormat();
            if (!cf.hasProperty(CodeSpan)) {
                flush();
                continue;
            }
            const int from = fragment.position() - block.position();
            if (start < 0 || from != end) {
                flush();
                start = from;
            }
            end = from + fragment.length();
            if (cf.fontFixedPitch()) {
                font = cf.font();
            }
        }
        flush();
    }
    for (const QRectF &bar : bars) {
        if (!bar.isNull()) {
            m_decorations.push_back({Decoration::QuoteBar, bar});
        }
    }
    m_decorationsValid = true;
}

void MarkdownView::paintEvent(QPaintEvent *event)
{
    if (!m_decorationsValid) {
        computeDecorations();
    }
    const Colors &c = Theme::colors();
    const QColor codeBackground = mix(c.editor, c.fg, c.dark ? 0.075 : 0.055);
    const QColor bar = c.inputBorder;
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const QPointF offset(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        p.translate(offset);
        const QRectF visible = QRectF(event->rect()).translated(-offset).adjusted(-4, -4, 4, 4);
        for (const Decoration &d : m_decorations) {
            if (!d.rect.intersects(visible)) {
                continue;
            }
            switch (d.kind) {
            case Decoration::TableStripe:
                p.fillRect(d.rect, mix(c.editor, c.fg, c.dark ? 0.04 : 0.035));
                break;
            case Decoration::TaskOpen:
                p.setPen(QPen(c.inputBorder, 1));
                p.setBrush(c.input);
                p.drawRoundedRect(d.rect.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
                p.setPen(Qt::NoPen);
                break;
            case Decoration::TaskDone: {
                p.setBrush(c.accent);
                p.drawRoundedRect(d.rect, 3, 3);
                const QRectF r = d.rect;
                QPainterPath tick;
                tick.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.52);
                tick.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.70);
                tick.lineTo(r.left() + r.width() * 0.77, r.top() + r.height() * 0.32);
                p.setPen(QPen(c.accentText, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(tick);
                p.setPen(Qt::NoPen);
                break;
            }
            case Decoration::CodeBlock:
                p.setBrush(codeBackground);
                p.drawRoundedRect(d.rect, 6, 6);
                break;
            case Decoration::InlineCode:
                p.setBrush(codeBackground);
                p.drawRoundedRect(d.rect, 4, 4);
                break;
            case Decoration::QuoteBar:
                p.setBrush(bar);
                p.drawRoundedRect(d.rect, 1.5, 1.5);
                break;
            case Decoration::Rule:
                p.fillRect(d.rect, c.border);
                break;
            }
        }
    }
    QTextBrowser::paintEvent(event);
}

void MarkdownView::scrollToSourceLine(int line, int lineCount)
{
    QScrollBar *bar = verticalScrollBar();
    if (line <= 0 || lineCount <= 0) {
        bar->setValue(0);
        return;
    }
    QTextDocument *doc = document();
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    const qreal height = layout->documentSize().height();
    std::vector<std::pair<qreal, qreal>> anchors{{0, 0}}; // source line -> position in the preview
    if (m_headingLines.size() == m_headingBlocks.size()) {
        for (size_t i = 0; i < m_headingLines.size(); ++i) {
            const QTextBlock block = doc->findBlockByNumber(m_headingBlocks[i]);
            const qreal y = std::max<qreal>(0, layout->blockBoundingRect(block).top() - 0.5 * m_em);
            if (m_headingLines[i] > anchors.back().first && y >= anchors.back().second) {
                anchors.emplace_back(m_headingLines[i], y);
            }
        }
    }
    if (lineCount > anchors.back().first) {
        anchors.emplace_back(lineCount, height);
    }
    qreal y = anchors.back().second;
    for (size_t i = 0; i + 1 < anchors.size(); ++i) {
        const auto &[l0, y0] = anchors[i];
        const auto &[l1, y1] = anchors[i + 1];
        if (line >= l0 && line < l1) {
            y = y0 + (y1 - y0) * (line - l0) / (l1 - l0);
            break;
        }
    }
    bar->setValue(int(std::round(y)));
}

bool MarkdownView::scrollToHeading(const QString &slug)
{
    QTextDocument *doc = document();
    QHash<QString, int> seen; // GitHub numbers repeated headings: "usage", "usage-1"
    for (const int number : std::as_const(m_headingBlocks)) {
        const QTextBlock block = doc->findBlockByNumber(number);
        QString anchor = slugOf(block.text().remove(kCodePad).remove(QChar::ObjectReplacementCharacter));
        const int n = seen.value(anchor);
        seen.insert(anchor, n + 1);
        if (n > 0) {
            anchor += QStringLiteral("-%1").arg(n);
        }
        if (anchor == slug.toLower()) {
            const qreal y = doc->documentLayout()->blockBoundingRect(block).top() - 0.5 * m_em;
            verticalScrollBar()->setValue(int(std::max<qreal>(0, y)));
            return true;
        }
    }
    return false;
}

void MarkdownView::openLink(const QUrl &url)
{
    if (url.scheme().isEmpty() && url.path().isEmpty() && url.hasFragment()) { // #a-heading
        scrollToHeading(url.fragment(QUrl::FullyDecoded));
        return;
    }
    if (!url.scheme().isEmpty() && !url.isLocalFile() && url.scheme().size() > 1) { // https:, mailto:...
        QDesktopServices::openUrl(url);
        return;
    }
    const QString path = url.isLocalFile() ? url.toLocalFile() : QDir(m_baseDir).absoluteFilePath(url.path(QUrl::FullyDecoded));
    const QFileInfo info(path);
    if (info.isFile() && QMimeDatabase().mimeTypeForFile(info).inherits(QStringLiteral("text/plain"))) {
        Q_EMIT openFileRequested(info.absoluteFilePath()); // another Markdown file, a source file: in Lieve
    } else if (info.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath())); // images, folders, PDFs...
    }
}

// ---------------------------------------------------------------------------
// MarkdownPreview

MarkdownPreview::MarkdownPreview(KTextEditor::Document *doc, QWidget *parent)
    : MarkdownView(parent)
    , m_doc(doc)
{
    setCodeFont(editorFont());
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(300);
    connect(&m_renderTimer, &QTimer::timeout, this, &MarkdownPreview::refresh);
    // Rendered again when typing pauses; a hidden preview waits until it is shown.
    connect(doc, &KTextEditor::Document::textChanged, this, [this] {
        m_dirty = true;
        if (isVisible()) {
            m_renderTimer.start();
        }
    });
    connect(doc, &KTextEditor::Document::documentUrlChanged, this, [this] {
        m_dirty = true;
        if (isVisible()) {
            m_renderTimer.start();
        }
        Q_EMIT titleChanged();
    });
    connect(doc, &KTextEditor::Document::documentNameChanged, this, &MarkdownPreview::titleChanged);
    connect(doc, &KTextEditor::Document::viewCreated, this, [this](KTextEditor::Document *, KTextEditor::View *view) {
        watch(view);
    });
    const QList<KTextEditor::View *> views = doc->views();
    for (KTextEditor::View *view : views) {
        watch(view);
    }
}

KTextEditor::Document *MarkdownPreview::source() const
{
    return m_doc;
}

QString MarkdownPreview::title() const
{
    return tr("Preview %1").arg(m_doc ? m_doc->documentName() : QString());
}

bool MarkdownPreview::isMarkdown(const KTextEditor::Document *doc)
{
    if (!doc) {
        return false;
    }
    static const QStringList suffixes = {QStringLiteral("md"),
                                         QStringLiteral("markdown"),
                                         QStringLiteral("mdown"),
                                         QStringLiteral("mkd"),
                                         QStringLiteral("mkdn"),
                                         QStringLiteral("mdwn")};
    return suffixes.contains(QFileInfo(doc->url().path()).suffix().toLower()) || doc->highlightingMode() == QLatin1String("Markdown");
}

// Like VS Code's "open preview" icon: a page split in two, a lens on the
// right half; drawn in the colours of the window's other glyphs.
QIcon MarkdownPreview::icon()
{
    static const QIcon icon = [] {
        const Colors &c = Theme::colors();
        const QColor fg = c.fg.isValid() ? c.fg : QApplication::palette().color(QPalette::WindowText);
        const QColor normal = c.fg.isValid() ? mix(c.fg, c.base, 0.22) : fg;
        QIcon result;
        for (const int dpr : {1, 2}) {
            for (const auto mode : {QIcon::Normal, QIcon::Active}) {
                QPixmap pixmap(16 * dpr, 16 * dpr);
                pixmap.setDevicePixelRatio(dpr);
                pixmap.fill(Qt::transparent);
                QPainter p(&pixmap);
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(QPen(mode == QIcon::Active ? fg : normal, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(QRectF(1.5, 2.5, 13, 11), 1.5, 1.5);
                p.drawLine(QPointF(7, 2.5), QPointF(7, 13.5));
                p.drawEllipse(QPointF(10.4, 7.4), 1.9, 1.9);
                p.drawLine(QPointF(11.8, 8.8), QPointF(12.9, 9.9));
                p.end();
                result.addPixmap(pixmap, mode);
            }
        }
        return result;
    }();
    return icon;
}

void MarkdownPreview::refresh()
{
    m_renderTimer.stop();
    if (!m_doc) {
        return;
    }
    m_dirty = false;
    const QString path = m_doc->url().isLocalFile() ? m_doc->url().toLocalFile() : QString();
    setMarkdownText(m_doc->text(), path.isEmpty() ? QDir::homePath() : QFileInfo(path).absolutePath());
}

void MarkdownPreview::showEvent(QShowEvent *event)
{
    MarkdownView::showEvent(event);
    if (m_dirty) {
        refresh();
    }
    if (!m_shown && m_doc && !m_doc->views().isEmpty()) {
        // Opened: at the place the editor shows (the one on screen, else the
        // one this preview replaced in its group).
        m_shown = true;
        const QList<KTextEditor::View *> views = m_doc->views();
        const auto visible = std::find_if(views.cbegin(), views.cend(), [](KTextEditor::View *view) {
            return view->isVisible();
        });
        follow(visible != views.cend() ? *visible : views.first());
    }
}

void MarkdownPreview::focusInEvent(QFocusEvent *event)
{
    MarkdownView::focusInEvent(event);
    Q_EMIT focused();
}

void MarkdownPreview::watch(KTextEditor::View *view)
{
    connect(view, &KTextEditor::View::verticalScrollPositionChanged, this, [this, view] {
        if (isVisible() && view->isVisible()) {
            follow(view);
        }
    });
}

// Roughly the same place as the editor: its first line, matched by headings.
void MarkdownPreview::follow(KTextEditor::View *view)
{
    if (!m_doc) {
        return;
    }
    const int lines = m_doc->lines();
    if (view->firstDisplayedLine() > 0 && view->lastDisplayedLine() >= lines - 1) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        return;
    }
    scrollToSourceLine(view->firstDisplayedLine(), lines);
}
