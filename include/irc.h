#ifndef IRC_H
#define IRC_H

#include "lua.h"
#include <libircclient/libircclient.h>
#include <pthread.h>
#include <stdbool.h>

#define MAX_CLIENTS 8
#define MAX_QUEUED_MESSAGES 64
#define MAX_MESSAGE_LENGTH 1024

struct message_queue {
    char *messages[MAX_QUEUED_MESSAGES];
    int write_pos;
    int read_pos;
};

struct Irc_client {
    irc_session_t *session;
    int callback;
    int index;
    pthread_t thread_id;
    bool thread_running;
    struct message_queue message_queue;
    pthread_mutex_t queue_mutex;
    struct config_vm *vm;
};

struct Irc_client *irc_client_create(const char *ip, long port, const char *nick, const char *pass,
                                     int callback, lua_State *L);
void manage_new_messages();
void irc_client_send(struct Irc_client *client, const char *message);
void irc_client_destroy(struct Irc_client *client);

#endif