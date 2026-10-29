#include "detail_panel.h"
#include <QDateTime>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QPushButton>
#include <QPainter>
#include <QSplitter>
extern "C" void format_fuzzy_time(time_t epoch, char *buf, size_t size);

DetailValue::DetailValue(QWidget *parent) : QLabel(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setMinimumWidth(0); setTextFormat(Qt::PlainText); setTextInteractionFlags(Qt::TextSelectableByMouse);
}
void DetailValue::setValue(const QString &value) { full = value.isEmpty() ? QStringLiteral("–") : value; setToolTip(full); elide(); }
void DetailValue::resizeEvent(QResizeEvent *event) { QLabel::resizeEvent(event); elide(); }
void DetailValue::elide() { const auto value = fontMetrics().elidedText(full, Qt::ElideRight, width()); if (text() != value) setText(value); }
QWidget *DetailPanel::makeSection(const QString &title, const QStringList &keys, QMap<QString, DetailValue *> &labels)
{
    auto *section = new QWidget; auto *layout = new QVBoxLayout(section); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(4);
    if (!title.isEmpty()) {
        auto *line = new QFrame; line->setFrameShape(QFrame::HLine); layout->addWidget(line);
        auto *heading = new QLabel(title); QFont bold = heading->font(); bold.setBold(true); heading->setFont(bold); layout->addWidget(heading);
    }
    auto *grid = new QGridLayout; grid->setHorizontalSpacing(8); grid->setVerticalSpacing(4); grid->setColumnStretch(1, 1); layout->addLayout(grid);
    int row = 0;
    for (const auto &key : keys) {
        auto *label = new QLabel(key); QFont font = label->font(); font.setBold(true); label->setFont(font);
        auto *value = new DetailValue; value->setValue({}); labels.insert(key, value);
        grid->addWidget(label, row, 0); grid->addWidget(value, row++, 1);
    }
    return section;
}
class VerticalSummary : public QWidget {
public:
    using QWidget::QWidget;
    QString text = QStringLiteral("–");
    QSize sizeHint() const override { return {24, 60}; }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.translate(0, height()); painter.rotate(-90);
        painter.drawText(QRect(2, 0, height() - 4, width()), Qt::AlignCenter,
            fontMetrics().elidedText(text, Qt::ElideRight, height() - 4));
    }
};
class ProcessIcon : public QLabel {
public:
    explicit ProcessIcon(QWidget *viewport) : QLabel(viewport)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAlignment(Qt::AlignTop | Qt::AlignRight);
        viewport->installEventFilter(this);
        hide();
    }
    void setIcon(const QIcon &value, const QString &name)
    {
        setAccessibleName(name.isEmpty() ? QString() : name + " process icon");
        if (icon.cacheKey() == value.cacheKey()) return;
        icon = value; refresh();
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize) refresh();
        return QLabel::eventFilter(watched, event);
    }
