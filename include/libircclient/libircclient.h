#pragma once

/* Minimal stub of libircclient headers for building in environments where the
 * real headers are unavailable. This is sufficient for the usage in
 * waywall/irc.c.
 */
#include <stddef.h>

typedef struct irc_session_s irc_session_t;

typedef void (*irc_event_callback_t)(irc_session_t *session, const char *event,
                                     const char *origin, const char **params,
                                     unsigned int count);
typedef void (*irc_eventcode_callback_t)(irc_session_t *session, unsigned int event,
                                         const char *origin, const char **params,
                                         unsigned int count);

typedef struct {
    irc_event_callback_t event_connect;
    irc_event_callback_t event_join;
    irc_event_callback_t event_part;
    irc_event_callback_t event_quit;
    irc_event_callback_t event_privmsg;
    irc_eventcode_callback_t event_numeric;
    irc_event_callback_t event_unknown;
} irc_callbacks_t;

irc_session_t *irc_create_session(irc_callbacks_t *callbacks);
void irc_destroy_session(irc_session_t *session);
int irc_connect(irc_session_t *session, const char *server, unsigned short port,
                const char *server_password, const char *nick, const char *username,
                const char *realname);
int irc_run(irc_session_t *session);
int irc_send_raw(irc_session_t *session, const char *message);
int irc_disconnect(irc_session_t *session);
int irc_errno(irc_session_t *session);
const char *irc_strerror(int err);
