#include "irc.h"
#include "util/log.h"
#include <config/vm.h>
#include <libircclient/libircclient.h>
#include <lua.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Irc_client *all_clients[MAX_CLIENTS] = {0};
static int client_count = 0;
static pthread_mutex_t clients_mutex =
    PTHREAD_MUTEX_INITIALIZER; // for client_count, all_clients, callbacks, and
                               // callbacks_initialised

static irc_callbacks_t callbacks = {0};
static bool callbacks_initialized = false;

static int pushed_count = 0;
static int popped_count = 0;

static bool
queue_is_full(struct message_queue *q) {
    return ((q->write_pos + 1) % MAX_QUEUED_MESSAGES) == q->read_pos;
}

static bool
queue_is_empty(struct message_queue *q) {
    return q->write_pos == q->read_pos;
}

static void
queue_init(struct message_queue *q) {
    if (!q)
        return;
    for (int i = 0; i < MAX_QUEUED_MESSAGES; i++) {
        free(q->messages[i]);
        q->messages[i] = NULL;
    }
    q->write_pos = 0;
    q->read_pos = 0;
}

static void
queue_push(struct Irc_client *client, const char *message) {
    pthread_mutex_lock(&client->queue_mutex);
    struct message_queue *q = &client->message_queue;

    if (queue_is_full(q)) {
        ww_log(LOG_WARN, "Message queue full for client %d. Dropping message.", client->index);
        pthread_mutex_unlock(&client->queue_mutex);
        return;
    }

    char *copy = strdup(message);
    if (!copy) {
        ww_log(LOG_ERROR, "strdup failed");
        pthread_mutex_unlock(&client->queue_mutex);
        return;
    }

    q->messages[q->write_pos] = copy;
    q->write_pos = (q->write_pos + 1) % MAX_QUEUED_MESSAGES;

    pushed_count++;

    pthread_mutex_unlock(&client->queue_mutex);
}

static void
queue_cleanup(struct message_queue *q) {
    if (!q)
        return;
    for (int i = 0; i < MAX_QUEUED_MESSAGES; i++) {
        free(q->messages[i]);
        q->messages[i] = NULL;
    }
    q->write_pos = 0;
    q->read_pos = 0;
}

