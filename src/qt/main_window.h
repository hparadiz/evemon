#pragma once
#include "process_model.h"
#include "inspector.h"
#include <QMainWindow>
#include <QTreeView>
#include <QLineEdit>
#include <QDockWidget>
#include <QTabWidget>
#include <QSplitter>
#include <QTimer>
#include "user_settings.h"
#include "detail_panel.h"
#include <QJsonObject>
extern "C" {
#include "../plugins/plugin_broker.h"
#include "../ipc/query.h"
}
class MainWindow : public QMainWindow {
public:
    MainWindow(monitor_state_t &state, int initialPid, bool safeMode);
    ~MainWindow() override;
    void selectPid(int pid);
protected:
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    monitor_state_t &state;
    ProcessModel *model;
    ProcessFilter *filter;
    QTreeView *tree;
    QLineEdit *search, *pidEntry;
    QDockWidget *detailDock, *systemDock;
    QTabWidget *tabs, *systemTabs;
    QSplitter *detailSplit;
    DetailPanel *details;
    DetailValue *summary;
    QLabel *load;
    QList<Inspector *> inspectors;
    plugin_registry_t registry{};
    std::vector<plugin_instance_t> instances;
    QTimer refreshTimer;
    UserSettings settings;
    QSet<int> collapsed;
    int selectedPid = 0, desiredPid = 0;
    int writeSubscription = 0, execSubscription = 0, gpuSubscription = 0;
    QSet<int> writePids;
    QMap<int, int> writeFds, writeParents, execParents;
    bool writerTracks(const Inspector *writer, int pid) const;
    bool mediaEnabled = true, metadataEnabled = true, writesEnabled = true;
    bool brokerBusy = false, closing = false;
    bool altPressed = false;
    double refreshMilliseconds = 0;
    QString gpuSummary;
    qint64 brokerGeneration = 0, gatherGeneration = 0;
    QMap<QString, QString> softwareFields;
    QJsonObject originalSettings;
    QList<QAction *> filterActions;
    QString processSettingKey(const QString &key) const;
    void showHeaderEditor(QLineEdit *entry, int column);
    bool eventFilter(QObject *object, QEvent *event) override;
    QString softwareName;
    void refresh();
    void refreshDetails();
    void startBroker();
    void syncRegistry();
    void menus();
    void contextMenu(const QPoint &point);
    void populatePluginWindowMenu(QMenu *menu);
    void openPluginWindow(Inspector *original, int requestedPid = 0);
    void sendSignal(int signal, bool descendants);
    void syncWrites();
    void lookupSoftware(const QString &name);
    void restart(bool admin);
    Inspector *addInspector(Inspector::Kind type, const QString &name, QTabWidget *notebook);
    void restoreTree();
};
