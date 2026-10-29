#pragma once
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Resolve before settings, toolkit initialization, or worker creation. */
void desktop_session_init(void);
uid_t desktop_session_uid(void);
gid_t desktop_session_gid(void);
const char *desktop_session_user(void);
/* Root may load immutable build artifacts owned by the invoking desktop user. */
int desktop_session_trusts_owner(uid_t owner);
/* Child setup only, after desktop_session_init() ran in the parent. */
void desktop_child_use_user(void *unused);
/* Caller frees the shell-quoted command with g_free(). */
char *desktop_elevation_command(const char *executable, const char *frontend);
int desktop_start_elevated(const char *executable, const char *frontend);
/* Only the application's own settings files/directories may be passed here. */
void desktop_settings_owner(const char *path);
#ifdef __cplusplus
}
#endif
