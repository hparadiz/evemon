extern "C" {
#include "../ipc/query.h"
}
#include "inspector.h"
#include "../app/runtime.h"
#include "write_view.h"
#include <QCoreApplication>
#include <QLibrary>
#include <QShowEvent>
#include <QDir>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QVBoxLayout>

extern "C" {
#include "../plugins/event_bus.h"
#ifdef HAVE_PIPEWIRE
evemon_plugin_t *evemon_qt_pipewire_init(void);
evemon_plugin_t *evemon_qt_audio_service_init(void);
void *evemon_qt_audio_services(evemon_host_services_t *, void *, void (*)(void *, const char *, pid_t, const char *));
void evemon_qt_audio_destroy(void *);
#endif
evemon_plugin_t *evemon_qt_net_init(void);
evemon_plugin_t *evemon_qt_gentoo_init(void);
#ifdef HAVE_NVTOP
evemon_plugin_t *evemon_qt_gpu_init(void);
#endif
evemon_plugin_t *evemon_qt_fd_init(void);
evemon_plugin_t *evemon_qt_env_init(void);
evemon_plugin_t *evemon_qt_devices_init(void);
evemon_plugin_t *evemon_qt_mmap_init(void);
evemon_plugin_t *evemon_qt_libs_init(void);
evemon_plugin_t *evemon_qt_threads_init(void);
evemon_plugin_t *evemon_qt_cgroup_init(void);
evemon_plugin_t *evemon_qt_system_fd_init(void);
evemon_plugin_t *evemon_qt_system_libs_init(void);
}

Inspector::Inspector(Kind type, const QString &name, QWidget *parent) : QWidget(parent), kind(type), title(name)
{
    titleBytes = name.toUtf8();
    const char *ids[] = {"pipewire", "net", "env", "write_monitor_ui", "fd", "threads", "devices", "mmap", "libs", "cgroup", "gentoo", "milkdrop", "system_fd", "system_libs", "gpu"};
    idBytes = "org.evemon." + QByteArray(ids[type]);
    descriptor.abi_version = evemon_PLUGIN_ABI_VERSION;
    descriptor.id = idBytes.constData(); descriptor.name = titleBytes.constData();
    descriptor.plugin_ctx = this;
    descriptor.role = type >= SystemFiles ? EVEMON_ROLE_SYSTEM : EVEMON_ROLE_PROCESS;
    descriptor.update = [](void *ctx, const evemon_proc_data_t *data) { static_cast<Inspector *>(ctx)->updateData(*data); };
    descriptor.wants_update = [](void *ctx) { return int(static_cast<Inspector *>(ctx)->wantsData()); };
    switch (kind) {
    #ifdef HAVE_PIPEWIRE
    case Audio: native = evemon_qt_pipewire_init(); break;
    #endif
    case Gentoo: native = evemon_qt_gentoo_init(); break;
    case Network: native = evemon_qt_net_init(); break;
#ifdef HAVE_NVTOP
    case Gpu: native = evemon_qt_gpu_init(); break;
#endif
    case Files: native = evemon_qt_fd_init(); break;
    case Environment: native = evemon_qt_env_init(); break;
    case Devices: native = evemon_qt_devices_init(); break;
    case Maps: native = evemon_qt_mmap_init(); break;
    case Libraries: native = evemon_qt_libs_init(); break;
    case Threads: native = evemon_qt_threads_init(); break;
    case Cgroup: native = evemon_qt_cgroup_init(); break;
    case SystemFiles: native = evemon_qt_system_fd_init(); break;
    case SystemLibraries: native = evemon_qt_system_libs_init(); break;
    default: break;
    }
    nativeServices.publish = host_event_publish;
    nativeServices.subscribe = host_event_subscribe;
    nativeServices.unsubscribe = host_event_unsubscribe;
#ifdef HAVE_PIPEWIRE
    if (kind == Audio || kind == MilkDrop) {
        audioServices = evemon_qt_audio_services(&nativeServices, this, [](void *p, const char *id, pid_t pid, const char *) {
            auto *i = static_cast<Inspector *>(p); if (i->openPlugin) i->openPlugin(QString::fromUtf8(id), pid);
        });
        setMediaEnabled(true);
    }
#endif
    if ((kind == Gpu || kind == Gentoo) && !native) { available = false; return; }
    if (native) {
        descriptor.data_needs = native->data_needs;
        descriptor.id = native->id;
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        nativeRoot = static_cast<QWidget *>(native->create_widget(native->plugin_ctx));
        auto *viewport = new QScrollArea(this); viewport->setWidgetResizable(true);
        viewport->setFrameShape(QFrame::NoFrame); viewport->setWidget(nativeRoot);
        layout->addWidget(viewport);
        if (native->activate) native->activate(native->plugin_ctx, &nativeServices);
        return;
    }
    if (kind == Writes) {
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
        writes = new WriteView(this); layout->addWidget(writes);
        writes->changed = [this] { refreshNeeds(); if (requestRefresh) requestRefresh(); };
        refreshNeeds(); return;
    }
    auto *box = new QVBoxLayout(this); box->setContentsMargins(0, 0, 0, 0); box->setSpacing(0);
    status = new QLabel(this); status->setWordWrap(true); status->hide(); box->addWidget(status);
    refreshNeeds();
}

