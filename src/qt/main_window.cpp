#include <gio/gio.h>
#include "main_window.h"
#include "theme.h"
#include <QApplication>
#include <QPushButton>
#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <algorithm>
#include <utility>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QDebug>
#include <QJsonArray>
#include <QShortcut>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPointer>
#include <QScrollBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTabBar>
#include <QToolButton>
#include <QThread>
#include <QVBoxLayout>
#include <sqlite3.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utmpx.h>
#include <signal.h>
#include <cerrno>
extern "C" {
#include "../plugins/event_bus.h"
#include "../core/settings.h"
#include "../platform/desktop_session.h"
#include "../app/runtime.h"
#include "../core/profile.h"
#ifdef HAVE_PIPEWIRE
#include "ui/pipewire_graph.h"
void evemon_qt_audio_theme(unsigned);
#endif
int fdmon_write_enable(fdmon_ctx_t *ctx);
int fdmon_add_pid_fd(fdmon_ctx_t *ctx, pid_t pid, int fd);
int fdmon_remove_pid_fd(fdmon_ctx_t *ctx, pid_t pid, int fd);
int fdmon_watch_parent_fds(fdmon_ctx_t *ctx, pid_t pid, int fd_mask);
void fdmon_unwatch_parent_fds(fdmon_ctx_t *ctx, pid_t pid);
#ifdef HAVE_NVTOP
#include "plugins/gpu_nvtop_adapter.h"
#endif
}

namespace {
class PanelTitleBar : public QWidget {
public:
    using QWidget::QWidget;
    QSize sizeHint() const override { return {0, 0}; }
    QSize minimumSizeHint() const override { return {0, 0}; }
};
class PanelViewport : public QScrollArea {
public:
    PanelViewport(QWidget *parent, QSize preferred) : QScrollArea(parent), preferred(preferred) {}
    QSize sizeHint() const override { return preferred; }
    QSize minimumSizeHint() const override { return {0, 0}; }
private:
    QSize preferred;
};
}

