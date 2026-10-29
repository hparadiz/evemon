#include "process_model.h"
#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <unistd.h>

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

QString formatBytes(double bytes)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (bytes >= 1024 && unit < 4) { bytes /= 1024; ++unit; }
    return QString::number(bytes, 'f', unit ? 1 : 0) + ' ' + units[unit];
}

ProcessModel::ProcessModel(QObject *parent) : QAbstractItemModel(parent) {}

QModelIndex ProcessModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount || parent.column() > 0) return {};
    const auto &list = parent.isValid() ? static_cast<Node *>(parent.internalPointer())->children : roots;
    return row < int(list.size()) ? createIndex(row, column, list[row]) : QModelIndex();
}

QModelIndex ProcessModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) return {};
    const Node *node = static_cast<Node *>(index.internalPointer())->parent;
    return node ? createIndex(node->position, 0, node) : QModelIndex();
}

int ProcessModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0) return 0;
    return int(parent.isValid() ? static_cast<Node *>(parent.internalPointer())->children.size() : roots.size());
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    static const QStringList titles = {"PID", "PPID", "User", "Name", "CPU%", "GPU", "VRAM", "ENC",
        "Memory (RSS)", "Group Memory (RSS)", "Group CPU%", "Disk Read", "Disk Write", "I/O History",
        "Start Time", "Container", "Service", "CWD", "Command"};
    return orientation == Qt::Horizontal && role == Qt::DisplayRole ? QVariant(titles.value(section)) : QVariant();
}

QVariant ProcessModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) return {};
    const auto &r = static_cast<Node *>(index.internalPointer())->row;
    const auto &e = r.entry;
    const int col = index.column();
    if (role == PidRole) return e.pid;
    if (role == PinnedRole) return pins.contains(e.pid);
    if (role == DeadRole) return r.died != 0;
    if (role == GpuRole) return r.gpuClient;
    if (role == Qt::SizeHintRole) return QSize(0, std::max(20, QFontMetrics(QApplication::font()).height() + 4));
    if (role == Qt::ToolTipRole) return r.command;
    if (role == HistoryRole) {
        QVariantList values;
        for (int i = 0; i < e.io_history_len; ++i) values.append(e.io_history[i]);
        return values;
    }
    if (role == Qt::TextAlignmentRole && col >= Cpu && col <= Write)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::FontRole && pins.contains(e.pid)) { QFont font = QApplication::font(); font.setBold(true); return font; }
    if (role == Qt::ForegroundRole && r.died) return QColor(190, 110, 110);
    if (role == Qt::BackgroundRole) {
        const qint64 since = QDateTime::currentMSecsSinceEpoch() - (r.died ? r.died : r.born);
        if (since < 2000) {
            QColor color = r.died ? QColor(170, 45, 45) : QColor(45, 160, 85);
            color.setAlpha(int(65 * (1.0 - since / 2000.0)));
            return color;
        }
    }
    if (role == Qt::DecorationRole && col == Name) {
        if (!icons.contains(r.name)) {
            QIcon icon = QIcon::fromTheme(r.name);
            if (icon.isNull() && r.name == "evemon") icon = QIcon(":/evemon/icon.png");
            icons.insert(r.name, icon);
        }
        return icons.value(r.name);
    }
    if (role != Qt::DisplayRole && role != SortRole) return {};
    const bool raw = role == SortRole;
    switch (col) {
    case Pid: return e.pid;
    case Ppid: return e.ppid;
    case User: return r.user;
    case Name: return r.name;
    case Cpu: return raw ? QVariant(e.cpu_percent) : QVariant(QString::number(e.cpu_percent, 'f', 1) + '%');
    case Gpu: return raw ? QVariant(r.gpu) : QVariant(r.gpuClient ? QString::number(r.gpu) + '%' : QStringLiteral("–"));
    case Vram: return raw ? QVariant::fromValue(r.vram) : QVariant(r.gpuClient ? formatBytes(r.vram) : QStringLiteral("–"));
    case Encoder: return raw ? QVariant(r.encoder) : QVariant(r.gpuClient ? QString::number(r.encoder) + '%' : QStringLiteral("–"));
    case Memory: return raw ? QVariant::fromValue(quint64(e.mem_rss_kb)) : QVariant(formatBytes(e.mem_rss_kb * 1024.0));
    case GroupMemory: return raw ? QVariant::fromValue(r.groupMemory) : QVariant(formatBytes(r.groupMemory * 1024.0));
    case GroupCpu: return raw ? QVariant(r.groupCpu) : QVariant(QString::number(r.groupCpu, 'f', 1) + '%');
    case Read: return raw ? QVariant(e.io_read_rate) : QVariant(formatBytes(e.io_read_rate) + "/s");
    case Write: return raw ? QVariant(e.io_write_rate) : QVariant(formatBytes(e.io_write_rate) + "/s");
    case Start: return raw ? QVariant::fromValue(quint64(e.start_time)) : QVariant(QDateTime::fromSecsSinceEpoch(e.start_time).toString("yyyy-MM-dd HH:mm:ss"));
    case Container: return r.container;
    case Service: return r.service;
    case Cwd: return r.cwd;
    case Command: return r.command;
    default: return {};
    }
}

