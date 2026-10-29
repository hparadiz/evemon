#include <gio/gio.h>
#include "main_window.h"
#include "theme.h"
#include "about_dialog.h"
#include "compatibility_dialog.h"
#include "remote_dialog.h"
#include <QApplication>
#include <QActionGroup>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QHeaderView>
#include <QDialog>
#include <QLabel>
#include <QVBoxLayout>
#include <QPainter>
#include <algorithm>
#include <unistd.h>
extern "C" {
#include "../app/client_session.h"
#include "../app/runtime.h"
#include "../core/settings.h"
#ifdef HAVE_PIPEWIRE
void evemon_qt_audio_theme(unsigned);
#endif
}

class InterfaceDialog : public QDialog {
public:
    using QDialog::QDialog;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        QColor background = palette().color(QPalette::Window);
        if (background.alpha() < 255) background = QColor(48, 48, 48);
        painter.setBrush(background); painter.setPen(palette().color(QPalette::Mid));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 12, 12);
    }
};

static bool confirmInterfaceChange(QWidget *parent, const QString &label)
{
    InterfaceDialog dialog(parent, Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setObjectName("evemonSwitchInterface");
    dialog.setWindowTitle("Switch interface");
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    dialog.setMinimumWidth(334);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 8, 12, 12); layout->setSpacing(12);
    auto *title = new QLabel("Switch interface"); title->setAlignment(Qt::AlignCenter); layout->addWidget(title);
    auto *question = new QLabel(QString("Switch to %1?").arg(label));
    QFont heading = question->font(); heading.setBold(true); question->setFont(heading);
    question->setAlignment(Qt::AlignCenter); layout->addWidget(question);
    auto *explanation = new QLabel("Evemon will restart to apply this change.");
    explanation->setAlignment(Qt::AlignCenter); layout->addWidget(explanation);
    layout->addSpacing(24);
    auto *buttons = new QHBoxLayout; buttons->setSpacing(12); layout->addLayout(buttons);
    auto *cancel = new QPushButton("Cancel"); auto *restart = new QPushButton("Restart");
    buttons->addWidget(cancel); buttons->addWidget(restart); cancel->setDefault(true);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(restart, &QPushButton::clicked, &dialog, &QDialog::accept);
    return dialog.exec() == QDialog::Accepted;
}

