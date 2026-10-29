extern "C" {
#include "../ipc/query.h"
}
#include "write_view.h"
#include "process_model.h"
#include "flow_layout.h"
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QWheelEvent>
class WriteEntry : public QTextBlockUserData {
public:
    int pid;
    QString name, time;
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
    setReadOnly(true); setLineWrapMode(WidgetWidth); setFrameShape(QFrame::NoFrame);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); gutter->hide();
    connect(this, &QPlainTextEdit::updateRequest, gutter, [this](const QRect &, int) { gutter->update(); });
}
void WriteText::showGutter(bool visible) { gutterVisible = visible; setViewportMargins(visible ? 145 : 0, 0, 0, 0); gutter->setVisible(visible); gutter->setGeometry(0, 0, 145, height()); }
void WriteText::resizeEvent(QResizeEvent *event) { QPlainTextEdit::resizeEvent(event); gutter->setGeometry(0, 0, 145, height()); }
void WriteText::wheelEvent(QWheelEvent *event) { if (event->angleDelta().y() > 0 && scrolledUp) scrolledUp(); QPlainTextEdit::wheelEvent(event); }
void WriteText::appendEvent(const QString &value, int pid, const QString &name, bool scroll)
{
    const int previous = verticalScrollBar()->value();
    QTextCursor cursor(document()); cursor.movePosition(QTextCursor::End);
    const int startBlock = cursor.blockNumber(); cursor.insertText(value);
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    for (QTextBlock block = document()->findBlockByNumber(startBlock); block.isValid() && block.blockNumber() < cursor.blockNumber(); block = block.next()) {
        auto *entry = new WriteEntry; entry->pid = pid; entry->name = name.left(16); entry->time = time; block.setUserData(entry);
    }
    verticalScrollBar()->setValue(scroll ? verticalScrollBar()->maximum() : previous); gutter->update();
}
void WriteText::paintGutter(QPaintEvent *)
{
    QPainter painter(gutter); painter.fillRect(gutter->rect(), palette().base()); painter.setFont(font());
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        const QRectF rect = blockBoundingGeometry(block).translated(contentOffset());
        if (rect.top() > height()) break;
        const auto *entry = static_cast<WriteEntry *>(block.userData()); if (!entry || !block.isVisible()) continue;
        int x = 4; const int y = qRound(rect.top()) + fontMetrics().ascent();
        const QList<QPair<QString, QColor>> parts{{entry->name.leftJustified(16), QColor("#7a8499")}, {"  ", QColor("#2e3a4a")}, {QString::number(entry->pid).rightJustified(7), QColor("#556070")}, {" │ ", QColor("#3d4a5c")}, {entry->time, QColor("#7a8499")}};
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
    text = new WriteText(this); layout->addWidget(text, 1);
    connect(destination, &QComboBox::currentIndexChanged, this, [this] { if (changed) changed(); });
    connect(descendants, &QCheckBox::toggled, this, [this] { if (changed) changed(); });
    connect(gutter, &QCheckBox::toggled, text, &WriteText::showGutter);
    text->scrolledUp = [this] { autoScroll->setChecked(false); };
}
void WriteView::clear() { text->clear(); }
void WriteView::append(const evemon_fd_write_payload_t &write)
{
    if (write.fd < 1 || write.fd > 2 || !(mask() & (1 << (write.fd - 1)))) return;
    proc_entry_t entry{}; char comm[256];
    const bool known = query_cached_process(write.tgid, &entry, comm, sizeof(comm));
    const QString name = QString::fromUtf8(comm);
    if (excludeKernel->isChecked() && known && (entry.ppid == 0 || name.startsWith('['))) return;
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
