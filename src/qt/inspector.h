#pragma once
#include <QWidget>
#include <QLabel>
#include <functional>
extern "C" {
#include "../plugins/evemon_plugin.h"
}
class WriteView;

class Inspector : public QWidget {
public:
    enum Kind { Audio, Network, Environment, Writes, Files, Threads, Devices, Maps, Libraries, Cgroup, Gentoo, MilkDrop, SystemFiles, SystemLibraries, Gpu };
    Inspector(Kind kind, const QString &title, QWidget *parent = nullptr);
    ~Inspector() override;
    Kind kind;
    QString title;
    evemon_plugin_t descriptor{};
    int pid = 0;
    bool pinned = false;
    bool pluginEnabled = true;
    bool available = true;
    std::function<void()> requestRefresh;
    std::function<void(int)> openPid;
    std::function<void(const QString &, int)> openPlugin;
    void setPid(int value);
    void updateData(const evemon_proc_data_t &data);
    void setMessage(const QString &message);
    void appendWrite(const evemon_fd_write_payload_t &write);
    bool wantsData() const;
    bool includesDescendants() const;
    void refreshNeeds();
    WriteView *writes = nullptr;
    int writeMask() const;
    void setMediaEnabled(bool enabled);
    QString menuTitle() const { return native && native->name ? QString::fromUtf8(native->name) : title; }
    QLabel *status = nullptr;
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    bool rendererLoading = false;
    QWidget *renderer = nullptr;
    QWidget *nativeRoot = nullptr;
    evemon_plugin_t *native = nullptr;
    evemon_host_services_t nativeServices{};
    void *audioServices = nullptr;
    QByteArray idBytes, titleBytes;
    evemon_plugin_t *mediaService = nullptr;
    void (*rendererUpdate)(QWidget *, const evemon_proc_data_t *) = nullptr;
    void (*rendererClear)(QWidget *) = nullptr;
};
