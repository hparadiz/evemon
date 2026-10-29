# Writing Plugins

Plugins are shared objects that export a single entry point:

```c
evemon_plugin_t *evemon_plugin_init(void);
```

The returned descriptor declares what data the plugin needs (`evemon_NEED_FDS`, `evemon_NEED_ENV`, etc.) and provides callbacks: `create_widget`, `update`, `clear`, `destroy`, `activate` (for host services injection), and optional `wants_update`, `is_available`, and `set_active` hooks. The host handles all `/proc` I/O, threading, and layout. Plugins just render.

Use the `EVEMON_PLUGIN_MANIFEST` macro to embed a relocation-free flat manifest:

```c
EVEMON_PLUGIN_MANIFEST(
    "org.evemon.myplugin",  /* id */
    "My Plugin",            /* name */
    "1.0",                  /* version */
    EVEMON_ROLE_PROCESS,    /* role */
    NULL                    /* dependencies (NULL-terminated list) */
);
```

See [`src/plugins/evemon_plugin.h`](../src/plugins/evemon_plugin.h) for the full ABI.

Compile and drop into `build/plugins/`:

```sh
gcc -shared -fPIC -o evemon_myplugin.so myplugin.c $(pkg-config --cflags --libs gtk+-3.0)
```
