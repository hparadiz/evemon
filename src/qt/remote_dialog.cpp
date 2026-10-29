#include <gio/gio.h>
#include "remote_dialog.h"
#include "main_window.h"
#include "theme.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
extern "C" {
#include "../app/client_session.h"
#include "../platform/desktop_session.h"
}

struct ConnectionDialog {
    QPointer<QDialog> dialog;
    QPointer<MainWindow> window;
    QPushButton *connect;
    GCancellable *cancel;
};

static void connected(GObject *, GAsyncResult *result, gpointer opaque)
{
    auto *job = static_cast<ConnectionDialog *>(opaque);
    GError *error = nullptr;
    auto *session = client_session_finish(result, &error);
    if (job->dialog && job->window && !g_cancellable_is_cancelled(job->cancel)) {
        if (session) {
            client_session_queue(session);
            job->dialog->accept();
            job->window->close();
            session = nullptr;
        } else {
            job->connect->setEnabled(true);
        }
    }
    client_session_discard(session);
    g_clear_error(&error); g_object_unref(job->cancel); delete job;
}
void showRemoteConnectionDialog(MainWindow *window)
{
    auto *dialog = new QDialog(window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("connectRemoteDialog");
    dialog->setWindowTitle("Connect to Remote");
    dialog->setWindowModality(Qt::WindowModal);
    auto *layout = new QFormLayout(dialog);
    layout->setVerticalSpacing(6);
    auto *host = new QLineEdit(dialog);
    host->setObjectName("connectRemoteHost"); host->setPlaceholderText("Hostname, IP address, or user@host");
    auto *socket = new QLineEdit(dialog);
    socket->setObjectName("connectRemoteSocket"); socket->setPlaceholderText("Automatic on remote host");
    socket->setToolTip("Blank finds an accessible system or SSH-user Evemon socket on the remote host.");
    layout->addRow("SSH host", host); layout->addRow("Socket override (optional)", socket);
    auto *runDaemon = new QCheckBox("Run Daemon on Host", dialog);
    runDaemon->setObjectName("connectRemoteRunDaemon"); runDaemon->setChecked(true);
    auto *idleShutdown = new QCheckBox("Self Shutdown Daemon after", dialog);
    idleShutdown->setObjectName("connectRemoteIdleShutdown"); idleShutdown->setChecked(true);
    auto *idleMinutes = new QSpinBox(dialog);
    idleMinutes->setObjectName("connectRemoteIdleMinutes");
    idleMinutes->setRange(1, 10080); idleMinutes->setValue(5);
    auto *idleRow = new QWidget(dialog); auto *idleLayout = new QHBoxLayout(idleRow);
    idleLayout->setContentsMargins(0, 0, 0, 0);
    idleLayout->addWidget(idleShutdown); idleLayout->addWidget(idleMinutes);
    idleLayout->addWidget(new QLabel("min", idleRow)); idleLayout->addStretch();
    layout->addRow(runDaemon); layout->addRow(idleRow);
    QObject::connect(runDaemon, &QCheckBox::toggled, idleShutdown, &QWidget::setEnabled);
    QObject::connect(runDaemon, &QCheckBox::toggled, dialog,
        [=](bool enabled) {
            idleMinutes->setEnabled(enabled && idleShutdown->isChecked());
        });
    QObject::connect(idleShutdown, &QCheckBox::toggled, dialog,
        [=](bool enabled) { idleMinutes->setEnabled(enabled && runDaemon->isChecked()); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto *button = buttons->addButton("Connect", QDialogButtonBox::AcceptRole);
    button->setObjectName("connectRemoteSubmit"); button->setDefault(true);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(button, &QPushButton::clicked, dialog, [=] {
        auto *cancel = g_cancellable_new();
        QObject::connect(dialog, &QDialog::finished, dialog, [cancel] { g_cancellable_cancel(cancel); });
        // The dialog retains one reference until destruction; the pending job owns another.
        g_object_ref(cancel);
        QObject::connect(dialog, &QObject::destroyed, [cancel] { g_cancellable_cancel(cancel); g_object_unref(cancel); });
        button->setEnabled(false);
        auto *job = new ConnectionDialog{dialog, window, button, cancel};
        const auto destination = host->text().trimmed().toUtf8(), path = socket->text().trimmed().toUtf8();
        const unsigned idle = idleShutdown->isChecked()
            ? unsigned(idleMinutes->value()) : 0;
        client_session_prepare(destination.constData(), path.constData(),
                               runDaemon->isChecked(), idle, cancel,
                               connected, job);
    });
    dialog->resize(460, dialog->sizeHint().height()); presentAuxiliaryWindow(dialog); host->setFocus();
}
