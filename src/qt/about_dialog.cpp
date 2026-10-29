extern "C" {
#include "../ipc/query.h"
#include "../ipc/query_about.h"
}
#include "about_dialog.h"
#include "theme.h"
#include "process_model.h"
#include "../ui/about_distro.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSysInfo>
#include <QTimer>
#include <cmath>
#include <algorithm>
extern "C" {
#include "../platform/desktop_session.h"
void format_memory(long kb, char *buf, size_t size);
}
#include <pwd.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {
QString osField(const QString &name)
{
    for (QString line : readText("/etc/os-release").split('\n')) {
        if (!line.startsWith(name + '=')) continue;
        line = line.mid(name.size() + 1);
        if (line.size() > 1 && (line.front() == '"' || line.front() == '\'')) line = line.mid(1, line.size() - 2);
        return line;
    }
    return {};
}

class DistroGlyph : public QWidget {
public:
    DistroGlyph(evemon_distro_logo_t logo, QWidget *parent) : QWidget(parent), color(logo.color)
    {
        static const int fontId = QFontDatabase::addApplicationFont(":/evemon/font-logos.ttf");
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        glyphFont = QFont(families.value(0, "font-logos"), 150);
        const char32_t codepoint = logo.codepoint;
        glyph = QString::fromUcs4(&codepoint, 1);
        QFontMetricsF metrics(glyphFont);
        setFixedSize(qCeil(metrics.horizontalAdvance(glyph)), qCeil(metrics.height()));
        clock.start();
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { update(); }); timer->start(33);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal scale = devicePixelRatioF();
        if (mask.size() != size() * scale) {
            mask = QImage(size() * scale, QImage::Format_ARGB32_Premultiplied);
            mask.setDevicePixelRatio(scale); mask.fill(Qt::transparent);
            QPainter painter(&mask); painter.setFont(glyphFont); painter.setPen(Qt::white);
            painter.drawText(QPointF(0, QFontMetricsF(glyphFont).ascent()), glyph);
        }
        QImage frame = mask.copy();
        const double phase = std::fmod(clock.elapsed() / 4000.0, 1.0);
        const int width = frame.width(), height = frame.height();
        for (int y = 0; y < height; ++y) {
            auto *pixels = reinterpret_cast<QRgb *>(frame.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const int alpha = qAlpha(pixels[x]); if (!alpha) continue;
                double d = double(x + y) / (width + height) - phase;
                if (d > .5) d -= 1;
                if (d < -.5) d += 1;
                const double mix = .7 * std::exp(-d * d / .04);
                auto channel = [mix, alpha](int base) { return int((base + mix * (255 - base)) * alpha / 255); };
                pixels[x] = qRgba(channel(color.red()), channel(color.green()), channel(color.blue()), alpha);
            }
        }
        QPainter painter(this); painter.drawImage(QPoint(0, 0), frame);
    }
private:
    QColor color;
    QString glyph;
    QFont glyphFont;
    QImage mask;
    QElapsedTimer clock;
};