void Inspector::setMediaEnabled(bool enabled)
{
#ifdef HAVE_PIPEWIRE
    enabled = enabled && !query_is_foreign_host();
    if (!audioServices || enabled == bool(mediaService)) return;
    if (enabled) {
        mediaService = evemon_qt_audio_service_init();
        if (mediaService) mediaService->activate(mediaService->plugin_ctx, &nativeServices);
    } else {
        mediaService->destroy(mediaService->plugin_ctx); free(mediaService); mediaService = nullptr;
        if (native) native->clear(native->plugin_ctx);
    }
#else
    Q_UNUSED(enabled);
#endif
}

Inspector::~Inspector() {
    delete renderer;
    if (mediaService) { mediaService->destroy(mediaService->plugin_ctx); free(mediaService); }
    if (native) { native->destroy(native->plugin_ctx); free(native); }
#ifdef HAVE_PIPEWIRE
    evemon_qt_audio_destroy(audioServices);
#endif
    delete nativeRoot;
}
void Inspector::refreshNeeds()
{
    if (native) return;
    int needs = kind == MilkDrop ? evemon_NEED_PIPEWIRE | evemon_NEED_MPRIS : 0;
    if (includesDescendants()) needs |= evemon_NEED_DESCENDANTS;
    descriptor.data_needs = static_cast<evemon_data_needs_t>(needs);
}
bool Inspector::includesDescendants() const { return writes ? writes->includesDescendants() : bool(descriptor.data_needs & evemon_NEED_DESCENDANTS); }
bool Inspector::wantsData() const { return pluginEnabled && isVisible() && pid > 0; }
void Inspector::setPid(int value)
{
    if (value == pid) return;
    pid = value;
    if (mediaService) mediaService->clear(mediaService->plugin_ctx);
    if (native) native->clear(native->plugin_ctx);
    if (writes) writes->clear();
    if (renderer && rendererClear) rendererClear(renderer);
}
void Inspector::setMessage(const QString &message) { if (status) status->setText(message); }
void Inspector::updateData(const evemon_proc_data_t &data)
{
    if (data.pid != pid || !wantsData()) return;
    if (native) native->update(native->plugin_ctx, &data);
    if (renderer && rendererUpdate) rendererUpdate(renderer, &data);
    // Consumers establish the PID before metadata is published to the event bus.
    if (mediaService) mediaService->update(mediaService->plugin_ctx, &data);
}
void Inspector::appendWrite(const evemon_fd_write_payload_t &write) { if (writes) writes->append(write); }
int Inspector::writeMask() const { return writes ? writes->mask() : 0; }

void Inspector::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
#ifdef HAVE_MILKDROP
    if (kind != MilkDrop || renderer || rendererLoading) return;
    // The first QOpenGLWidget recreates its native top-level window and can
    // deliver another show event while the renderer factory is still running.
    const QScopedValueRollback<bool> loading(rendererLoading, true);
    const QStringList paths = {QCoreApplication::applicationDirPath() + "/plugins-qt/libevemon_qt_milkdrop.so",
        QStringLiteral(EVEMON_QT_PLUGIN_DIR) + "/libevemon_qt_milkdrop.so"};
    auto *library = new QLibrary(this);
    using Factory = QWidget *(*)(QWidget *, const evemon_host_services_t *, const char *);
    Factory factory = nullptr;
    for (const auto &path : paths) {
        library->setFileName(path); factory = reinterpret_cast<Factory>(library->resolve("evemon_qt_milkdrop_create"));
        if (factory) break;
    }
    rendererUpdate = reinterpret_cast<decltype(rendererUpdate)>(library->resolve("evemon_qt_milkdrop_update"));
    rendererClear = reinterpret_cast<decltype(rendererClear)>(library->resolve("evemon_qt_milkdrop_clear"));
    if (!factory || !rendererUpdate || !rendererClear) { status->setText("MilkDrop renderer could not load: " + library->errorString()); status->show(); return; }
    char dataPath[4096];
    if (evemon_data_path("milkdrop/presets", dataPath, sizeof(dataPath))) return;
    QString presets = QString::fromUtf8(dataPath);
    renderer = factory(this, &nativeServices, presets.toUtf8().constData());
    static_cast<QVBoxLayout *>(layout())->addWidget(renderer, 1);
#endif
}

void Inspector::hideEvent(QHideEvent *event)
{
    if (native && (kind == Audio || kind == SystemFiles || kind == SystemLibraries)) native->clear(native->plugin_ctx);
    if (mediaService) mediaService->clear(mediaService->plugin_ctx);
    QWidget::hideEvent(event);
}