MainWindow::MainWindow(monitor_state_t &monitor, int initialPid, bool safeMode)
    : state(monitor), settings("evemon-qt", "evemon-qt"), desiredPid(initialPid)
{
    setWindowTitle(query_is_foreign_host()
        ? QStringLiteral("ＥＶＥＭＯＮ | %1").arg(QString::fromUtf8(query_host_name()))
        : QStringLiteral("ＥＶＥＭＯＮ"));
    setWindowIcon(QIcon(":/evemon/icon.png")); resize(1100, 700);
    QFile original(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/evemon/settings.json");
    if (original.open(QIODevice::ReadOnly)) originalSettings = QJsonDocument::fromJson(original.readAll()).object();
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);
    themeManager()->prepareWindow(this);
    themeManager()->apply(settings.value("theme", originalSettings.value("theme").toString("System")).toString());
    QFont font = QApplication::font(); font.setPointSize(settings.value("fontSize", originalSettings.value("font_size").toInt(9)).toInt()); QApplication::setFont(font);
    auto *central = new QWidget(this); auto *layout = new QVBoxLayout(central); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(2);
    search = new QLineEdit(this); search->setPlaceholderText("filter…"); search->hide();
    pidEntry = new QLineEdit(this); pidEntry->setPlaceholderText("PID…"); pidEntry->hide();
    model = new ProcessModel(this); filter = new ProcessFilter(this); filter->setSourceModel(model);
    tree = new QTreeView(this); tree->setModel(filter); tree->setUniformRowHeights(true); tree->setAnimated(false);
    tree->setSortingEnabled(true); tree->setAllColumnsShowFocus(true); tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    QFont treeFont(QStringLiteral("Monospace")); treeFont.setPointSize(font.pointSize()); tree->setFont(treeFont);
    tree->setObjectName("processTree");
    tree->setStyleSheet("QTreeView::item { min-height: 20px; padding: 0px; }");
    QFont headerFont = treeFont; headerFont.setBold(true); tree->header()->setFont(headerFont);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers); tree->setIndentation(14); tree->setIconSize(QSize(16, 16));
    tree->header()->setSectionsMovable(true); tree->header()->setStretchLastSection(false);
    tree->setItemDelegateForColumn(ProcessModel::History, new SparklineDelegate(tree));
    const int widths[] = {190, 70, 80, 310, 70, 60, 80, 55, 115, 155, 100, 100, 100, 86, 140, 90, 120, 150, 300};
    for (int c = 0; c < ProcessModel::ColumnCount; ++c) tree->setColumnWidth(c, widths[c]);
    const auto originalColumns = originalSettings.value("columns").toArray();
    for (int c = 0; c < ProcessModel::ColumnCount; ++c) {
        const bool visible = originalColumns.isEmpty() || originalColumns.contains(model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
        tree->setColumnHidden(c, !visible);
    }
    tree->sortByColumn(ProcessModel::Pid, Qt::DescendingOrder);
    layout->addWidget(tree, 1); setCentralWidget(central);
    detailDock = new QDockWidget(QString(), this); detailDock->setObjectName("detailPanel"); detailDock->setTitleBarWidget(new PanelTitleBar(detailDock)); detailDock->toggleViewAction()->setText("Detail Panel");
    detailSplit = new QSplitter(Qt::Horizontal, detailDock);
    details = new DetailPanel(detailSplit);
    tabs = new QTabWidget(detailSplit); tabs->setDocumentMode(true); tabs->setMovable(true);
    detailSplit->addWidget(details); detailSplit->addWidget(tabs);
    detailSplit->setStretchFactor(0, 0); detailSplit->setStretchFactor(1, 1); detailDock->setWidget(detailSplit);
    const Qt::DockWidgetArea areas[] = {Qt::BottomDockWidgetArea, Qt::TopDockWidgetArea, Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea};
    addDockWidget(areas[std::clamp(originalSettings.value("detail_panel_position").toInt(1), 0, 3)], detailDock);
    detailDock->setVisible(originalSettings.value("detail_panel_open").toBool(false));
    systemDock = new QDockWidget(QString(), this); systemDock->setObjectName("systemPanel"); systemDock->setTitleBarWidget(new PanelTitleBar(systemDock)); systemDock->toggleViewAction()->setText("System Plugin Panel");
    systemTabs = new QTabWidget(systemDock); systemTabs->setDocumentMode(true); systemTabs->setMovable(true);
    auto *systemClose = new QToolButton(systemTabs);
    systemClose->setText(QStringLiteral("×"));
    systemClose->setProperty("evemonPanelClose", true);
    systemClose->setAutoRaise(true); systemClose->setFocusPolicy(Qt::NoFocus);
    systemClose->setToolTip("Hide System Panel"); systemTabs->setCornerWidget(systemClose, Qt::TopRightCorner);
    connect(systemClose, &QToolButton::clicked, systemDock, &QDockWidget::hide);
    systemDock->setWidget(systemTabs); addDockWidget(areas[std::clamp(originalSettings.value("system_panel_position").toInt(0), 0, 3)], systemDock); systemDock->setVisible(originalSettings.value("system_panel_open").toBool(false));
    for (auto *dock : {detailDock, systemDock}) {
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        auto *content = dock->widget();
        dock->setWidget(nullptr);
        auto *viewport = new PanelViewport(dock, dock == detailDock ? QSize(350, 250) : QSize(320, 200));
        viewport->setFrameShape(QFrame::NoFrame);
        viewport->setWidgetResizable(true);
        viewport->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        viewport->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        viewport->setWidget(content);
        dock->setWidget(viewport);
    }
    auto serviceEnabled = [this](const char *id) {
        return settings.value("plugins/" + QString::fromLatin1(id), settings_plugin_enabled(id)).toBool();
    };
    mediaEnabled = serviceEnabled("org.evemon.audio_service");
    metadataEnabled = serviceEnabled("org.evemon.proc_meta");
    writesEnabled = serviceEnabled("org.evemon.write_monitor");
    if (!safeMode) {
#ifdef HAVE_PIPEWIRE
        if (!query_is_foreign_host() ||
            (query_remote_capabilities() & IPC_CAP_PIPEWIRE))
            addInspector(Inspector::Audio, "PipeWire", tabs);
#endif
        addInspector(Inspector::Network, "Network", tabs);
        addInspector(Inspector::Environment, "Environment Variables", tabs);
        addInspector(Inspector::Writes, "Write Log", tabs);
        addInspector(Inspector::Files, "File Descriptors", tabs);
        addInspector(Inspector::Threads, "Threads", tabs);
        addInspector(Inspector::Devices, "Devices", tabs);
        if (!query_is_foreign_host() && QDir("/var/db/pkg").exists()) addInspector(Inspector::Gentoo, "Gentoo / Portage", tabs);
        addInspector(Inspector::Maps, "Memory Maps", tabs);
        addInspector(Inspector::Cgroup, "cgroup Limits", tabs);
        addInspector(Inspector::Libraries, "Libraries", tabs);
#ifdef HAVE_MILKDROP
        if (!query_is_foreign_host()) addInspector(Inspector::MilkDrop, "MilkDrop", tabs);
#endif
#ifdef HAVE_NVTOP
        addInspector(Inspector::Gpu, "GPU", systemTabs);
#endif
        addInspector(Inspector::SystemLibraries, "Library", systemTabs);
        addInspector(Inspector::SystemFiles, "Files", systemTabs);
    }
    summary = new DetailValue(this); summary->setTextInteractionFlags(Qt::NoTextInteraction);
    load = new QLabel(this); statusBar()->addWidget(summary, 1); statusBar()->addPermanentWidget(load);
    menus();
    menuBar()->hide(); statusBar()->setSizeGripEnabled(false);
    qApp->installEventFilter(this); search->installEventFilter(this); pidEntry->installEventFilter(this);
    connect(tree, &QTreeView::doubleClicked, this, [this] { detailDock->show(); });
    statusBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(statusBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        QMenu menu(this); for (auto *action : filterActions) menu.addAction(action);
        populatePluginWindowMenu(menu.addMenu("Open Plugin as Window"));
        menu.addSeparator(); menu.addAction(menuBar()->isVisible() ? "Hide Menubar" : "Show Menubar", this, [this] { menuBar()->setVisible(!menuBar()->isVisible()); });
        menu.exec(statusBar()->mapToGlobal(point));
    });
    connect(search, &QLineEdit::textChanged, filter, &ProcessFilter::setQuery);
    connect(pidEntry, &QLineEdit::textEdited, this, [this](const QString &text) {
        bool ok; const int pid = text.toInt(&ok);
        if (ok && pid > 0 && model->process(pid)) selectPid(pid);
    });
    connect(pidEntry, &QLineEdit::returnPressed, this, [this] {
        bool ok; const int pid = pidEntry->text().toInt(&ok);
        if (ok && pid > 0) selectPid(pid);
        pidEntry->clear(); pidEntry->hide(); tree->setFocus();
    });
    connect(tree->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        const int pid = index.data(ProcessModel::PidRole).toInt();
        if (pid == selectedPid) return;
        selectedPid = pid; ++brokerGeneration;
        for (auto *inspector : inspectors) if (inspector->kind < Inspector::SystemFiles && !inspector->pinned) inspector->setPid(pid);
        refreshDetails(); syncWrites(); if (!brokerBusy) startBroker();
    });
    tree->setContextMenuPolicy(Qt::CustomContextMenu); connect(tree, &QTreeView::customContextMenuRequested, this, &MainWindow::contextMenu);
    connect(tree, &QTreeView::expanded, this, [this](const QModelIndex &i) { collapsed.remove(i.data(ProcessModel::PidRole).toInt()); });
    connect(tree, &QTreeView::collapsed, this, [this](const QModelIndex &i) { collapsed.insert(i.data(ProcessModel::PidRole).toInt()); });
    connect(model, &QAbstractItemModel::modelReset, this, &MainWindow::restoreTree);
    for (auto *notebook : {tabs, systemTabs}) {
        connect(notebook, &QTabWidget::currentChanged, this, [this] { ++brokerGeneration; syncWrites(); QTimer::singleShot(0, this, [this] { if (!brokerBusy) startBroker(); }); });
    }
    broker_set_complete_callback([](broker_cycle_t *cycle, void *ctx) {
        auto *window = static_cast<MainWindow *>(ctx);
        // The shared owner also frees cancelled queued deliveries during destruction.
        auto owned = std::shared_ptr<broker_cycle_t>(cycle, broker_cycle_free);
        QMetaObject::invokeMethod(window, [window, owned] {
            window->brokerBusy = false;
            if (!window->closing && window->gatherGeneration == window->brokerGeneration)
                broker_dispatch_cycle(owned.get());
            else if (!window->closing)
                QTimer::singleShot(0, window, [window] { window->startBroker(); });
        }, Qt::QueuedConnection);
    }, this);
    broker_set_audio_callback([](const pid_t *pids, size_t count, void *ctx) {
        QSet<int> active; for (size_t i = 0; i < count; ++i) active.insert(pids[i]);
        static_cast<MainWindow *>(ctx)->filter->setAudioPids(active);
    }, this);