class AboutDialog : public QDialog {
public:
    explicit AboutDialog(QWidget *parent, const query_about_t *machine = nullptr) : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    {
        const query_about_t *remote = query_is_foreign_host() ? machine : nullptr;
        setObjectName("evemonAbout"); setWindowTitle(remote ? "About evemon | " + QString::fromUtf8(remote->hostname) : "About evemon"); setWindowRole("about");
        setAttribute(Qt::WA_DeleteOnClose); setAttribute(Qt::WA_TranslucentBackground);
        setModal(false);
        const auto logo = evemon_distro_logo(remote ? remote->distro_id : osField("ID").toUtf8().constData()); brand = QColor(logo.color);
        auto *layout = new QHBoxLayout(this); layout->setContentsMargins(37, 29, 37, 29); layout->setSpacing(24);
        layout->setSizeConstraint(QLayout::SetFixedSize);
        auto *glyph = new DistroGlyph(logo, this); layout->addWidget(glyph, 0, Qt::AlignVCenter);
        glyph->setAccessibleName(remote ? QString::fromUtf8(remote->distro_name) : osField("PRETTY_NAME"));
        label = new QLabel(this); QFont infoFont("Monospace"); infoFont.setPointSizeF(10000.0 / 1024);
        label->setFont(infoFont); label->setWordWrap(true);
        label->setTextFormat(Qt::RichText); label->setTextInteractionFlags(Qt::NoTextInteraction);
        label->setStyleSheet("color: #e6e6e6; background: transparent;");
        const int maximumWidth = (parent->screen() ? parent->screen()->availableGeometry().width() : 1200) - glyph->width() - 110;
        label->setMaximumWidth(qMax(240, maximumWidth));
        layout->addWidget(label, 1, Qt::AlignVCenter);
        if (remote) {
            header = QString::fromUtf8(remote->user) + '@' + QString::fromUtf8(remote->hostname);
            QString uptime;
            const quint64 days = remote->uptime / 86400, hours = remote->uptime / 3600 % 24;
            if (days) uptime += QString::number(days) + "d ";
            if (days || hours) uptime += QString::number(hours) + "h ";
            uptime += QString::number(remote->uptime / 60 % 60) + 'm';
            char used[64], total[64];
            format_memory((remote->memory_total - remote->memory_available) / 1024, used, sizeof(used));
            format_memory(remote->memory_total / 1024, total, sizeof(total));
            rows = {{"OS:", QString::fromUtf8(remote->distro_name)}, {"Kernel:", QString::fromUtf8(remote->kernel)},
                {"Uptime:", uptime}, {"Shell:", QString::fromUtf8(remote->shell)}, {"DE:", "Unavailable"},
                {"Resolution:", "Unavailable"}, {"HDR:", "Unavailable"}, {"GPU:", QString::fromUtf8(remote->gpu)},
                {"CPU:", QString::fromUtf8(remote->cpu) + QString(" (%1)").arg(remote->cpus)},
                {"Memory:", QString::fromLatin1(used) + " / " + QString::fromLatin1(total)}};
            renderText();
        } else {
        const auto *user = getpwuid(desktop_session_uid());
        header = (user ? QString::fromLocal8Bit(user->pw_name) : qEnvironmentVariable("USER")) + '@' + QSysInfo::machineHostName();
        struct utsname kernel{}; uname(&kernel);
        struct sysinfo info{}; sysinfo(&info);
        const long days = info.uptime / 86400, hours = info.uptime / 3600 % 24, mins = info.uptime / 60 % 60;
        QString uptime;
        if (days) uptime += QString::number(days) + "d ";
        if (days || hours) uptime += QString::number(hours) + "h ";
        uptime += QString::number(mins) + 'm';
        char cpuName[256]; query_about_cpu_name(cpuName, sizeof(cpuName));
        const QString cpu = QString::fromUtf8(machine ? machine->cpu : cpuName);
        QString gpu = machine ? QString::fromUtf8(machine->gpu) : parent->property("gpuNames").toString();
        if (gpu.isEmpty()) gpu = "unknown";
        const QString mem = readText("/proc/meminfo");
        auto field = [&mem](const QString &name) { return QRegularExpression(name + ":\\s+(\\d+)").match(mem).captured(1).toULongLong() * 1024; };
        const quint64 total = field("MemTotal"), available = field("MemAvailable");
        QStringList displays;
        for (auto *screen : QGuiApplication::screens()) {
            const QSize pixels = screen->size() * screen->devicePixelRatio();
            QString text = QString("%1x%2").arg(pixels.width()).arg(pixels.height());
            if (screen->refreshRate() > 0) text += QString(" @ %1 Hz").arg(qRound(screen->refreshRate()));
            displays.append(text);
        }
        QString display = displays.join(", ");
        if (!displays.isEmpty() && std::all_of(displays.cbegin(), displays.cend(), [&](const QString &s) { return s == displays.first(); }))
            display = displays.first() + (displays.size() > 1 ? QString(" (%1 displays)").arg(displays.size()) : QString());
        QString shellPath = qEnvironmentVariable("SHELL");
        if (shellPath.isEmpty() && user) shellPath = QString::fromLocal8Bit(user->pw_shell);
        const QString shell = QFileInfo(shellPath).fileName();
        QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP", qEnvironmentVariable("DESKTOP_SESSION", "unknown"));
        char usedMemory[64], totalMemory[64];
        format_memory((total - available) / 1024, usedMemory, sizeof(usedMemory));
        format_memory(total / 1024, totalMemory, sizeof(totalMemory));
        rows = {{"OS:", osField("PRETTY_NAME")}, {"Kernel:", QString::fromLocal8Bit(kernel.release)}, {"Uptime:", uptime},
            {"Shell:", shell}, {"DE:", desktop}, {"Resolution:", display}, {"HDR:", "No"}, {"GPU:", gpu},
            {"CPU:", cpu + QString(" (%1)").arg(sysconf(_SC_NPROCESSORS_ONLN))},
            {"Memory:", QString::fromLatin1(usedMemory) + " / " + QString::fromLatin1(totalMemory)}};
        for (const QString &connector : QDir("/sys/class/drm").entryList({"card*-*"}, QDir::Dirs | QDir::NoDotAndDotDot))
            if (QFileInfo::exists("/sys/class/drm/" + connector + "/hdr_output_metadata")) rows[6].second = "Supported";
        renderText();
        // Gather PCI names without blocking the GUI or executing a shell command.
        auto *pci = new QProcess(this);
        watchProcess(pci);
        connect(pci, &QProcess::finished, this, [this, pci](int, QProcess::ExitStatus) {
            QStringList gpus;
            for (const QString &line : QString::fromLocal8Bit(pci->readAllStandardOutput()).split('\n'))
                if (line.contains("VGA compatible controller:") || line.contains("3D controller:") || line.contains("Display controller:"))
                    {
                    QString name = line.section(": ", 1).remove(QRegularExpression(" \\(rev [^)]+\\)$"));
                    const auto vendor = QRegularExpression("^.+?\\[([^] /]+)(?:/[^]]+)?\\] (.+)$").match(name);
                    if (vendor.hasMatch()) name = vendor.captured(1) + ' ' + vendor.captured(2);
                    const QString slot = line.section(' ', 0, 0);
                    const quint64 memory = readText("/sys/bus/pci/devices/" + slot + "/mem_info_vram_total").toULongLong();
                    if (memory) name += QString(" (%1 GiB)").arg(qRound64(double(memory) / (1024 * 1024 * 1024)));
                    gpus.append(name);
                }
            if (!gpus.isEmpty()) rows[7].second = gpus.join(", ");
            renderText(); pci->deleteLater();
        });
        pci->setChildProcessModifier([] { desktop_child_use_user(nullptr); });
        pci->start("/usr/bin/lspci", {"-D"});
        QTimer::singleShot(1500, pci, [pci] { if (pci->state() != QProcess::NotRunning) pci->kill(); });
        // Match GTK's prompt header, with a deadline and no GUI-thread wait.
        if (shell == "zsh") {
            auto *prompt = new QProcess(this);
            watchProcess(prompt);
            prompt->setChildProcessModifier([] { desktop_child_use_user(nullptr); });
            connect(prompt, &QProcess::finished, this, [this, prompt](int code, QProcess::ExitStatus status) {
                QString text = QString::fromUtf8(prompt->readAllStandardOutput());
                text.remove(QRegularExpression("\\x1b\\[[0-?]*[ -/]*[@-~]"));
                text.remove(QRegularExpression("[\\x{e0a0}-\\x{e0d4}\\x00-\\x1f]"));
                text = text.simplified().left(160);
                const auto identity = QRegularExpression("(?:^|\\s)(\\S+@\\S+)(?:\\s|$)").match(text);
                if (identity.hasMatch()) text = identity.captured(1);
                if (!code && status == QProcess::NormalExit && !text.isEmpty()) { header = text; renderText(); }
                prompt->deleteLater();
            });
            prompt->start(shellPath, {"-ic", "print -Pn \"$PROMPT\""});
            QTimer::singleShot(1000, prompt, [prompt] { if (prompt->state() != QProcess::NotRunning) prompt->kill(); });
        }
        }
        installEventFilter(this); label->installEventFilter(this); glyph->installEventFilter(this);
        adjustSize(); center();
    }
    void present() {
        presentationRequested = true;
        if (pendingProcesses) return;
        adjustSize(); center(); presentAuxiliaryWindow(this, false);
    }
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress) { close(); return true; }
        return false;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(brand, 1)); painter.setBrush(QColor(18, 18, 22));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 12, 12);
    }