void ProcessModel::refresh(monitor_state_t &state)
{
    std::vector<ProcessRow> snapshot;
    pthread_mutex_lock(&state.lock);
    const auto &s = state.snapshot;
    snapshot.reserve(s.len);
    for (uint i = 0; i < s.len; ++i) {
        const auto &e = s.entries[i];
        ProcessRow row;
        row.entry = e;
        row.name = QString::fromUtf8(PROC_NAME(&s, &e));
        row.command = QString::fromUtf8(PROC_CMDLINE(&s, &e));
        row.user = QString::fromUtf8(PROC_USER(&s, &e));
        row.cwd = QString::fromUtf8(PROC_CWD(&s, &e));
        row.container = QString::fromUtf8(PROC_CONTAINER(&s, &e));
        row.service = QString::fromUtf8(PROC_SERVICE(&s, &e));
        if (e.extra_idx < s.extras_len && s.extras[e.extra_idx].steam) {
            const auto &st = *s.extras[e.extra_idx].steam;
            row.steam = QString("%1\nApp ID: %2\nProton: %3\nRuntime: %4\nPrefix: %5\nGame directory: %6")
                .arg(st.display_label, st.app_id, st.proton_version, st.runtime_layer, st.compat_data, st.game_dir);
        }
        snapshot.push_back(std::move(row));
    }
    pthread_mutex_unlock(&state.lock);
    if (snapshot.empty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSet<int> seen;
    bool topology = false;
    for (const auto &r : snapshot) {
        seen.insert(r.entry.pid);
        const Node *node = byPid.value(r.entry.pid);
        if (!node || node->row.entry.ppid != r.entry.ppid || node->row.entry.start_time != r.entry.start_time) topology = true;
    }
    for (const auto &node : nodes)
        if (!seen.contains(node->row.entry.pid) && node->row.died && now - node->row.died >= 2000) topology = true;
    if (topology) beginResetModel();
    for (auto &row : snapshot) {
        Node *node = byPid.value(row.entry.pid);
        if (!node) {
            auto added = std::make_unique<Node>();
            node = added.get();
            nodes.push_back(std::move(added));
            byPid.insert(row.entry.pid, node);
            row.born = firstSnapshot ? 0 : now;
        } else {
            row.born = node->row.entry.start_time == row.entry.start_time ? node->row.born : now;
            row.gpu = node->row.gpu; row.vram = node->row.vram;
            row.encoder = node->row.encoder; row.gpuClient = node->row.gpuClient;
        }
        node->row = std::move(row);
    }
    for (auto &node : nodes) {
        auto &row = node->row;
        if (!seen.contains(row.entry.pid) && !row.died) row.died = now;
    }
    if (topology) {
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const auto &node) {
            if (node->row.died && now - node->row.died >= 2000) {
                byPid.remove(node->row.entry.pid); pins.remove(node->row.entry.pid); return true;
            }
            return false;
        }), nodes.end());
    }
    rebuild();
    firstSnapshot = false;
    if (topology) endResetModel(); else notify();
}

void ProcessModel::rebuild()
{
    roots.clear();
    for (auto &n : nodes) { n->children.clear(); n->parent = nullptr; n->row.groupCpu = n->row.entry.cpu_percent; n->row.groupMemory = n->row.entry.mem_rss_kb; }
    for (auto &n : nodes) {
        // Walk the original ancestry, keeping aggregate accounting independent of pin placement.
        QSet<int> visited{n->row.entry.pid};
        auto *p = byPid.value(n->row.entry.ppid);
        while (p && !visited.contains(p->row.entry.pid)) {
            visited.insert(p->row.entry.pid);
            if (!n->row.died) { p->row.groupCpu += n->row.entry.cpu_percent; p->row.groupMemory += n->row.entry.mem_rss_kb; }
            p = byPid.value(p->row.entry.ppid);
        }
        p = byPid.value(n->row.entry.ppid);
        if (p && p != n.get() && !pins.contains(n->row.entry.pid)) {
            QSet<int> chain{n->row.entry.pid};
            auto *ancestor = p;
            while (ancestor && !chain.contains(ancestor->row.entry.pid)) {
                chain.insert(ancestor->row.entry.pid); ancestor = byPid.value(ancestor->row.entry.ppid);
            }
            if (ancestor) p = nullptr;
        } else p = nullptr;
        n->parent = p;
        auto &siblings = p ? p->children : roots;
        n->position = int(siblings.size()); siblings.push_back(n.get());
    }
}

