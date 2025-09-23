#ifndef HTTP_H
#define HTTP_H
#include "lua.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>

#define MAX_CLIENTS 8
#define MAX_QUEUED_RESPONSES 256
#define MAX_RESPONSE_LENGTH 8192

struct queued_response {
    char *data;
    size_t size;
    char *url;
};

struct response_queue {
    struct queued_response *responses[MAX_QUEUED_RESPONSES];
    int write_pos;
    int read_pos;
};

struct Http_client {
    CURL *curl;
    int callback;
    int index;
    pthread_t thread_id;
    bool thread_running;
    struct response_queue response_queue;
    pthread_mutex_t queue_mutex;
    pthread_mutex_t request_mutex;
    struct config_vm *vm;

    char *pending_requests[MAX_QUEUED_RESPONSES];
    int request_write_pos;
    int request_read_pos;
    pthread_cond_t request_cond;
    bool should_exit;
};

struct Http_client *http_client_create(int callback, lua_State *L);
void manage_new_responses();
void http_client_get(struct Http_client *client, const char *url);
void http_client_destroy(struct Http_client *client);

#endif