static struct Irc_client *
find_client_by_session(irc_session_t *session) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (all_clients[i] && all_clients[i]->session == session) {
            struct Irc_client *client = all_clients[i];
            pthread_mutex_unlock(&clients_mutex);
            return client;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

static void
format_irc_message(char *buf, size_t size, const char *prefix, const char *origin,
                   const char **params, unsigned int count) {
    int n = snprintf(buf, size, "%s", prefix ? prefix : "");
    if (n < 0 || (size_t)n >= size) {
        buf[size - 1] = '\0';
        return;
    }

    if (origin && (size_t)n < size - 1) {
        int ret = snprintf(buf + n, size - n, " from %s", origin);
        if (ret > 0 && (size_t)ret < size - n)
            n += ret;
    }

    for (unsigned int i = 0; i < count && (size_t)n < size - 1; i++) {
        if (params[i]) {
            int ret = snprintf(buf + n, size - n, " %s", params[i]);
            if (ret > 0 && (size_t)ret < size - n)
                n += ret;
        }
    }
    buf[size - 1] = '\0';
}

void
on_any_numeric(irc_session_t *session, unsigned int event, const char *origin, const char **params,
               unsigned int count) {
    struct Irc_client *client = find_client_by_session(session);
    if (!client)
        return;

    char buf[MAX_MESSAGE_LENGTH];
    char ev_str[32];
    snprintf(ev_str, sizeof(ev_str), "%u", event);
    format_irc_message(buf, sizeof(buf), ev_str, origin, params, count);
    queue_push(client, buf);
}

void
on_any_event(irc_session_t *session, const char *event, const char *origin, const char **params,
             unsigned int count) {
    struct Irc_client *client = find_client_by_session(session);
    if (!client || !event)
        return;

    char buf[MAX_MESSAGE_LENGTH];
    format_irc_message(buf, sizeof(buf), event, origin, params, count);
    queue_push(client, buf);
}

static void *
irc_thread(void *arg) {
    struct Irc_client *client = arg;
    if (!client || !client->session) {
        ww_log(LOG_ERROR, "Invalid client in IRC thread");
        return NULL;
    }

    ww_log(LOG_INFO, "IRC thread starting for client %d", client->index);

    int ret = irc_run(client->session);
    if (ret != 0)
        ww_log(LOG_WARN, "irc_run() exited with error: %s",
               irc_strerror(irc_errno(client->session)));
    else
        ww_log(LOG_INFO, "irc_run() exited normally");

    ww_log(LOG_INFO, "IRC thread ending for client %d", client->index);
    return NULL;
}

struct Irc_client *
irc_client_create(const char *ip, long port, const char *nick, const char *pass, int callback,
                  lua_State *L) {
    if (!ip || !nick || !L) {
        ww_log(LOG_ERROR, "Invalid parameters for IRC client creation");
        return NULL;
    }

    pthread_mutex_lock(&clients_mutex);

    if (client_count >= MAX_CLIENTS) {
        ww_log(LOG_ERROR, "Too many IRC clients (max %d)", MAX_CLIENTS);
        return NULL;
    }

    if (!callbacks_initialized) {
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.event_numeric = on_any_numeric;
        callbacks.event_unknown = on_any_event;
        callbacks.event_privmsg = on_any_event;
        callbacks.event_connect = on_any_event;
        callbacks.event_join = on_any_event;
        callbacks.event_part = on_any_event;
        callbacks.event_quit = on_any_event;
        callbacks_initialized = true;
    }

    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!all_clients[i]) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        ww_log(LOG_ERROR, "No free IRC client slots");
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    irc_session_t *session = irc_create_session(&callbacks);
    if (!session) {
        ww_log(LOG_ERROR, "Failed to create IRC session");
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    struct Irc_client *client = calloc(1, sizeof(struct Irc_client));
    if (!client) {
        irc_destroy_session(session);
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    client->session = session;
    client->callback = callback;
    client->index = slot;
    client->thread_running = false;
    queue_init(&client->message_queue);
    pthread_mutex_init(&client->queue_mutex, NULL);
    client->vm = config_vm_from(L);

    all_clients[slot] = client;
    client_count++;

    if (irc_connect(session, ip, port, pass, nick, nick, nick) != 0) {
        ww_log(LOG_ERROR, "IRC connection failed: %s", irc_strerror(irc_errno(session)));
        all_clients[slot] = NULL;
        client_count--;
        queue_cleanup(&client->message_queue);
        irc_destroy_session(session);
        pthread_mutex_destroy(&client->queue_mutex);
        free(client);
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    if (pthread_create(&client->thread_id, NULL, irc_thread, client) != 0) {
        ww_log(LOG_ERROR, "Failed to create IRC thread");
        irc_disconnect(session);
        all_clients[slot] = NULL;
        client_count--;
        queue_cleanup(&client->message_queue);
        irc_destroy_session(session);
        pthread_mutex_destroy(&client->queue_mutex);
        free(client);
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    client->thread_running = true;
    ww_log(LOG_INFO, "IRC client created successfully (slot %d)", slot);
    pthread_mutex_unlock(&clients_mutex);
    return client;
}

void
irc_client_send(struct Irc_client *client, const char *message) {
    if (!client || !client->session || !message) {
        ww_log(LOG_WARN, "Invalid parameters for IRC send");
        return;
    }

    if (!client->thread_running) {
        ww_log(LOG_WARN, "Cannot send to disconnected IRC client");
        return;
    }

    int ret = irc_send_raw(client->session, message);
    if (ret != 0) {
        ww_log(LOG_WARN, "Failed to send IRC message: %s",
               irc_strerror(irc_errno(client->session)));
    }
}

void
irc_client_destroy(struct Irc_client *client) {
    if (!client)
        return;

    ww_log(LOG_INFO, "Destroying IRC client %d", client->index);

    if (client->thread_running && client->session) {
        irc_disconnect(client->session);
        pthread_join(client->thread_id, NULL);
        client->thread_running = false;
    }

    if (client->session) {
        irc_destroy_session(client->session);
        client->session = NULL;
    }

    queue_cleanup(&client->message_queue);

    pthread_mutex_destroy(&client->queue_mutex);

    luaL_unref(client->vm->L, LUA_REGISTRYINDEX, client->callback);

    pthread_mutex_lock(&clients_mutex);
    if (client->index >= 0 && client->index < MAX_CLIENTS) {
        all_clients[client->index] = NULL;
        client_count--;
    }
    pthread_mutex_unlock(&clients_mutex);

    free(client);
    ww_log(LOG_INFO, "IRC client destroyed");
    ww_log(LOG_INFO, "%d pushed, %d popped.", pushed_count, popped_count);
}

void
manage_new_messages() {
    struct Irc_client *clients_snapshot[MAX_CLIENTS];
    int count = 0;

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (all_clients[i]) {
            clients_snapshot[count++] = all_clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    for (int i = 0; i < count; i++) {
        struct Irc_client *client = clients_snapshot[i];
        pthread_mutex_lock(&client->queue_mutex);
        struct message_queue *q = &client->message_queue;

        while (!queue_is_empty(q)) {
            char *msg = q->messages[q->read_pos];
            q->messages[q->read_pos] = NULL;
            q->read_pos = (q->read_pos + 1) % MAX_QUEUED_MESSAGES;

            pthread_mutex_unlock(&client->queue_mutex);

            // push callback function and argument onto vm->L stack
            lua_rawgeti(client->vm->L, LUA_REGISTRYINDEX, client->callback);
            lua_pushstring(client->vm->L, msg);

            bool consumed = config_vm_try_callback_arg(client->vm);

            if (!consumed) {
                ww_log(LOG_WARN, "IRC callback did not consume message");
            }

            free(msg);
            popped_count++;
            pthread_mutex_lock(&client->queue_mutex);
        }

        pthread_mutex_unlock(&client->queue_mutex);
    }
}
