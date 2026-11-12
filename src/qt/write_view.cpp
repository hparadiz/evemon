extern "C" {
#include "../ipc/query.h"
}
#include "write_view.h"
#include "flow_layout.h"
#include <QDateTime>
#include <QFontDatabase>
#include <QVBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QKeyEvent>
#include <algorithm>
#ifdef HAVE_QT_SYNTAX
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#endif
#include <QScrollBar>
#include <QTextBlock>
#include <QWheelEvent>
// The log highlighter uses only Qt. Extra code-language definitions are optional.
class LogHighlighter : public QSyntaxHighlighter {
public:
    LogHighlighter(QTextDocument *document, const QPalette &palette) : QSyntaxHighlighter(document)
    {
        const bool dark = palette.color(QPalette::Base).lightness() < 128;
        const auto rule = [this](const QString &pattern, const QColor &color, bool bold = false) {
            QTextCharFormat format; format.setForeground(color);
            if (bold) format.setFontWeight(QFont::Bold);
            rules.append({QRegularExpression(pattern), format});
        };
        rule(QStringLiteral(R"(\b(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?|true|false|null)\b)"), dark ? QColor("#d6a7ff") : QColor("#773b9e"));
        rule(QStringLiteral(R"(\b\d{2}:\d{2}:\d{2}(?:\.\d+)?\b)"), dark ? QColor("#7cc4e8") : QColor("#246480"));
        rule(QStringLiteral(R"(\b(?:DEBUG|TRACE|INFO|NOTICE)\b)"), dark ? QColor("#82c99a") : QColor("#276b3f"), true);
        rule(QStringLiteral(R"(\b(?:WARN(?:ING)?|ERROR|FATAL|CRITICAL)\b)"), dark ? QColor("#ffb080") : QColor("#a13e18"), true);
        rule(QStringLiteral(R"regex("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')regex"), dark ? QColor("#ddc486") : QColor("#795814"));
        rule(QStringLiteral(R"regex("(?:\\.|[^"\\])*"(?=\s*:))regex"), dark ? QColor("#85c6dd") : QColor("#22667d"));
    }
protected:
    void highlightBlock(const QString &text) override
    {
        for (const auto &rule : rules) {
            auto matches = rule.first.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                setFormat(int(match.capturedStart()), int(match.capturedLength()), rule.second);
            }
        }
    }
private:
    QList<QPair<QRegularExpression, QTextCharFormat>> rules;
};
#ifdef HAVE_QT_SYNTAX
static KSyntaxHighlighting::Repository &syntaxRepository()
{
    static KSyntaxHighlighting::Repository repository;
    return repository;
}
#endif
class WriteHighlighting {
public:
    explicit WriteHighlighting(WriteText *view) : view(view) { select(QStringLiteral("Log")); }
    void select(const QString &name)
    {
        mode = name;
        highlighter.reset();
        if (name == QStringLiteral("Log"))
            highlighter = std::make_unique<LogHighlighter>(view->document(), view->palette());
#ifdef HAVE_QT_SYNTAX
        else if (name != QStringLiteral("Plain Text")) {
            auto code = std::make_unique<KSyntaxHighlighting::SyntaxHighlighter>(view->document());
            auto &repository = syntaxRepository();
            code->setDefinition(repository.definitionForName(name));
            code->setTheme(repository.defaultTheme(view->palette().color(QPalette::Base).lightness() < 128
                ? KSyntaxHighlighting::Repository::DarkTheme : KSyntaxHighlighting::Repository::LightTheme));
            highlighter = std::move(code);
        }
#endif
        if (highlighter) highlighter->rehighlight();
    }
    QString mode;
private:
    WriteText *view;
    std::unique_ptr<QSyntaxHighlighter> highlighter;
};
class WriteGutter : public QWidget {
public:
    explicit WriteGutter(WriteText *text) : QWidget(text), text(text) {}
protected:
    void paintEvent(QPaintEvent *event) override { text->paintGutter(event); }
private:
    WriteText *text;
};
WriteText::WriteText(QWidget *parent) : QPlainTextEdit(parent), gutter(new WriteGutter(this))
{
    setReadOnly(true); setUndoRedoEnabled(false);
    setLineWrapMode(WidgetWidth); setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setFrameShape(QFrame::NoFrame);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); gutter->hide();
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    highlighting = std::make_unique<WriteHighlighting>(this);
    connect(this, &QPlainTextEdit::updateRequest, gutter, [this](const QRect &, int) { gutter->update(); });
    connect(verticalScrollBar(), &QScrollBar::sliderMoved, this, [this](int value) {
        if (value < verticalScrollBar()->maximum() && scrolledUp) scrolledUp();
    });
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int action) {
        if ((action == QAbstractSlider::SliderSingleStepSub || action == QAbstractSlider::SliderPageStepSub ||
             action == QAbstractSlider::SliderToMinimum) && scrolledUp) scrolledUp();
    });
}
WriteText::~WriteText() = default;
void WriteText::updateGutterGeometry()
{
    const int desired = fontMetrics().horizontalAdvance(QStringLiteral("MMMMMMMMMMMMMMMM  9999999999 │ 23:59:59.999")) + 12;
    const int textMinimum = fontMetrics().horizontalAdvance(QStringLiteral("MMMMMMMMMMMM"));
    const int width = gutterVisible ? std::min(desired, std::max(0, contentsRect().width() - textMinimum)) : 0;
    setViewportMargins(width, 0, 0, 0);
    gutter->setGeometry(contentsRect().left(), contentsRect().top(), width, contentsRect().height());
    gutter->update();
}
void WriteText::showGutter(bool visible)
{
    gutterVisible = visible; gutter->setVisible(visible); updateGutterGeometry();
}
void WriteText::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event); updateGutterGeometry();
}
void WriteText::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4); updateGutterGeometry();
    }
    if (event->type() == QEvent::PaletteChange && highlighting) highlighting->select(highlighting->mode);
}
void WriteText::wheelEvent(QWheelEvent *event)
{
    if ((event->angleDelta().y() > 0 || event->pixelDelta().y() > 0) && scrolledUp) scrolledUp();
    QPlainTextEdit::wheelEvent(event);
}
void WriteText::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_PageUp || event->key() == Qt::Key_Home) && scrolledUp) scrolledUp();
    QPlainTextEdit::keyPressEvent(event);
}
void WriteText::scrollToBottom() { verticalScrollBar()->setValue(verticalScrollBar()->maximum()); }
void WriteText::clear()
{
    entries.clear(); QPlainTextEdit::clear(); gutter->update();
}
QStringList WriteText::syntaxNames() const
{
    QStringList names{QStringLiteral("Log"), QStringLiteral("Plain Text")};
#ifdef HAVE_QT_SYNTAX
    QStringList languages;
    for (const auto &definition : syntaxRepository().definitions())
        if (!definition.isHidden() && !names.contains(definition.name())) languages.append(definition.name());
    languages.sort(Qt::CaseInsensitive); names.append(languages);
#endif
    return names;
}
void WriteText::setSyntax(const QString &name) { highlighting->select(name); }
void WriteText::appendEvent(const QString &value, int pid, const QString &name, bool scroll)
{
    const int previous = verticalScrollBar()->value();
    const int horizontal = horizontalScrollBar()->value();
    QTextCursor cursor(document()); cursor.movePosition(QTextCursor::End);
    const int startBlock = cursor.blockNumber(); cursor.insertText(value);
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    // Syntax highlighters own QTextBlockUserData. Keep capture metadata separate,
    // including a final partial line that has not received its newline yet.
    for (QTextBlock block = document()->findBlockByNumber(startBlock); block.isValid(); block = block.next()) {
        if ((block.blockNumber() < cursor.blockNumber() || !block.text().isEmpty()) && !entries.contains(block.blockNumber()))
            entries.insert(block.blockNumber(), {pid, name.isEmpty() ? QStringLiteral("[unknown]") : name.left(16), time});
    }
    verticalScrollBar()->setValue(scroll ? verticalScrollBar()->maximum() : previous);
    horizontalScrollBar()->setValue(horizontal); gutter->update();
}
void WriteText::paintGutter(QPaintEvent *event)
{
    QPainter painter(gutter); painter.fillRect(event->rect(), palette().alternateBase()); painter.setFont(font());
    const QColor label = palette().color(QPalette::Text);
    const QColor dim = palette().color(QPalette::PlaceholderText);
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        const QRectF rect = blockBoundingGeometry(block).translated(contentOffset());
        if (rect.top() > event->rect().bottom()) break;
        const auto entry = entries.constFind(block.blockNumber());
        if (entry == entries.cend() || !block.isVisible() || rect.bottom() < event->rect().top()) continue;
        int x = 6; const int y = qRound(rect.top()) + fontMetrics().ascent();
        const QString full = entry->name.leftJustified(16) + "  " + QString::number(entry->pid).rightJustified(10) + " │ " + entry->time;
        if (fontMetrics().horizontalAdvance(full) > gutter->width() - 12) {
            painter.setPen(label);
            painter.drawText(x, y, fontMetrics().elidedText(full, Qt::ElideLeft, std::max(0, gutter->width() - 12)));
            continue;
        }
        const QList<QPair<QString, QColor>> parts{{entry->name.leftJustified(16), label}, {"  ", dim},
            {QString::number(entry->pid).rightJustified(10), dim}, {" │ ", dim}, {entry->time, label}};
        for (const auto &part : parts) { painter.setPen(part.second); painter.drawText(x, y, part.first); x += fontMetrics().horizontalAdvance(part.first); }
    }
}
WriteView::WriteView(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(6, 6, 6, 6); layout->setSpacing(6);
    auto *bar = new FlowLayout; bar->addWidget(new QLabel("Monitor writes to:", this));
    destination = new QComboBox(this); destination->addItems({"None", "STDOUT (fd 1)", "STDERR (fd 2)", "Both"}); bar->addWidget(destination);
    descendants = new QCheckBox("Include descendants", this); gutter = new QCheckBox("Show gutter", this);
    autoScroll = new QCheckBox("Auto-scroll", this); autoScroll->setChecked(true);
    excludeKernel = new QCheckBox("Exclude kernel", this); excludeKernel->setChecked(true);
    for (auto *check : {descendants, gutter, autoScroll, excludeKernel}) bar->addWidget(check);
    layout->addLayout(bar);
    text = new WriteText(this);
    auto *codeBar = new FlowLayout;
    codeBar->addWidget(new QLabel("Syntax:", this));
    auto *syntax = new QComboBox(this); syntax->addItems(text->syntaxNames()); syntax->setMaxVisibleItems(20);
    syntax->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); syntax->setMinimumContentsLength(14);
    syntax->setToolTip("Log highlighting is always available. Additional code languages use KDE SyntaxHighlighting when installed.");
    codeBar->addWidget(syntax);
    auto *wrap = new QCheckBox("Wrap lines", this); wrap->setChecked(true); codeBar->addWidget(wrap);
    auto *clearButton = new QPushButton("Clear", this); codeBar->addWidget(clearButton);
    layout->addLayout(codeBar);
    captureStatus = new QLabel(this); captureStatus->setWordWrap(true); captureStatus->hide(); layout->addWidget(captureStatus);
    layout->addWidget(text, 1);
    connect(destination, &QComboBox::currentIndexChanged, this, [this] { if (changed) changed(); });
    connect(descendants, &QCheckBox::toggled, this, [this] { if (changed) changed(); });
    connect(gutter, &QCheckBox::toggled, text, &WriteText::showGutter);
    connect(autoScroll, &QCheckBox::toggled, text, [this](bool enabled) { if (enabled) text->scrollToBottom(); });
    connect(syntax, &QComboBox::currentTextChanged, text, &WriteText::setSyntax);
    connect(wrap, &QCheckBox::toggled, text, [this](bool enabled) {
        text->setLineWrapMode(enabled ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        if (autoScroll->isChecked()) text->scrollToBottom();
    });
    connect(clearButton, &QPushButton::clicked, this, &WriteView::clear);
    text->scrolledUp = [this] { autoScroll->setChecked(false); };
}
void WriteView::clear() { text->clear(); }
void WriteView::setCaptureAvailable(bool available)
{
    destination->setEnabled(available); descendants->setEnabled(available);
    captureStatus->setText(available ? QString() : QStringLiteral("Write capture is unavailable on this monitor: an active eBPF backend is required."));
    captureStatus->setVisible(!available);
}
void WriteView::append(const evemon_fd_write_payload_t &write)
{
    if (write.fd < 1 || write.fd > 2 || !(mask() & (1 << (write.fd - 1)))) return;
    proc_entry_t entry{}; char comm[256];
    const bool known = query_cached_process(write.tgid, &entry, comm, sizeof(comm));
    const QString name = QString::fromUtf8(comm);
    if (excludeKernel->isChecked() && known && write.tgid != 1 &&
        (entry.ppid == 0 || name.startsWith('['))) return;
    // Preserve the original Write Log sanitization and event boundaries.
    QByteArray clean; int escape = 0;
    for (size_t i = 0; i < std::min(write.len, sizeof(write.data)); ++i) {
        const auto ch = static_cast<unsigned char>(write.data[i]);
        if (escape == 1) { escape = ch == '[' ? 2 : 0; continue; }
        if (escape == 2) { if (ch >= 0x40 && ch <= 0x7e) escape = 0; continue; }
        if (ch == 0x1b) { escape = 1; continue; }
        if (ch == '\n' || ch == '\t' || (ch >= 32 && ch < 127)) clean.append(char(ch));
    }
    if (!clean.isEmpty()) text->appendEvent(QString::fromLatin1(clean), write.tgid, name, autoScroll->isChecked());
}