void ProcessModel::notify(Node *parentNode)
{
    const auto &list = parentNode ? parentNode->children : roots;
    const QModelIndex p = parentNode ? createIndex(parentNode->position, 0, parentNode) : QModelIndex();
    if (!list.empty()) emit dataChanged(index(0, 0, p), index(int(list.size()) - 1, ColumnCount - 1, p));
    for (auto *node : list) if (!node->children.empty()) notify(node);
}

void ProcessModel::pin(int pid) { beginResetModel(); if (pins.contains(pid)) pins.remove(pid); else pins.insert(pid); rebuild(); endResetModel(); }
const ProcessRow *ProcessModel::process(int pid) const { const auto *n = byPid.value(pid); return n ? &n->row : nullptr; }
QModelIndex ProcessModel::forPid(int pid, int column) const { auto *n = byPid.value(pid); return n ? createIndex(n->position, column, n) : QModelIndex(); }
QList<int> ProcessModel::descendants(int pid) const
{
    QList<int> result{pid};
    QSet<int> seen{pid};
    for (qsizetype i = 0; i < result.size(); ++i)
        for (const auto &n : nodes) if (!n->row.died && n->row.entry.ppid == result[i] && !seen.contains(n->row.entry.pid)) {
            seen.insert(n->row.entry.pid); result.append(n->row.entry.pid);
        }
    return result;
}
void ProcessModel::setGpu(const QHash<int, QVector<quint64>> &values)
{
    for (auto &n : nodes) {
        const auto v = values.value(n->row.entry.pid);
        n->row.gpuClient = !v.isEmpty(); n->row.gpu = v.value(0); n->row.vram = v.value(1); n->row.encoder = v.value(2);
    }
    notify();
}
ProcessFilter::ProcessFilter(QObject *parent) : QSortFilterProxyModel(parent) { setRecursiveFilteringEnabled(true); setSortRole(ProcessModel::SortRole); setSortCaseSensitivity(Qt::CaseInsensitive); }
void ProcessFilter::setQuery(const QString &value) { query = value; invalidate(); }
void ProcessFilter::setCpuOnly(bool value) { cpuOnly = value; invalidate(); }
void ProcessFilter::setGpuOnly(bool value) { gpuOnly = value; invalidate(); }
void ProcessFilter::setAudioOnly(bool value) { audioOnly = value; invalidate(); }
void ProcessFilter::setAudioPids(const QSet<int> &value) { if (value != audioPids) { audioPids = value; invalidate(); } }
bool ProcessFilter::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const auto idx = sourceModel()->index(row, 0, parent);
    if (cpuOnly && sourceModel()->index(row, ProcessModel::GroupCpu, parent).data(sortRole()).toDouble() < 50.0 / std::max(1L, sysconf(_SC_NPROCESSORS_ONLN))) return false;
    if (gpuOnly && !idx.data(ProcessModel::GpuRole).toBool()) return false;
    if (audioOnly && !audioPids.contains(idx.data(ProcessModel::PidRole).toInt())) return false;
    return query.isEmpty()
        || sourceModel()->index(row, ProcessModel::Name, parent).data().toString().contains(query, Qt::CaseInsensitive);
}
bool ProcessFilter::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const bool lp = left.data(ProcessModel::PinnedRole).toBool(), rp = right.data(ProcessModel::PinnedRole).toBool();
    if (lp != rp) return sortOrder() == Qt::AscendingOrder ? lp : rp;
    return QSortFilterProxyModel::lessThan(right, left);
}
void SparklineDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyledItemDelegate::paint(p, option, index);
    const auto values = index.data(ProcessModel::HistoryRole).toList();
    if (values.isEmpty()) return;
    double peak = 1;
    for (const auto &v : values) peak = std::max(peak, v.toDouble());
    const QRectF rect = option.rect.adjusted(3, 3, -3, -3);
    p->save(); p->setClipRect(rect);
    const qreal width = rect.width() / IO_HISTORY_LEN;
    for (int i = 0; i < values.size(); ++i) {
        const qreal height = std::max(1.0, rect.height() * values[i].toDouble() / peak);
        p->fillRect(QRectF(rect.left() + i * width, rect.bottom() - height, std::max(1.0, width - 1), height), QColor(85, 157, 221));
    }
    p->restore();
}
