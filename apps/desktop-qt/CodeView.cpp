#include "CodeView.h"

#include <algorithm>
#include <vector>

#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QScrollBar>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>

using codetrace::Language;

namespace {

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeView* v) : QWidget(v), view_(v) {}
    QSize sizeHint() const override { return {view_->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* e) override { view_->paintLineNumbers(e); }

private:
    CodeView* view_;
};

class Highlighter : public QSyntaxHighlighter {
public:
    Highlighter(QTextDocument* doc, Language lang) : QSyntaxHighlighter(doc) {
        QStringList kw;
        QString comment;
        switch (lang) {
            case Language::Cpp:
                kw = {"alignas", "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr",
                      "continue", "default", "delete", "do", "double", "else", "enum", "explicit", "extern", "false",
                      "float", "for", "friend", "if", "inline", "int", "long", "namespace", "new", "noexcept",
                      "nullptr", "operator", "override", "private", "protected", "public", "return", "short",
                      "signed", "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try",
                      "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while"};
                comment = "//[^\n]*";
                break;
            case Language::Python:
                kw = {"False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue",
                      "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import",
                      "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while",
                      "with", "yield", "self"};
                comment = "#[^\n]*";
                break;
            case Language::JavaScript:
                kw = {"async", "await", "break", "case", "catch", "class", "const", "continue", "default", "delete",
                      "do", "else", "export", "extends", "false", "finally", "for", "from", "function", "if",
                      "import", "in", "instanceof", "let", "new", "null", "return", "static", "super", "switch",
                      "this", "throw", "true", "try", "typeof", "undefined", "var", "void", "while", "yield"};
                comment = "//[^\n]*";
                break;
            default: break;
        }
        QTextCharFormat kwf;
        kwf.setForeground(QColor(0x7c, 0x3a, 0xed));
        kwf.setFontWeight(QFont::DemiBold);
        if (!kw.isEmpty()) rules_.push_back({QRegularExpression("\\b(" + kw.join('|') + ")\\b"), kwf});
        QTextCharFormat num;
        num.setForeground(QColor(0xb4, 0x53, 0x09));
        rules_.push_back({QRegularExpression("\\b[0-9][0-9a-fA-FxX.']*\\b"), num});
        QTextCharFormat call;
        call.setForeground(QColor(0x1d, 0x4e, 0xd8));
        rules_.push_back({QRegularExpression("\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()"), call});
        QTextCharFormat str;
        str.setForeground(QColor(0x15, 0x80, 0x3d));
        rules_.push_back({QRegularExpression(R"("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')"), str});
        if (lang == Language::Cpp) {
            QTextCharFormat pp;
            pp.setForeground(QColor(0x9d, 0x17, 0x4d));
            rules_.push_back({QRegularExpression("^\\s*#\\s*\\w+[^\n]*"), pp});
        }
        QTextCharFormat cf;
        cf.setForeground(QColor(0x6b, 0x72, 0x80));
        cf.setFontItalic(true);
        if (!comment.isEmpty()) rules_.push_back({QRegularExpression(comment), cf});
        blockComments_ = lang == Language::Cpp || lang == Language::JavaScript;
        commentFormat_ = cf;
    }

protected:
    void highlightBlock(const QString& text) override {
        for (const auto& r : rules_) {
            auto it = r.re.globalMatch(text);
            while (it.hasNext()) {
                auto m = it.next();
                setFormat(static_cast<int>(m.capturedStart()), static_cast<int>(m.capturedLength()), r.format);
            }
        }
        if (!blockComments_) return;
        setCurrentBlockState(0);
        int start = previousBlockState() == 1 ? 0 : static_cast<int>(text.indexOf("/*"));
        while (start >= 0) {
            int end = static_cast<int>(text.indexOf("*/", start));
            int len;
            if (end < 0) {
                setCurrentBlockState(1);
                len = static_cast<int>(text.length()) - start;
            } else {
                len = end - start + 2;
            }
            setFormat(start, len, commentFormat_);
            start = static_cast<int>(text.indexOf("/*", start + len));
        }
    }

private:
    struct Rule {
        QRegularExpression re;
        QTextCharFormat format;
    };
    std::vector<Rule> rules_;
    bool blockComments_ = false;
    QTextCharFormat commentFormat_;
};

}  // namespace

CodeView::CodeView(QWidget* parent) : QPlainTextEdit(parent), lineArea_(new LineNumberArea(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(lineNumberAreaWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy) lineArea_->scroll(0, dy);
        else lineArea_->update(0, rect.y(), lineArea_->width(), rect.height());
    });
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    setPlaceholderText(tr("Select a function to view its source."));
}

int CodeView::lineNumberAreaWidth() const {
    int digits = QString::number(std::max(1, blockCount())).length();
    return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeView::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    lineArea_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeView::paintLineNumbers(QPaintEvent* event) {
    QPainter p(lineArea_);
    p.fillRect(event->rect(), QColor(0xf3, 0xf4, 0xf6));
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            bool marked = number + 1 == markedLine_;
            p.setPen(marked ? QColor(0x11, 0x18, 0x27) : QColor(0x9c, 0xa3, 0xaf));
            QFont f = font();
            f.setBold(marked);
            p.setFont(f);
            p.drawText(0, top, lineArea_->width() - 8, fontMetrics().height(), Qt::AlignRight, QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

bool CodeView::showLocation(const QString& path, int line, Language lang) {
    if (path != file_) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            setPlainText(tr("Cannot open %1: %2").arg(path, f.errorString()));
            file_.clear();
            return false;
        }
        QByteArray data = f.read(8 * 1024 * 1024);
        setPlainText(QString::fromUtf8(data));
        delete highlighter_;
        highlighter_ = new Highlighter(document(), lang);
        file_ = path;
    }
    highlightLine(line);
    return true;
}

void CodeView::reloadIfShowing(const QString& path) {
    if (path != file_) return;
    int line = markedLine_;
    int scroll = verticalScrollBar()->value();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    setPlainText(QString::fromUtf8(f.read(8 * 1024 * 1024)));
    highlightLine(line);
    verticalScrollBar()->setValue(scroll);
}

void CodeView::highlightLine(int line) {
    markedLine_ = line;
    QList<QTextEdit::ExtraSelection> extras;
    QTextBlock block = document()->findBlockByNumber(std::max(0, line - 1));
    if (block.isValid() && line > 0) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(QColor(0xfe, 0xf0, 0x8a));
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = QTextCursor(block);
        extras << sel;
        QTextCursor c(block);
        setTextCursor(c);
        centerCursor();
    }
    setExtraSelections(extras);
    lineArea_->update();
}

void CodeView::mouseDoubleClickEvent(QMouseEvent* e) {
    QPlainTextEdit::mouseDoubleClickEvent(e);
    emit lineActivated(file_, textCursor().blockNumber() + 1);
}