#ifdef HAVE_NVTOP
    gpuSubscription = evemon_event_bus_subscribe(EVEMON_EVENT_GPU_SNAPSHOT, [](const evemon_event_t *event, void *ctx) {
        auto *window = static_cast<MainWindow *>(ctx); auto *snapshot = static_cast<const evemon_gpu_snapshot_t *>(event->payload);
        QHash<int, QVector<quint64>> values;
        for (uint i = 0; i < snapshot->process_count; ++i) {
            const auto &p = snapshot->processes[i]; auto &v = values[p.pid]; if (v.isEmpty()) v = {0, 0, 0};
            if (p.valid & EVEMON_GPU_PROCESS_VALID_UTILIZATION) v[0] += p.utilization_percent;
            if (p.valid & EVEMON_GPU_PROCESS_VALID_MEMORY) v[1] += p.memory_bytes;
            if (p.valid & EVEMON_GPU_PROCESS_VALID_ENCODE) v[2] += p.encode_percent;
        }
        window->model->setGpu(values);
        quint64 utilization = 0, used = 0, total = 0, encoder = 0;
        uint utilizationCount = 0, memoryCount = 0, encoderCount = 0;
        QStringList gpuNames;
        const auto saturatingAdd = [](quint64 a, quint64 b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; };
        for (uint i = 0; i < snapshot->device_count; ++i) {
            const auto &device = snapshot->devices[i];
            if (device.name[0]) gpuNames.append(QString::fromUtf8(device.name));
            if (device.valid & EVEMON_GPU_VALID_UTILIZATION) { utilization += device.utilization_percent; ++utilizationCount; }
            if (device.valid & EVEMON_GPU_VALID_MEMORY) {
                used = saturatingAdd(used, device.memory_used_bytes);
                total = saturatingAdd(total, device.memory_total_bytes); ++memoryCount;
            }
            if (device.valid & EVEMON_GPU_VALID_ENCODER) { encoder += device.encoder_percent; ++encoderCount; }
        }
        window->setProperty("gpuNames", gpuNames.join(", "));
        if (!encoderCount) {
            for (uint i = 0; i < snapshot->process_count; ++i)
                if (snapshot->processes[i].valid & EVEMON_GPU_PROCESS_VALID_ENCODE) {
                    encoder += snapshot->processes[i].encode_percent; encoderCount = snapshot->device_count;
                }
        }
        const QString unavailable = QStringLiteral("—");
        window->gpuSummary = snapshot->device_count ? QString("  |  GPU: %1  |  VRAM: %2  |  ENC: %3")
            .arg(utilizationCount ? QString::number(utilization / utilizationCount) + '%' : unavailable,
                 memoryCount ? formatBytes(used) + " / " + formatBytes(total) : unavailable,
                 encoderCount ? QString::number(std::min(quint64(100), encoder / encoderCount)) + '%' : unavailable) : QString();
    }, this);
