#include "../plugins/broker_data.h"
#include <errno.h>
#include <stdlib.h>

/* All fields have fixed byte order and width; strings are length-prefixed. */
static void put_fd(ipc_buf_t *b, const evemon_fd_t *v)
{
    ipc_put_u32(b, v->fd);
    ipc_put_string(b, v->path);
    ipc_put_string(b, v->desc);
    ipc_put_u32(b, v->category);
    ipc_put_u64(b, v->net_sort_key);
    ipc_put_u32(b, v->source_pid);
}
static void get_fd(ipc_buf_t *b, evemon_fd_t *v)
{
    v->fd = ipc_get_u32(b);
    ipc_get_fixed(b, v->path, sizeof(v->path));
    ipc_get_fixed(b, v->desc, sizeof(v->desc));
    v->category = ipc_get_u32(b);
    v->net_sort_key = ipc_get_u64(b);
    v->source_pid = ipc_get_u32(b);
}
static void put_env(ipc_buf_t *b, const evemon_env_t *v)
{
    ipc_put_string(b, v->text);
    ipc_put_u32(b, v->category);
}
static void get_env(ipc_buf_t *b, evemon_env_t *v)
{
    v->text = ipc_get_string(b);
    v->category = ipc_get_u32(b);
}
static void put_mmap(ipc_buf_t *b, const evemon_mmap_t *v)
{
    ipc_put_string(b, v->text);
    ipc_put_u32(b, v->category);
    ipc_put_u64(b, v->size_kb);
}
static void get_mmap(ipc_buf_t *b, evemon_mmap_t *v)
{
    v->text = ipc_get_string(b);
    v->category = ipc_get_u32(b);
    v->size_kb = ipc_get_u64(b);
}
static void put_lib(ipc_buf_t *b, const evemon_lib_t *v)
{
    ipc_put_string(b, v->path);
    ipc_put_string(b, v->name);
    ipc_put_string(b, v->version);
    ipc_put_string(b, v->origin);
    ipc_put_u32(b, v->category);
    ipc_put_u64(b, v->size_kb);
}
static void get_lib(ipc_buf_t *b, evemon_lib_t *v)
{
    ipc_get_fixed(b, v->path, sizeof(v->path));
    ipc_get_fixed(b, v->name, sizeof(v->name));
    ipc_get_fixed(b, v->version, sizeof(v->version));
    ipc_get_fixed(b, v->origin, sizeof(v->origin));
    v->category = ipc_get_u32(b);
    v->size_kb = ipc_get_u64(b);
}
static void put_sock(ipc_buf_t *b, const evemon_socket_t *v)
{
    ipc_put_string(b, v->desc);
    ipc_put_u64(b, v->send_delta);
    ipc_put_u64(b, v->recv_delta);
    ipc_put_u64(b, v->total);
    ipc_put_u32(b, v->source_pid);
}
static void get_sock(ipc_buf_t *b, evemon_socket_t *v)
{
    ipc_get_fixed(b, v->desc, sizeof(v->desc));
    v->send_delta = ipc_get_u64(b);
    v->recv_delta = ipc_get_u64(b);
    v->total = ipc_get_u64(b);
    v->source_pid = ipc_get_u32(b);
}
static void put_thread(ipc_buf_t *b, const evemon_thread_t *v)
{
    ipc_put_u32(b, v->tid);
    ipc_put_string(b, v->name);
    ipc_put_u32(b, v->state);
    ipc_put_u32(b, v->priority);
    ipc_put_u32(b, v->nice);
    ipc_put_u32(b, v->processor);
    ipc_put_u64(b, v->utime);
    ipc_put_u64(b, v->stime);
    ipc_put_u64(b, v->starttime);
    ipc_put_f64(b, v->cpu_percent);
    ipc_put_u64(b, v->voluntary_ctxt_switches);
    ipc_put_u64(b, v->nonvoluntary_ctxt_switches);
}
static void get_thread(ipc_buf_t *b, evemon_thread_t *v)
{
    v->tid = ipc_get_u32(b);
    ipc_get_fixed(b, v->name, sizeof(v->name));
    v->state = ipc_get_u32(b);
    v->priority = ipc_get_u32(b);
    v->nice = ipc_get_u32(b);
    v->processor = ipc_get_u32(b);
    v->utime = ipc_get_u64(b);
    v->stime = ipc_get_u64(b);
    v->starttime = ipc_get_u64(b);
    v->cpu_percent = ipc_get_f64(b);
    v->voluntary_ctxt_switches = ipc_get_u64(b);
    v->nonvoluntary_ctxt_switches = ipc_get_u64(b);
}
static void put_pw_node(ipc_buf_t *b, const evemon_pw_node_t *v)
{
    ipc_put_u32(b, v->id);
    ipc_put_u32(b, v->client_id);
    ipc_put_u32(b, v->pid);
    ipc_put_string(b, v->app_name);
    ipc_put_string(b, v->node_name);
    ipc_put_string(b, v->node_desc);
    ipc_put_string(b, v->media_class);
    ipc_put_string(b, v->media_name);
}
static void get_pw_node(ipc_buf_t *b, evemon_pw_node_t *v)
{
    v->id = ipc_get_u32(b);
    v->client_id = ipc_get_u32(b);
    v->pid = ipc_get_u32(b);
    ipc_get_fixed(b, v->app_name, sizeof(v->app_name));
    ipc_get_fixed(b, v->node_name, sizeof(v->node_name));
    ipc_get_fixed(b, v->node_desc, sizeof(v->node_desc));
    ipc_get_fixed(b, v->media_class, sizeof(v->media_class));
    ipc_get_fixed(b, v->media_name, sizeof(v->media_name));
}
static void put_pw_link(ipc_buf_t *b, const evemon_pw_link_t *v)
{
    ipc_put_u32(b, v->output_node);
    ipc_put_u32(b, v->output_port);
    ipc_put_u32(b, v->input_node);
    ipc_put_u32(b, v->input_port);
}
static void get_pw_link(ipc_buf_t *b, evemon_pw_link_t *v)
{
    v->output_node = ipc_get_u32(b);
    v->output_port = ipc_get_u32(b);
    v->input_node = ipc_get_u32(b);
    v->input_port = ipc_get_u32(b);
}
static void put_pw_port(ipc_buf_t *b, const evemon_pw_port_t *v)
{
    ipc_put_u32(b, v->id);
    ipc_put_u32(b, v->node_id);
    ipc_put_string(b, v->port_name);
    ipc_put_string(b, v->direction);
    ipc_put_string(b, v->format_dsp);
}
static void get_pw_port(ipc_buf_t *b, evemon_pw_port_t *v)
{
    v->id = ipc_get_u32(b);
    v->node_id = ipc_get_u32(b);
    ipc_get_fixed(b, v->port_name, sizeof(v->port_name));
    ipc_get_fixed(b, v->direction, sizeof(v->direction));
    ipc_get_fixed(b, v->format_dsp, sizeof(v->format_dsp));
}
static void put_mpris(ipc_buf_t *b, const evemon_mpris_player_t *v)
{
    ipc_put_string(b, v->identity);
    ipc_put_string(b, v->desktop_entry);
    ipc_put_u32(b, v->pid);
    ipc_put_string(b, v->bus_name);
    ipc_put_string(b, v->playback_status);
    ipc_put_string(b, v->loop_status);
    ipc_put_u32(b, v->shuffle);
    ipc_put_f64(b, v->volume);
    ipc_put_f64(b, v->rate);
    ipc_put_u64(b, v->position_us);
    ipc_put_string(b, v->track_title);
    ipc_put_string(b, v->track_artist);
    ipc_put_string(b, v->track_album);
    ipc_put_string(b, v->track_album_artist);
    ipc_put_string(b, v->art_url);
    ipc_put_string(b, v->track_id);
    ipc_put_string(b, v->genre);
    ipc_put_u64(b, v->length_us);
    ipc_put_u32(b, v->track_number);
    ipc_put_u32(b, v->disc_number);
    ipc_put_string(b, v->content_type);
    ipc_put_string(b, v->url);
    ipc_put_u32(b, v->can_play);
    ipc_put_u32(b, v->can_pause);
    ipc_put_u32(b, v->can_seek);
    ipc_put_u32(b, v->can_go_next);
    ipc_put_u32(b, v->can_go_previous);
    ipc_put_u32(b, v->can_control);
}
static void get_mpris(ipc_buf_t *b, evemon_mpris_player_t *v)
{
    ipc_get_fixed(b, v->identity, sizeof(v->identity));
    ipc_get_fixed(b, v->desktop_entry, sizeof(v->desktop_entry));
    v->pid = ipc_get_u32(b);
    ipc_get_fixed(b, v->bus_name, sizeof(v->bus_name));
    ipc_get_fixed(b, v->playback_status, sizeof(v->playback_status));
    ipc_get_fixed(b, v->loop_status, sizeof(v->loop_status));
    v->shuffle = ipc_get_u32(b);
    v->volume = ipc_get_f64(b);
    v->rate = ipc_get_f64(b);
    v->position_us = ipc_get_u64(b);
    ipc_get_fixed(b, v->track_title, sizeof(v->track_title));
    ipc_get_fixed(b, v->track_artist, sizeof(v->track_artist));
    ipc_get_fixed(b, v->track_album, sizeof(v->track_album));
    ipc_get_fixed(b, v->track_album_artist, sizeof(v->track_album_artist));
    ipc_get_fixed(b, v->art_url, sizeof(v->art_url));
    ipc_get_fixed(b, v->track_id, sizeof(v->track_id));
    ipc_get_fixed(b, v->genre, sizeof(v->genre));
    v->length_us = ipc_get_u64(b);
    v->track_number = ipc_get_u32(b);
    v->disc_number = ipc_get_u32(b);
    ipc_get_fixed(b, v->content_type, sizeof(v->content_type));
    ipc_get_fixed(b, v->url, sizeof(v->url));
    v->can_play = ipc_get_u32(b);
    v->can_pause = ipc_get_u32(b);
    v->can_seek = ipc_get_u32(b);
    v->can_go_next = ipc_get_u32(b);
    v->can_go_previous = ipc_get_u32(b);
    v->can_control = ipc_get_u32(b);
}
static void put_cgroup(ipc_buf_t *b, const evemon_cgroup_t *v)
{
    ipc_put_string(b, v->path);
    ipc_put_u64(b, v->mem_current);
    ipc_put_u64(b, v->mem_max);
    ipc_put_u64(b, v->mem_high);
    ipc_put_u64(b, v->swap_max);
    ipc_put_u64(b, v->cpu_quota);
    ipc_put_u64(b, v->cpu_period);
    ipc_put_u64(b, v->pids_current);
    ipc_put_u64(b, v->pids_max);
    ipc_put_string(b, v->io_max);
}
static void get_cgroup(ipc_buf_t *b, evemon_cgroup_t *v)
{
    ipc_get_fixed(b, v->path, sizeof(v->path));
    v->mem_current = ipc_get_u64(b);
    v->mem_max = ipc_get_u64(b);
    v->mem_high = ipc_get_u64(b);
    v->swap_max = ipc_get_u64(b);
    v->cpu_quota = ipc_get_u64(b);
    v->cpu_period = ipc_get_u64(b);
    v->pids_current = ipc_get_u64(b);
    v->pids_max = ipc_get_u64(b);
    ipc_get_fixed(b, v->io_max, sizeof(v->io_max));
}