private:
    QColor brand;
    QLabel *label;
    QString header;
    QList<QPair<QString, QString>> rows;
    int pendingProcesses = 0;
    bool presentationRequested = false;
    void watchProcess(QProcess *process) {
        ++pendingProcesses;
        connect(process, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
            if (state != QProcess::NotRunning) return;
            // Let finished/error handlers update the text before mapping the
            // dialog. Wayland cannot recenter a mapped window after it grows.
            QTimer::singleShot(0, this, [this] {
                --pendingProcesses;
                if (presentationRequested && !pendingProcesses) present();
            });
        });
    }
    void center() {
        if (!isWindow()) return;
        const QRect parentRect = parentWidget()->frameGeometry();
        move(parentRect.center() - rect().center());
    }
    void renderText() {
        const QString color = brand.name();
        QString text = QString("<span style='font-family:monospace;font-size:11.71875pt;color:%1;font-weight:bold'>%2</span><br>")
            .arg(color, header.toHtmlEscaped());
        text += QString(header.size(), '-').toHtmlEscaped() + "<br>";
        for (const auto &[key, value] : rows)
            text += QString("<span style='font-family:monospace;font-size:9.765625pt'><b style='color:%1'>%2</b> %3</span><br>")
                .arg(color, key.toHtmlEscaped(), value.toHtmlEscaped());
        text += QString("<br><b>evemon</b> %1<br><span style='font-size:8.7890625pt;font-style:italic'>A lightweight Linux everything monitor</span>")
            .arg(QCoreApplication::applicationVersion().toHtmlEscaped());
        label->setText(text); adjustSize(); if (isVisible()) center();
    }
};
}

void showAboutDialog(QWidget *parent)
{
    for (auto *child : parent->findChildren<QDialog *>("evemonAbout")) {
        static_cast<AboutDialog *>(child)->present(); return;
    }
    if (!query_remote_path()) {
        auto *dialog = new AboutDialog(parent); dialog->present(); return;
    }
    if (parent->property("aboutLoading").toBool()) return;
    parent->setProperty("aboutLoading", true);
    auto *guard = new QPointer<QWidget>(parent);
    query_about_async(query_remote_path(), [](GObject *, GAsyncResult *result, gpointer data) {
        auto *guard = static_cast<QPointer<QWidget> *>(data);
        GError *error = nullptr;
        auto *info = query_about_finish(result, &error);
        if (*guard) {
            (*guard)->setProperty("aboutLoading", false);
            if (info) {
                auto *dialog = new AboutDialog(*guard, info); dialog->present();
            } else if (!query_is_foreign_host()) {
                auto *dialog = new AboutDialog(*guard); dialog->present();
            } else QMessageBox::warning(*guard, "About evemon", QString::fromUtf8(error->message));
        }
        g_free(info); g_clear_error(&error); delete guard;
    }, guard);
}
