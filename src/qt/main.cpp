#include <gio/gio.h>
#include "main_window.h"
#include <QApplication>
#include <QTimer>
extern "C" {
#include "../app/runtime.h"
#include "../ipc/query.h"
#include "../app/client_session.h"
#include "../plugins/event_bus.h"
#include "../plugins/plugin_broker.h"
#include <glib.h>
#ifdef HAVE_PIPEWIRE
#include "ui/pipewire_graph.h"
#endif
}
extern "C" int evemon_ui_run(monitor_state_t *state, int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("evemon");
    QCoreApplication::setApplicationName("evemon");
    QGuiApplication::setDesktopFileName("evemon");
    QCoreApplication::setApplicationVersion(EVEMON_VERSION);
    int result;
    do {
#ifdef HAVE_PIPEWIRE
        broker_set_pipewire_gatherer(pw_broker_gather);
        if (!query_is_foreign_host()) pw_watcher_start();
#endif
      {
        MainWindow window(*state, state->preselect_pid, evemon_safe_mode);
        QTimer events;
        QObject::connect(&events, &QTimer::timeout, &window, [&window] {
            for (int i = 0; i < 64 && g_main_context_pending(nullptr); ++i)
                g_main_context_iteration(nullptr, false);
            if (evemon_shutdown_requested) window.close();
        });
        events.start(25);
        window.show();
        result = app.exec();
      }
#ifdef HAVE_PIPEWIRE
        pw_watcher_stop();
#endif
        if (evemon_shutdown_requested || !client_session_commit()) break;
        state = client_session_state();
        state->preselect_pid = 0; /* Restore the new host's Qt selection. */
    } while (true);
    return result;
}