static void *get_array(ipc_buf_t *b, size_t size, size_t minimum, size_t *count, size_t *budget)
{
    uint32_t n = ipc_get_u32(b);
    if (b->error || n > 65536 || n > (b->len - b->pos) / minimum || n > *budget / size) {
        b->error = EPROTO; return NULL;
    }
    void *p = calloc(n ? n : 1, size);
    if (!p) { b->error = ENOMEM; return NULL; }
    *budget -= n * size; *count = n; return p;
}
int query_inspect_encode(pid_t pid, unsigned needs, void *fdmon, ipc_buf_t *b)
{
    broker_pid_data_t *owned = broker_gather(pid, needs, fdmon);
    if (!owned) return IPC_INTERNAL;
    const evemon_proc_data_t *d = &owned->data;
    ipc_put_u32(b, d->pid); ipc_put_u32(b, d->ppid);
    ipc_put_string(b, d->name); ipc_put_string(b, d->cmdline);
    ipc_put_string(b, d->user); ipc_put_string(b, d->cwd);
    ipc_put_f64(b, d->cpu_percent); ipc_put_u64(b, d->rss_kb);
    ipc_put_u32(b, d->descendant_count);
    for (size_t i = 0; i < d->descendant_count; ++i) ipc_put_u32(b, d->descendant_pids[i]);
    ipc_put_u32(b, d->fd_count);
    for (size_t i = 0; i < d->fd_count && !b->error; ++i) put_fd(b, &d->fds[i]);
    ipc_put_u32(b, d->env_count);
    for (size_t i = 0; i < d->env_count && !b->error; ++i) put_env(b, &d->envs[i]);
    ipc_put_u32(b, d->mmap_count);
    for (size_t i = 0; i < d->mmap_count && !b->error; ++i) put_mmap(b, &d->mmaps[i]);
    ipc_put_u32(b, d->lib_count);
    for (size_t i = 0; i < d->lib_count && !b->error; ++i) put_lib(b, &d->libs[i]);
    ipc_put_u32(b, d->socket_count);
    for (size_t i = 0; i < d->socket_count && !b->error; ++i) put_sock(b, &d->sockets[i]);
    ipc_put_u32(b, d->thread_count);
    for (size_t i = 0; i < d->thread_count && !b->error; ++i) put_thread(b, &d->threads[i]);
    ipc_put_u32(b, d->pw_node_count);
    for (size_t i = 0; i < d->pw_node_count && !b->error; ++i) put_pw_node(b, &d->pw_nodes[i]);
    ipc_put_u32(b, d->pw_link_count);
    for (size_t i = 0; i < d->pw_link_count && !b->error; ++i) put_pw_link(b, &d->pw_links[i]);
    ipc_put_u32(b, d->pw_port_count);
    for (size_t i = 0; i < d->pw_port_count && !b->error; ++i) put_pw_port(b, &d->pw_ports[i]);
    ipc_put_u32(b, d->mpris_player_count);
    for (size_t i = 0; i < d->mpris_player_count && !b->error; ++i) put_mpris(b, &d->mpris_players[i]);
    ipc_put_u32(b, d->cgroup != NULL);
    if (d->cgroup) put_cgroup(b, d->cgroup);
    ipc_put_string(b, d->raw_status); ipc_put_string(b, d->raw_maps); ipc_put_string(b, d->raw_cgroup);
    broker_pid_data_free(owned); free(owned);
    return b->error ? IPC_INTERNAL : 0;
}
broker_pid_data_t *query_inspect_decode(ipc_buf_t *b)
{
    broker_pid_data_t *owned = calloc(1, sizeof(*owned));
    if (!owned) return NULL;
    owned->decoded = 1;
    evemon_proc_data_t *d = &owned->data;
    d->pid = ipc_get_u32(b); d->ppid = ipc_get_u32(b); owned->pid = d->pid;
    d->name = ipc_get_string(b); d->cmdline = ipc_get_string(b);
    d->user = ipc_get_string(b); d->cwd = ipc_get_string(b);
    d->cpu_percent = ipc_get_f64(b); d->rss_kb = ipc_get_u64(b);
    size_t budget = IPC_LIMIT;
    owned->desc_pids = get_array(b, sizeof(pid_t), 4, &d->descendant_count, &budget);
    d->descendant_pids = owned->desc_pids;
    for (size_t i = 0; i < d->descendant_count && !b->error; ++i) owned->desc_pids[i] = ipc_get_u32(b);
    owned->fd_buf = get_array(b, sizeof(*owned->fd_buf), 28, &d->fd_count, &budget);
    d->fds = owned->fd_buf;
    for (size_t i = 0; i < d->fd_count && !b->error; ++i) get_fd(b, &owned->fd_buf[i]);
    owned->env_buf = get_array(b, sizeof(*owned->env_buf), 8, &d->env_count, &budget);
    d->envs = owned->env_buf;
    for (size_t i = 0; i < d->env_count && !b->error; ++i) get_env(b, &owned->env_buf[i]);
    owned->mmap_buf = get_array(b, sizeof(*owned->mmap_buf), 16, &d->mmap_count, &budget);
    d->mmaps = owned->mmap_buf;
    for (size_t i = 0; i < d->mmap_count && !b->error; ++i) get_mmap(b, &owned->mmap_buf[i]);
    owned->lib_buf = get_array(b, sizeof(*owned->lib_buf), 28, &d->lib_count, &budget);
    d->libs = owned->lib_buf;
    for (size_t i = 0; i < d->lib_count && !b->error; ++i) get_lib(b, &owned->lib_buf[i]);
    owned->sock_buf = get_array(b, sizeof(*owned->sock_buf), 32, &d->socket_count, &budget);
    d->sockets = owned->sock_buf;
    for (size_t i = 0; i < d->socket_count && !b->error; ++i) get_sock(b, &owned->sock_buf[i]);
    owned->thread_buf = get_array(b, sizeof(*owned->thread_buf), 72, &d->thread_count, &budget);
    d->threads = owned->thread_buf;
    for (size_t i = 0; i < d->thread_count && !b->error; ++i) get_thread(b, &owned->thread_buf[i]);
    owned->pw_nodes = get_array(b, sizeof(*owned->pw_nodes), 32, &d->pw_node_count, &budget);
    d->pw_nodes = owned->pw_nodes;
    for (size_t i = 0; i < d->pw_node_count && !b->error; ++i) get_pw_node(b, &owned->pw_nodes[i]);
    owned->pw_links = get_array(b, sizeof(*owned->pw_links), 16, &d->pw_link_count, &budget);
    d->pw_links = owned->pw_links;
    for (size_t i = 0; i < d->pw_link_count && !b->error; ++i) get_pw_link(b, &owned->pw_links[i]);
    owned->pw_ports = get_array(b, sizeof(*owned->pw_ports), 20, &d->pw_port_count, &budget);
    d->pw_ports = owned->pw_ports;
    for (size_t i = 0; i < d->pw_port_count && !b->error; ++i) get_pw_port(b, &owned->pw_ports[i]);
    owned->mpris_players = get_array(b, sizeof(*owned->mpris_players), 128, &d->mpris_player_count, &budget);
    d->mpris_players = owned->mpris_players;
    for (size_t i = 0; i < d->mpris_player_count && !b->error; ++i) get_mpris(b, &owned->mpris_players[i]);
    uint32_t cgroup = ipc_get_u32(b);
    if (cgroup > 1) b->error = EPROTO;
    if (cgroup && !b->error) { get_cgroup(b, &owned->cgroup_data); d->cgroup = &owned->cgroup_data; }
    owned->raw_status = ipc_get_string(b); d->raw_status = owned->raw_status;
    owned->raw_maps = ipc_get_string(b); d->raw_maps = owned->raw_maps;
    owned->raw_cgroup = ipc_get_string(b); d->raw_cgroup = owned->raw_cgroup;
    if (!ipc_done(b)) { broker_pid_data_free(owned); free(owned); errno = EPROTO; return NULL; }
    return owned;
}