#endif
    writeSubscription = evemon_event_bus_subscribe(EVEMON_EVENT_FD_WRITE, [](const evemon_event_t *event, void *ctx) {
        auto *window = static_cast<MainWindow *>(ctx); const auto &w = *static_cast<const evemon_fd_write_payload_t *>(event->payload);
        if (!window->writePids.contains(w.tgid)) return;
        for (auto *inspector : window->inspectors) if (inspector->kind == Inspector::Writes && inspector->wantsData() && window->writerTracks(inspector, w.tgid)) inspector->appendWrite(w);
    }, this);
    execSubscription = evemon_event_bus_subscribe(EVEMON_EVENT_CHILD_EXEC, [](const evemon_event_t *event, void *ctx) {
        auto *window = static_cast<MainWindow *>(ctx); const auto &e = *static_cast<const evemon_exec_payload_t *>(event->payload);
        if (window->writeParents.isEmpty()) return;
        window->execParents[e.pid] = e.ppid;
        for (auto *writer : window->inspectors) if (writer->kind == Inspector::Writes && writer->wantsData() && writer->writeMask() && window->writerTracks(writer, e.pid)) {
            window->writePids.insert(e.pid); window->writeFds[e.pid] |= writer->writeMask();
        }
    }, this);
    if (settings.contains("geometry")) restoreGeometry(settings.value("geometry").toByteArray());
    if (settings.contains("windowState")) restoreState(settings.value("windowState").toByteArray());
    if (settings.contains("columns")) tree->header()->restoreState(settings.value("columns").toByteArray());
    if (settings.contains("detailSplit")) detailSplit->restoreState(settings.value("detailSplit").toByteArray());
    details->setExpanded(settings.value("procInfoOpen", originalSettings.value("proc_info_open").toBool(true)).toBool());
    detailSplit->setCollapsible(0, false);
    // Restore usable sizes once, including panels first opened later. After
    // this the user can shrink either panel without a content minimum.
    auto initializePanels = [this] {
        QList<QDockWidget *> sidePanels, horizontalPanels;
        QList<int> widths, heights;
        for (auto *dock : {detailDock, systemDock}) {
            if (dock->isHidden() || dock->isFloating()) continue;
            const auto area = dockWidgetArea(dock);
            const bool side = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
            int extent = side ? dock->width() : dock->height();
            if (!dock->property("initialSizeApplied").toBool() || !dock->geometry().intersects(rect())) {
                const int initial = side ? (dock == detailDock ? 350 : 320) : (dock == detailDock ? 250 : 200);
                const int requested = std::min(initial, std::max(1, (side ? width() : height()) / 3));
                extent = dock->property("restorePanelSize").toBool() ? std::max(requested, extent) : requested;
                dock->setProperty("initialSizeApplied", true);
                dock->setProperty("restorePanelSize", false);
            }
            (side ? sidePanels : horizontalPanels).append(dock);
            (side ? widths : heights).append(std::max(1, extent));
        }
        // Resize both panels together so opening or moving one cannot consume
        // the other's entire dock area. These are requests, never minimums.
        if (!sidePanels.isEmpty()) resizeDocks(sidePanels, widths, Qt::Horizontal);
        if (!horizontalPanels.isEmpty()) resizeDocks(horizontalPanels, heights, Qt::Vertical);
    };
    for (auto *dock : {detailDock, systemDock}) {
        dock->setProperty("restorePanelSize", settings.contains("windowState"));
        // An unsized dock is temporarily placed offscreen. Qt then emits
        // visibilityChanged(false) even when it has just been shown.
        connect(dock->toggleViewAction(), &QAction::toggled, this, [this, initializePanels](bool visible) {
            if (visible) QTimer::singleShot(0, this, initializePanels);
        });
        connect(dock, &QDockWidget::dockLocationChanged, this, [this, dock, initializePanels] {
            dock->setProperty("initialSizeApplied", false);
            dock->setProperty("restorePanelSize", false);
            QTimer::singleShot(0, this, initializePanels);
        });
    }
    QTimer::singleShot(0, this, [this, initializePanels] {
        initializePanels();
        if (!settings.contains("detailSplit")) {
            const int sidebar = details->isExpanded() ? 296 : 40;
            detailSplit->setSizes({sidebar, qMax(1, detailSplit->width() - sidebar - detailSplit->handleWidth())});
        }
    });
    tabs->setCurrentIndex(settings.value("tab", 0).toInt());
    if (!desiredPid) desiredPid = settings.value(processSettingKey("pid"), settings_get()->preselected_pid).toInt();
    connect(&refreshTimer, &QTimer::timeout, this, &MainWindow::refresh); refreshTimer.start(1000);
    QTimer::singleShot(0, this, &MainWindow::refresh);
}