private:
    QIcon icon;
    void refresh()
    {
        const auto *viewport = parentWidget();
        const int target = qMin(96, qMin(viewport->height() / 2, viewport->width() - 16));
        if (icon.isNull() || target < 16) { clear(); hide(); return; }
        setPixmap(icon.pixmap(QSize(target, target), devicePixelRatioF()));
        setGeometry(viewport->width() - target - 8, 8, target, target);
        show(); raise();
    }
};
DetailPanel::DetailPanel(QWidget *parent) : QWidget(parent)
{
    auto *tray = new QHBoxLayout(this); tray->setContentsMargins(0, 0, 0, 0); tray->setSpacing(0);
    toggle = new QPushButton(QStringLiteral("◀"), this); toggle->setFixedWidth(16);
    toggle->setObjectName("processInfoToggle");
    toggle->setStyleSheet("QPushButton { border: none; border-radius: 0; padding: 0; background: transparent; }");
    toggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding); toggle->setFocusPolicy(Qt::NoFocus);
    toggle->setToolTip("Hide process info"); tray->addWidget(toggle);
    summary = new VerticalSummary(this); summary->hide(); tray->addWidget(summary);
    body = new QScrollArea(this); body->setWidgetResizable(true);
    body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    body->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); body->setMinimumWidth(280); tray->addWidget(body, 1);
    connect(toggle, &QPushButton::clicked, this, [this] { setExpanded(!isExpanded()); });
    auto *content = new QWidget; auto *layout = new QVBoxLayout(content); layout->setContentsMargins(8, 8, 8, 8); layout->setSpacing(4);
    layout->addWidget(makeSection({}, {"PID", "PPID", "User", "Name", "CPU%", "Memory (RSS)", "Group Memory", "Group CPU%", "Disk Read", "Disk Write", "Net Send", "Net Recv", "Start Time", "Container", "Service", "CWD", "Command"}, processValues));
    steamSection = makeSection("Steam / Proton", {"Game", "App ID", "Proton", "Runtime", "Compat Data", "Game Directory"}, steamValues); layout->addWidget(steamSection); steamSection->hide();
    softwareSection = makeSection("Software", {"Organisation", "Homepage", "Source", "Funding", "Licence", "Summary"}, softwareValues); layout->addWidget(softwareSection); softwareSection->hide();
    layout->addStretch(); body->setWidget(content);
    processIcon = new ProcessIcon(body->viewport());
}
bool DetailPanel::isExpanded() const { return !body->isHidden(); }
void DetailPanel::setExpanded(bool open)
{
    body->setVisible(open); summary->setVisible(!open);
    toggle->setText(open ? QStringLiteral("◀") : QStringLiteral("▶"));
    toggle->setToolTip(open ? "Hide process info" : "Show process info");
    setMaximumWidth(open ? QWIDGETSIZE_MAX : 40);
    if (auto *splitter = qobject_cast<QSplitter *>(parentWidget()); splitter && splitter->isVisible()) {
        const int total = splitter->width(); splitter->setSizes({open ? 296 : 40, qMax(0, total - (open ? 296 : 40))});
    }
}
void DetailPanel::setProcess(const ProcessRow *r, quint64 send, quint64 receive, const QIcon &icon)
{
    processIcon->setIcon(r ? icon : QIcon(), r ? r->name : QString());
    if (!r) { for (auto *value : processValues) value->setValue({}); steamSection->hide(); softwareSection->hide(); return; }
    const auto &e = r->entry;
    char age[64]; format_fuzzy_time(e.start_time, age, sizeof(age));
    const QString start = e.start_time > 0
        ? QDateTime::fromSecsSinceEpoch(e.start_time).toString("yyyy-MM-dd HH:mm:ss") + QString(" (%1)").arg(QString::fromUtf8(age))
        : QStringLiteral("–");
    summary->text = QString("%1  %2  %3%  %4").arg(e.pid).arg(r->name).arg(e.cpu_percent, 0, 'f', 1).arg(formatBytes(e.mem_rss_kb * 1024.0)); summary->update();
    const QMap<QString, QString> values{{"PID", QString::number(e.pid)}, {"PPID", QString::number(e.ppid)}, {"User", r->user}, {"Name", r->name},
        {"CPU%", QString::number(e.cpu_percent, 'f', 1) + '%'}, {"Memory (RSS)", formatBytes(e.mem_rss_kb * 1024.0)}, {"Group Memory", formatBytes(r->groupMemory * 1024.0)},
        {"Group CPU%", QString::number(r->groupCpu, 'f', 1) + '%'}, {"Disk Read", formatBytes(e.io_read_rate) + "/s"}, {"Disk Write", formatBytes(e.io_write_rate) + "/s"},
        {"Net Send", formatBytes(send) + "/s"}, {"Net Recv", formatBytes(receive) + "/s"},
        {"Start Time", start}, {"Container", r->container}, {"Service", r->service}, {"CWD", r->cwd}, {"Command", r->command}};
    for (auto it = values.cbegin(); it != values.cend(); ++it) processValues[it.key()]->setValue(it.value());
    steamSection->setVisible(!r->steam.isEmpty());
    const auto lines = r->steam.split('\n');
    const QStringList names{"Game", "App ID", "Proton", "Runtime", "Compat Data", "Game Directory"};
    for (int i = 0; i < names.size(); ++i) { const auto line = lines.value(i); steamValues[names[i]]->setValue(i ? line.mid(line.indexOf(':') + 1).trimmed() : line); }
}
void DetailPanel::setSoftware(const QMap<QString, QString> &values)
{
    softwareSection->setVisible(!values.isEmpty());
    for (auto it = softwareValues.cbegin(); it != softwareValues.cend(); ++it) it.value()->setValue(values.value(it.key()));
}
