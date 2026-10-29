#pragma once

#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QHash>
#include <QIcon>
#include <QSet>
#include <memory>
#include <vector>
extern "C" {
#include "../core/proc.h"
}

QString formatBytes(double bytes);
QString readText(const QString &path);

struct ProcessRow {
    proc_entry_t entry{};
    QString name, command, user, cwd, container, service, steam;
    double groupCpu = 0;
    quint64 groupMemory = 0;
    qint64 born = 0, died = 0;
    uint gpu = 0, encoder = 0;
    quint64 vram = 0;
    bool gpuClient = false;
};

class ProcessModel : public QAbstractItemModel {
public:
    enum Column { Pid, Ppid, User, Name, Cpu, Gpu, Vram, Encoder, Memory,
        GroupMemory, GroupCpu, Read, Write, History, Start, Container, Service,
        Cwd, Command, ColumnCount };
    enum Role { SortRole = Qt::UserRole + 1, PidRole, PinnedRole, HistoryRole, DeadRole, GpuRole };
    explicit ProcessModel(QObject *parent = nullptr);
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return ColumnCount; }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void refresh(monitor_state_t &state);
    void pin(int pid);
    bool isPinned(int pid) const { return pins.contains(pid); }
    const ProcessRow *process(int pid) const;
    QModelIndex forPid(int pid, int column = 0) const;
    QList<int> descendants(int pid) const;
    void setGpu(const QHash<int, QVector<quint64>> &values);
    bool firstSnapshot = true;
    QSet<int> pins;
private:
    struct Node {
        ProcessRow row;
        Node *parent = nullptr;
        std::vector<Node *> children;
        int position = 0;
    };
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<Node *> roots;
    QHash<int, Node *> byPid;
    mutable QHash<QString, QIcon> icons;
    void rebuild();
    void notify(Node *parent = nullptr);
};

class ProcessFilter : public QSortFilterProxyModel {
public:
    explicit ProcessFilter(QObject *parent = nullptr);
    void setQuery(const QString &value);
    void setCpuOnly(bool value);
    void setGpuOnly(bool value);
    void setAudioOnly(bool value);
    void setAudioPids(const QSet<int> &value);
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
private:
    QString query;
    bool cpuOnly = false, gpuOnly = false, audioOnly = false;
    QSet<int> audioPids;
};

class SparklineDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