Inspector *MainWindow::addInspector(Inspector::Kind kind, const QString &title, QTabWidget *notebook)
{
    auto *inspector = new Inspector(kind, title, notebook);
    if (!inspector->available) { delete inspector; return nullptr; }
    bool enabled = true;
    for (const auto &entry : originalSettings.value("plugins").toArray()) {
        const auto plugin = entry.toObject();
        if (plugin.value("id").toString() == QLatin1String(inspector->descriptor.id)) enabled = plugin.value("enabled").toBool(true);
    }
    inspector->pluginEnabled = settings.value("plugins/" + QString::fromLatin1(inspector->descriptor.id), enabled).toBool();
    inspector->setMediaEnabled(mediaEnabled);
    inspectors.append(inspector); notebook->addTab(inspector, title);
    notebook->setTabVisible(notebook->indexOf(inspector), inspector->pluginEnabled);
    if (kind >= Inspector::SystemFiles) inspector->setPid(1);
    inspector->requestRefresh = [this] { syncWrites(); if (!brokerBusy) startBroker(); };
    inspector->openPid = [this](int pid) { selectPid(pid); };
    inspector->openPlugin = [this](const QString &id, int pid) {
        for (auto *candidate : inspectors) if (!candidate->pinned && candidate->pluginEnabled && QLatin1String(candidate->descriptor.id) == id) {
            openPluginWindow(candidate, pid); break;
        }
    };
    return inspector;
}
void MainWindow::syncRegistry()
{
    instances.clear(); instances.reserve(inspectors.size());
    registry = {};
    for (auto *inspector : inspectors) {
        plugin_instance_t inst{}; inst.plugin = &inspector->descriptor; inst.widget = inspector;
        inst.tracked_pid = inspector->pid; inst.pinned = inspector->pinned; inst.is_active = 1;
        instances.push_back(inst);
        registry.combined_needs = static_cast<evemon_data_needs_t>(registry.combined_needs | inspector->descriptor.data_needs);
    }
    registry.instances = instances.data(); registry.count = instances.size(); registry.capacity = instances.size();
}
void MainWindow::startBroker()
{
    if (closing || brokerBusy || inspectors.isEmpty()) return;
    // Join the finished worker before changing the registry used by its cycle.
    broker_cancel(); syncRegistry();
    bool active = false;
    for (auto *i : inspectors) active |= i->wantsData();
    if (!active) return;
    gatherGeneration = brokerGeneration;
    brokerBusy = broker_start(&registry, state.fdmon) != 0;
}
void MainWindow::refresh()
{
    if (closing) return;
    QElapsedTimer elapsed; elapsed.start();
    const int scroll = tree->verticalScrollBar()->value();
    const int horizontalScroll = tree->horizontalScrollBar()->value();
    for (auto *bar : {tree->verticalScrollBar(), tree->horizontalScrollBar()})
        bar->setProperty("evemonUpdatingModel", true);
    model->refresh(state);
#ifdef HAVE_PIPEWIRE
    // The audio filter must work even when no inspector is selected or open.
    pw_graph_t graph{};
    if (!query_is_foreign_host() && pw_snapshot(&graph) == 0) {
        QSet<int> pids;
        for (size_t i = 0; i < graph.node_count; ++i) {
            const auto &node = graph.nodes[i];
            if (node.pid > 0 && strstr(node.media_class, "Audio") && strstr(node.media_class, "Stream")) pids.insert(node.pid);
        }
        filter->setAudioPids(pids); pw_graph_free(&graph);
    }
#endif
    if (desiredPid && model->process(desiredPid)) {
        const auto index = filter->mapFromSource(model->forPid(desiredPid));
        if (index.isValid()) { for (auto p = index.parent(); p.isValid(); p = p.parent()) tree->expand(p); tree->setCurrentIndex(index); }
        desiredPid = 0;
    }
    tree->verticalScrollBar()->setValue(scroll);
    tree->horizontalScrollBar()->setValue(horizontalScroll);
    for (auto *bar : {tree->verticalScrollBar(), tree->horizontalScrollBar()})
        bar->setProperty("evemonUpdatingModel", false);
    refreshDetails();
    if (state.fdmon) fdmon_net_io_snapshot(state.fdmon);
    syncWrites(); if (!brokerBusy) startBroker();
    pthread_mutex_lock(&state.lock);
    const monitor_system_t system = state.system;
    const bool disconnected = state.remote && !state.connected;
    const uint count = state.snapshot.len;
    pthread_mutex_unlock(&state.lock);
    const quint64 total = system.memory_total;
    const quint64 available = std::min(system.memory_available, system.memory_total);
    refreshMilliseconds = elapsed.nsecsElapsed() / 1e6;
    profile__record("ui_render", refreshMilliseconds);
    double average = 0, maximum = 0;
    profile_get("ui_render", nullptr, &average, &maximum);
    summary->setValue(QString("%1 processes  |  %2 CPUs  |  memory: %3 / %4%5  |  render: %6 ms (avg %7, max %8)")
        .arg(count).arg(system.cpus).arg(formatBytes(total - available), formatBytes(total))
        .arg(gpuSummary).arg(refreshMilliseconds, 0, 'f', 1).arg(average, 0, 'f', 1).arg(maximum, 0, 'f', 1));
    load->setText(disconnected ? QStringLiteral("Evemon disconnected — reconnecting") :
        QString("up %1d %2h %3m  |  %4 user%5  |  load: %6 %7 %8")
        .arg(system.uptime_seconds / 86400).arg(system.uptime_seconds / 3600 % 24).arg(system.uptime_seconds / 60 % 60)
        .arg(system.users).arg(system.users == 1 ? "" : "s")
        .arg(system.load[0], 0, 'f', 2).arg(system.load[1], 0, 'f', 2).arg(system.load[2], 0, 'f', 2));
}
void MainWindow::restoreTree()
{
    const QSignalBlocker blocker(tree);
    tree->expandAll();
    for (auto it = collapsed.begin(); it != collapsed.end(); ) {
        const QModelIndex source = model->forPid(*it);
        if (!source.isValid()) { it = collapsed.erase(it); continue; }
        const QModelIndex index = filter->mapFromSource(source);
        if (index.isValid()) tree->collapse(index);
        ++it;
    }
    const auto selected = filter->mapFromSource(model->forPid(selectedPid));
    if (selected.isValid()) tree->selectionModel()->setCurrentIndex(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}
void MainWindow::selectPid(int pid)
{
    const auto source = model->forPid(pid);
    if (!source.isValid()) { statusBar()->showMessage(QString("PID %1 is not in the current snapshot.").arg(pid), 4000); return; }
    search->clear(); for (auto *action : filterActions) action->setChecked(false);
    const auto index = filter->mapFromSource(source);
    for (auto parent = index.parent(); parent.isValid(); parent = parent.parent()) tree->expand(parent);
    tree->setCurrentIndex(index); tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
}
void MainWindow::refreshDetails()
{
    const auto *row = model->process(selectedPid);
    uint64_t send = 0, receive = 0;
    if (state.fdmon) fdmon_net_io_get(state.fdmon, selectedPid, &send, &receive);
    const QIcon icon = row ? model->data(model->forPid(selectedPid, ProcessModel::Name), Qt::DecorationRole).value<QIcon>() : QIcon();
    details->setProcess(row, send, receive, icon);
    if (!row) return;
    if (metadataEnabled && softwareName != row->name) lookupSoftware(row->name);
    details->setSoftware(softwareFields);
    for (auto *inspector : inspectors) if (inspector->pid == selectedPid && row->died) inspector->setPid(0);
}
bool MainWindow::writerTracks(const Inspector *writer, int pid) const
{
    if (pid == writer->pid) return true;
    if (!writer->includesDescendants()) return false;
    QSet<int> seen;
    while (pid > 0 && !seen.contains(pid)) {
        if (pid == writer->pid) return true;
        seen.insert(pid);
        if (execParents.contains(pid)) pid = execParents.value(pid);
        else if (const auto *process = model->process(pid)) pid = process->entry.ppid;
        else return false;
    }
    return false;
}
void MainWindow::syncWrites()
{
    if (!state.fdmon || fdmon_active_backend(state.fdmon) != FDMON_BACKEND_EBPF) return;
    QMap<int, int> requested, parents;
    for (auto *writer : inspectors) {
        if (!writesEnabled || writer->kind != Inspector::Writes || !writer->wantsData() || !writer->writeMask()) continue;
        const int mask = writer->writeMask();
        const auto pids = writer->includesDescendants() ? model->descendants(writer->pid) : QList<int>{writer->pid};
        for (int pid : pids) requested[pid] |= mask;
        if (writer->includesDescendants()) {
            parents[writer->pid] |= mask;
            for (auto it = execParents.cbegin(); it != execParents.cend(); ++it)
                if (writerTracks(writer, it.key()) && model->process(it.key()) && !model->process(it.key())->died) requested[it.key()] |= mask;
        }
    }
    for (auto it = writeParents.cbegin(); it != writeParents.cend(); ++it)
        if (parents.value(it.key()) != it.value()) fdmon_unwatch_parent_fds(state.fdmon, it.key());
    for (auto it = parents.cbegin(); it != parents.cend(); ++it)
        if (writeParents.value(it.key()) != it.value()) fdmon_watch_parent_fds(state.fdmon, it.key(), it.value());
    for (auto it = writeFds.cbegin(); it != writeFds.cend(); ++it)
        for (int fd : {1, 2}) if ((it.value() & (1 << (fd - 1))) && !(requested.value(it.key()) & (1 << (fd - 1)))) fdmon_remove_pid_fd(state.fdmon, it.key(), fd);
    if (!requested.isEmpty()) fdmon_write_enable(state.fdmon);
    for (auto it = requested.cbegin(); it != requested.cend(); ++it)
        for (int fd : {1, 2}) if ((it.value() & (1 << (fd - 1))) && !(writeFds.value(it.key()) & (1 << (fd - 1)))) fdmon_add_pid_fd(state.fdmon, it.key(), fd);
    writeFds = requested; writeParents = parents; writePids.clear();
    for (int pid : writeFds.keys()) writePids.insert(pid);
    for (auto it = execParents.begin(); it != execParents.end(); ) {
        if (!writePids.contains(it.key())) it = execParents.erase(it); else ++it;
    }
}
void MainWindow::paintEvent(QPaintEvent *event)
{
    themeManager()->paintWindow(this, event);
}

void MainWindow::contextMenu(const QPoint &point)
{
    const auto index = tree->indexAt(point); if (!index.isValid()) return; tree->setCurrentIndex(index);
    const auto *r = model->process(selectedPid); if (!r || r->died) return;
    QMenu menu(this);
    menu.addAction(QString("%1 (%2, pid %3)").arg(model->isPinned(selectedPid) ? "Unpin Process" : "Pin Process", r->name).arg(selectedPid), this, [this] { model->pin(selectedPid); restoreTree(); });
    for (auto *action : filterActions) menu.addAction(action);
    populatePluginWindowMenu(menu.addMenu("Open Plugin as Window"));
    menu.addSeparator();
    menu.addAction("Copy Command", this, [this] { if (const auto *r = model->process(selectedPid)) QApplication::clipboard()->setText(r->command); });
    menu.addSeparator();
    menu.addAction(QString("End Process (%1, pid %2)").arg(r->name).arg(selectedPid), this, [this] { sendSignal(SIGTERM, false); });
    menu.addAction(QString("End Process Tree (%1, pid %2)").arg(r->name).arg(selectedPid), this, [this] { sendSignal(SIGTERM, true); });
    menu.addSeparator();
    const QList<QPair<QString, int>> signalChoices{
        {"SIGTERM (15) – Terminate", SIGTERM}, {"SIGKILL (9)  – Kill", SIGKILL}, {"SIGSTOP (19) – Stop", SIGSTOP}, {"SIGCONT (18) – Continue", SIGCONT}, {{}, 0},
        {"SIGINT  (2)  – Interrupt", SIGINT}, {"SIGHUP  (1)  – Hangup", SIGHUP}, {"SIGUSR1 (10)", SIGUSR1}, {"SIGUSR2 (12)", SIGUSR2}, {{}, 0},
        {"SIGABRT (6)  – Abort", SIGABRT}, {"SIGQUIT (3)  – Quit", SIGQUIT}};
    for (bool group : {false, true}) {
        auto *signalMenu = menu.addMenu(group ? "Send Signal to Tree" : "Send Signal");
        for (const auto &pair : signalChoices) {
            if (!pair.second) signalMenu->addSeparator();
            else signalMenu->addAction(pair.first, this, [this, pair, group] { sendSignal(pair.second, group); });
        }
    }
    menu.exec(tree->viewport()->mapToGlobal(point));
}
void MainWindow::populatePluginWindowMenu(QMenu *menu)
{
    const auto *process = model->process(selectedPid);
    menu->setEnabled(process && !process->died);
    for (auto *inspector : inspectors) if (!inspector->pinned && inspector->pluginEnabled)
        menu->addAction(inspector->menuTitle(), this, [this, inspector] { openPluginWindow(inspector); });
}
void MainWindow::sendSignal(int signal, bool group)
{
    const auto *r = model->process(selectedPid); if (!r || r->died) return;
    const auto pids = group ? model->descendants(selectedPid) : QList<int>{selectedPid};
    QList<proc_entry_t> targets;
    for (int pid : pids) {
        const auto *current = model->process(pid);
        if (current && !current->died) targets.append(current->entry);
    }
    if (signal != SIGCONT && QMessageBox::question(this, "Send Signal", QString("Send signal %1 to %2 (%3)%4?")
        .arg(signal).arg(r->name).arg(selectedPid).arg(group ? QString(" and its descendants (%1 processes total)").arg(pids.size()) : QString())) != QMessageBox::Yes) return;
    struct SignalRequest {
        QPointer<MainWindow> window;
        QList<proc_entry_t> targets;
        QByteArray path;
        QStringList errors;
        int signo;
    };
    auto *request = new SignalRequest{this, targets, QByteArray(query_remote_path()), {}, signal};
    GTask *task = g_task_new(nullptr, nullptr, [](GObject *, GAsyncResult *result, gpointer) {
        auto *request = static_cast<SignalRequest *>(g_task_get_task_data(G_TASK(result)));
        if (request->window && !request->errors.isEmpty())
            QMessageBox::warning(request->window, "Signal delivery", request->errors.join('\n'));
    }, nullptr);
    g_task_set_task_data(task, request, [](gpointer data) { delete static_cast<SignalRequest *>(data); });
    g_task_run_in_thread(task, [](GTask *task, gpointer, gpointer opaque, GCancellable *) {
        auto *request = static_cast<SignalRequest *>(opaque);
        for (const auto &target : request->targets) {
            if (evemon_shutdown_requested) break;
            if (query_signal_path(request->path.constData(), &target, request->signo))
                request->errors << QString("%1: %2").arg(target.pid).arg(strerror(errno));
        }
        g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
}
void MainWindow::openPluginWindow(Inspector *original, int requestedPid)
{
    const int targetPid = requestedPid > 0 ? requestedPid : selectedPid;
    const auto *process = model->process(targetPid); if (!process) return;
    auto *window = new ThemedWindow(this); window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QString("%1 — %2 (pid %3)").arg(original->title, process->name).arg(targetPid)); window->resize(720, 480);
    auto *inspector = new Inspector(original->kind, original->title, window); inspector->setMediaEnabled(mediaEnabled); inspector->pinned = true; inspector->setPid(targetPid);
    auto *layout = new QVBoxLayout(window); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(inspector); inspectors.append(inspector);
    connect(window, &QObject::destroyed, this, [this, inspector] {
        if (closing) return;
        ++brokerGeneration; broker_cancel(); inspectors.removeAll(inspector); syncWrites();
    });
    inspector->requestRefresh = [this] { syncWrites(); if (!brokerBusy) startBroker(); };
    inspector->openPid = original->openPid;
    inspector->openPlugin = original->openPlugin;
    presentAuxiliaryWindow(window); ++brokerGeneration; if (!brokerBusy) startBroker();
}
void MainWindow::lookupSoftware(const QString &name)
{
    softwareName = name; softwareFields.clear();
    char dataPath[4096];
    if (evemon_data_path("software-directory.sqlite", dataPath, sizeof(dataPath))) return;
    const QStringList paths = {QString::fromUtf8(dataPath)};
    sqlite3 *db = nullptr;
    for (const auto &path : paths) {
        if (!QFileInfo::exists(path)) continue;
        if (sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) break;
        sqlite3_close(db); db = nullptr;
    }
    if (!db) return;
    const char *sql = "SELECT display_name,organization_name,homepage,summary,primary_license,source_url,funding_url FROM program_cards "
        "WHERE package_name=?1 OR package_name IN (SELECT package_name FROM program_card_aliases WHERE alias=?1) LIMIT 1";
    sqlite3_stmt *query = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &query, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(query, 1, name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(query) == SQLITE_ROW) {
            auto value = [&](int column) { const auto *v = sqlite3_column_text(query, column); return v ? QString::fromUtf8(reinterpret_cast<const char *>(v)) : QString(); };
            softwareFields = {{"Organisation", value(1)}, {"Homepage", value(2)}, {"Source", value(5)}, {"Funding", value(6)}, {"Licence", value(4)}, {"Summary", value(3)}};
        }
    }
    sqlite3_finalize(query); sqlite3_close(db);
}
void MainWindow::restart(bool admin)
{
    if (!admin) { evemon_request_restart(); close(); return; }
    settings.sync();
    if (desktop_start_elevated(QCoreApplication::applicationFilePath().toUtf8().constData(), "qt"))
        QMessageBox::warning(this, "Restart", "Could not start /usr/bin/kdesu.");
}
QString MainWindow::processSettingKey(const QString &key) const
{
    return query_is_foreign_host()
        ? "hosts/" + QString::fromLatin1(QByteArray(query_host_name()).toHex()) + '/' + key : key;
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    settings.setValue("geometry", saveGeometry()); settings.setValue("windowState", saveState()); settings.setValue("columns", tree->header()->saveState());
    settings.setValue("procInfoOpen", details->isExpanded());
    settings.setValue("detailSplit", detailSplit->saveState()); settings.setValue("tab", tabs->currentIndex()); settings.setValue(processSettingKey("pid"), selectedPid);
    const QStringList filterKeys{"show_audio_only", "show_gpu_only", "show_cpu_intensive"};
    for (int i = 0; i < filterActions.size(); ++i) settings.setValue(processSettingKey(filterKeys[i]), filterActions[i]->isChecked());
    settings.setValue("spectro_theme", static_cast<evemon_settings_t *>(evemon_settings_get())->spectro_theme);
    settings.sync(); QMainWindow::closeEvent(event);
}
MainWindow::~MainWindow()
{
    closing = true; refreshTimer.stop(); broker_destroy();
    evemon_event_bus_unsubscribe(writeSubscription); evemon_event_bus_unsubscribe(execSubscription); evemon_event_bus_unsubscribe(gpuSubscription);
    if (state.fdmon) {
        for (int pid : writeParents.keys()) fdmon_unwatch_parent_fds(state.fdmon, pid);
        for (int pid : writePids) { fdmon_remove_pid_fd(state.fdmon, pid, 1); fdmon_remove_pid_fd(state.fdmon, pid, 2); }
    }

}

void MainWindow::showHeaderEditor(QLineEdit *entry, int column)
{
    entry->setParent(tree->header());
    entry->setGeometry(tree->header()->sectionViewportPosition(column), 0, tree->header()->sectionSize(column), tree->header()->height());
    entry->show(); entry->raise(); entry->setFocus(); entry->selectAll();
}
bool MainWindow::eventFilter(QObject *object, QEvent *event)
{
    if (evemon_debug && object == tree->viewport() && event->type() == QEvent::Resize) {
        const auto size = static_cast<QResizeEvent *>(event)->size();
        qDebug() << "Qt process viewport:" << size << "detail:" << detailDock->geometry() << "system:" << systemDock->geometry();
    }
    if ((object == search || object == pidEntry) && event->type() == QEvent::FocusOut) {
        if (object == pidEntry || search->text().isEmpty()) static_cast<QWidget *>(object)->hide();
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Alt && isActiveWindow()) altPressed = true;
        else if (key->modifiers() & Qt::AltModifier) {
            altPressed = false;
            // Hidden menu bars do not receive Qt's normal mnemonic handling.
            if (isActiveWindow() && key->modifiers() == Qt::AltModifier) {
                const QKeySequence pressed(QKeyCombination(Qt::AltModifier, Qt::Key(key->key())));
                for (auto *action : menuBar()->actions()) {
                    if (QKeySequence::mnemonic(action->text()) != pressed || !action->menu()) continue;
                    menuBar()->show(); menuBar()->setActiveAction(action);
                    action->menu()->popup(menuBar()->mapToGlobal(menuBar()->actionGeometry(action).bottomLeft()));
                    for (auto *item : action->menu()->actions()) {
                        if (!item->isEnabled() || !item->isVisible() || item->isSeparator()) continue;
                        action->menu()->setActiveAction(item); break;
                    }
                    return true;
                }
            }
        }
        if (key->key() == Qt::Key_Escape && (object == search || object == pidEntry)) {
            static_cast<QLineEdit *>(object)->clear(); static_cast<QWidget *>(object)->hide(); tree->setFocus(); return true;
        }
    }
    if (event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Alt && altPressed && isActiveWindow()) {
            altPressed = false; menuBar()->show();
            menuBar()->setFocus(); menuBar()->setActiveAction(menuBar()->actions().front());
            return true;
        }
        altPressed = false;
    }
    return QMainWindow::eventFilter(object, event);
}