void MainWindow::menus()
{
    auto *file = menuBar()->addMenu("&File");
    file->addAction("Connect to Remote…", this, [this] { showRemoteConnectionDialog(this); });
    if (query_remote_path())
        file->addAction("Disconnect", this, [this] { client_session_disconnect("qt"); restart(false); });
    file->addSeparator();
    file->addAction("Restart", this, [this] { restart(false); });
    file->addAction("Restart as Administrator", this, [this] { restart(true); })->setEnabled(geteuid() != 0 && !query_is_foreign_host());
    file->addSeparator();
    file->addAction("Exit", QKeySequence::Quit, this, &QWidget::close);
    auto *view = menuBar()->addMenu("&View");
    for (auto *dock : {detailDock, systemDock}) {
        if (dock == systemDock) view->addSeparator();
        view->addAction(dock->toggleViewAction());
        auto *positions = view->addMenu(dock == detailDock ? "Panel Position" : "System Panel Position");
        auto *group = new QActionGroup(positions);
        for (const auto &pair : {qMakePair(QString("Bottom"), Qt::BottomDockWidgetArea), qMakePair(QString("Top"), Qt::TopDockWidgetArea), qMakePair(QString("Left"), Qt::LeftDockWidgetArea), qMakePair(QString("Right"), Qt::RightDockWidgetArea)}) {
            auto *action = positions->addAction(pair.first); action->setCheckable(true); group->addAction(action);
            connect(positions, &QMenu::aboutToShow, action, [this, dock, pair, action] { action->setChecked(dockWidgetArea(dock) == pair.second); });
            connect(action, &QAction::triggered, this, [this, dock, pair] { dock->setFloating(false); addDockWidget(pair.second, dock); });
        }
    }
    auto *columns = view->addMenu("Columns");
    for (int c = 0; c < ProcessModel::ColumnCount; ++c) {
        auto *action = columns->addAction(model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString()); action->setCheckable(true);
        connect(columns, &QMenu::aboutToShow, action, [this, c, action] { action->setChecked(!tree->isColumnHidden(c)); });
        connect(action, &QAction::toggled, this, [this, c](bool checked) { tree->setColumnHidden(c, !checked); });
    }
    view->addSeparator(); view->addAction("Filter by Name", QKeySequence("Ctrl+F"), this, [this] { showHeaderEditor(search, ProcessModel::Name); });
    view->addAction("Go to PID", QKeySequence("Ctrl+G"), this, [this] { showHeaderEditor(pidEntry, ProcessModel::Pid); });
    view->addSeparator();
    for (const auto &name : {"Show Audio Processes Only", "Show GPU Processes Only", "Show CPU-Intensive Processes Only"}) {
        auto *action = view->addAction(name); action->setCheckable(true); filterActions.append(action);
        connect(action, &QAction::toggled, this, [this, name, action](bool enabled) {
            if (enabled) for (auto *other : filterActions) if (other != action) other->setChecked(false);
            if (QString(name).contains("Audio")) filter->setAudioOnly(enabled);
            else if (QString(name).contains("GPU")) filter->setGpuOnly(enabled);
            else filter->setCpuOnly(enabled);
        });
    }
    const QStringList filterKeys{"show_audio_only", "show_gpu_only", "show_cpu_intensive"};
    const auto *sharedView = settings_get();
    const bool filterDefaults[] = {sharedView->show_audio_only, sharedView->show_gpu_only, sharedView->show_cpu_intensive};
    for (int i = 0; i < filterActions.size(); ++i)
        filterActions[i]->setChecked(settings.value(processSettingKey(filterKeys[i]), filterDefaults[i]).toBool());
#ifndef HAVE_PIPEWIRE
    filterActions[0]->setChecked(false); filterActions[0]->setEnabled(false);
#endif
    bool hasGpu = false; for (auto *inspector : inspectors) hasGpu |= inspector->kind == Inspector::Gpu;
    filterActions[1]->setEnabled(hasGpu); filterActions[1]->setVisible(hasGpu);
    if (!hasGpu) { filterActions[1]->setChecked(false); for (int c : {ProcessModel::Gpu, ProcessModel::Vram, ProcessModel::Encoder}) tree->setColumnHidden(c, true); }
    view->addSeparator();
    auto *appearance = view->addMenu("Appearance");
    auto changeFont = [this](int delta) {
        QFont font = QApplication::font(); font.setPointSize(delta ? std::clamp(font.pointSize() + delta, 7, 24) : 9);
        QApplication::setFont(font); QFont mono = tree->font(); mono.setPointSize(font.pointSize()); tree->setFont(mono);
        QFont header = mono; header.setBold(true); tree->header()->setFont(header); settings.setValue("fontSize", font.pointSize());
    };
    appearance->addAction("Increase Font", QKeySequence("Ctrl++"), this, [changeFont] { changeFont(1); });
    appearance->addAction("Decrease Font", QKeySequence("Ctrl+-"), this, [changeFont] { changeFont(-1); });
    appearance->addAction("Reset Font", QKeySequence("Ctrl+0"), this, [changeFont] { changeFont(0); });
    appearance->addSeparator();
    auto *theme = appearance->addMenu("Theme");
    connect(theme, &QMenu::aboutToShow, this, [this, theme] {
        themeManager()->populate(theme, [this](const QString &id) { settings.setValue("theme", id); });
    });
    auto *charting = appearance->addMenu("Charting Theme"); auto *chartGroup = new QActionGroup(charting);
    int themeIndex = 0;
    auto *sharedSettings = static_cast<evemon_settings_t *>(evemon_settings_get());
    sharedSettings->spectro_theme = settings.value("spectro_theme", sharedSettings->spectro_theme).toInt();
    for (const auto &name : {"Classic", "Heat", "Cool", "Greyscale", "Neon", "Vaporwave"}) {
        const int index = themeIndex++; auto *action = charting->addAction(name); action->setCheckable(true); chartGroup->addAction(action); action->setChecked(sharedSettings->spectro_theme == index);
        connect(charting, &QMenu::aboutToShow, action, [action, index, sharedSettings] { action->setChecked(sharedSettings->spectro_theme == index); });
        connect(action, &QAction::triggered, this, [this, index, sharedSettings] { sharedSettings->spectro_theme = index; settings.setValue("spectro_theme", index);
#ifdef HAVE_PIPEWIRE
            evemon_qt_audio_theme(index);
#endif
        });
    }
    auto *plugins = menuBar()->addMenu("&Plugins");
    connect(plugins, &QMenu::aboutToShow, this, [this, plugins] {
        plugins->clear();
        struct Entry { QString title, id; Inspector *inspector; bool *service; };
        QList<Entry> entries;
        for (auto *inspector : inspectors) if (!inspector->pinned)
            entries.append({inspector->menuTitle(), QString::fromLatin1(inspector->descriptor.id), inspector, nullptr});
#ifdef HAVE_PIPEWIRE
        entries.append({"Media Meta", "org.evemon.audio_service", nullptr, &mediaEnabled});
#endif
        entries.append({"Process Metadata", "org.evemon.proc_meta", nullptr, &metadataEnabled});
        entries.append({"Write Monitor", "org.evemon.write_monitor", nullptr, &writesEnabled});
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
        });
        for (const auto &entry : entries) {
            auto *action = plugins->addAction(entry.title);
            action->setCheckable(true);
            action->setChecked(entry.inspector ? entry.inspector->pluginEnabled : *entry.service);
            connect(action, &QAction::toggled, this, [this, entry](bool enabled) {
                ++brokerGeneration;
                if (auto *inspector = entry.inspector) {
                    inspector->pluginEnabled = enabled;
                    auto *notebook = inspector->kind >= Inspector::SystemFiles ? systemTabs : tabs;
                    notebook->setTabVisible(notebook->indexOf(inspector), enabled);
                } else {
                    *entry.service = enabled;
                    if (entry.service == &mediaEnabled)
                        for (auto *inspector : inspectors) inspector->setMediaEnabled(enabled);
                    if (entry.service == &metadataEnabled) {
                        softwareName.clear(); softwareFields.clear(); details->setSoftware({}); refreshDetails();
                    }
                }
                settings.setValue("plugins/" + entry.id, enabled);
                settings_plugin_set_enabled(entry.id.toUtf8().constData(), enabled);
                syncWrites();
            });
        }
    });
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("Compatibility…", this, [this] { showCompatibilityDialog(this); });
    help->addSeparator();
    help->addAction("About", this, [this] { showAboutDialog(this); });
    if (evemon_ui_available("qt") && evemon_ui_available("gtk")) {
        appearance->addSeparator();
        auto *interfaces = appearance->addMenu("Interface");
        auto *choices = new QActionGroup(interfaces);
        for (const char *name : {"gtk", "qt"}) {
            const QString label = !strcmp(name, "qt") ? QStringLiteral("Qt") : QStringLiteral("GTK");
            auto *action = interfaces->addAction(label);
            action->setCheckable(true); choices->addAction(action);
            action->setData(QString::fromLatin1(name));
            action->setChecked(!strcmp(name, "qt"));
            connect(action, &QAction::triggered, this, [this, name, label, choices] {
                // The checkmark describes the running interface until restart.
                for (auto *choice : choices->actions()) choice->setChecked(choice->data().toString() == QLatin1String("qt"));
                if (!strcmp(name, "qt")) return;
                if (!confirmInterfaceChange(this, label)) return;
                if (evemon_set_preferred_ui(name)) {
                    QMessageBox::warning(this, "Interface", "Could not save the interface preference.");
                    return;
                }
                restart(false);
            });
        }
    }
}
