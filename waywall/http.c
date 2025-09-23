#include "http.h"
#include "util/log.h"
#include <config/vm.h>
#include <curl/curl.h>
#include <lua.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Http_client *all_clients[MAX_CLIENTS] = {0};
static int client_count = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static int pushed_count = 0;
static int popped_count = 0;

struct response_buffer {
    char *data;
    size_t size;
};

static size_t
write_callback(void *contents, size_t size, size_t nmemb, struct response_buffer *buffer) {
    size_t realsize = size * nmemb;
    char *ptr = realloc(buffer->data, buffer->size + realsize + 1);
    if (!ptr) {
        ww_log(LOG_ERROR, "realloc failed in write_callback");
        return 0;
    }

    buffer->data = ptr;
    memcpy(&(buffer->data[buffer->size]), contents, realsize);
    buffer->size += realsize;
    buffer->data[buffer->size] = '\0';

    return realsize;
}

static bool
response_queue_is_full(struct response_queue *q) {
    return ((q->write_pos + 1) % MAX_QUEUED_RESPONSES) == q->read_pos;
}

static bool
response_queue_is_empty(struct response_queue *q) {
    return q->write_pos == q->read_pos;
}

static bool
request_queue_is_full(struct Http_client *client) {
    return ((client->request_write_pos + 1) % MAX_QUEUED_RESPONSES) == client->request_read_pos;
}

static bool
request_queue_is_empty(struct Http_client *client) {
    return client->request_write_pos == client->request_read_pos;
}

static void
response_queue_init(struct response_queue *q) {
    if (!q)
        return;
    for (int i = 0; i < MAX_QUEUED_RESPONSES; i++) {
        free(q->responses[i]);
        q->responses[i] = NULL;
    }
    q->write_pos = 0;
    q->read_pos = 0;
}

static void
request_queue_init(struct Http_client *client) {
    if (!client)
        return;
    for (int i = 0; i < MAX_QUEUED_RESPONSES; i++) {
        free(client->pending_requests[i]);
        client->pending_requests[i] = NULL;
    }
    client->request_write_pos = 0;
    client->request_read_pos = 0;
}

static void
response_queue_push(struct Http_client *client, const char *response, size_t response_len,
                    char *url) {
    pthread_mutex_lock(&client->queue_mutex);
    struct response_queue *q = &client->response_queue;

    if (response_queue_is_full(q)) {
        ww_log(LOG_WARN, "Response queue full for client %d. Dropping response.", client->index);
        pthread_mutex_unlock(&client->queue_mutex);
        return;
    }

    struct queued_response *qr = malloc(sizeof(*qr));
    if (!qr) {
        ww_log(LOG_ERROR, "malloc failed for queued_response");
        pthread_mutex_unlock(&client->queue_mutex);
        return;
    }

    qr->data = malloc(response_len);
    if (!qr->data) {
        ww_log(LOG_ERROR, "malloc failed for response data");
        free(qr);
        pthread_mutex_unlock(&client->queue_mutex);
        return;
    }

    memcpy(qr->data, response, response_len);
    qr->size = response_len;

    qr->url = strdup(url);

    q->responses[q->write_pos] = qr;
    q->write_pos = (q->write_pos + 1) % MAX_QUEUED_RESPONSES;

    pushed_count++;

    pthread_mutex_unlock(&client->queue_mutex);
}

static void
request_queue_push(struct Http_client *client, const char *url) {
    pthread_mutex_lock(&client->request_mutex);

    if (request_queue_is_full(client)) {
        ww_log(LOG_WARN, "Request queue full for client %d. Dropping request.", client->index);
        pthread_mutex_unlock(&client->request_mutex);
        return;
    }

    char *copy = strdup(url);
    if (!copy) {
        ww_log(LOG_ERROR, "strdup failed");
        pthread_mutex_unlock(&client->request_mutex);
        return;
    }

    client->pending_requests[client->request_write_pos] = copy;
    client->request_write_pos = (client->request_write_pos + 1) % MAX_QUEUED_RESPONSES;

    // Signal the worker thread that a new request is available
    pthread_cond_signal(&client->request_cond);
    pthread_mutex_unlock(&client->request_mutex);
}

static char *
request_queue_pop(struct Http_client *client) {
    pthread_mutex_lock(&client->request_mutex);

    while (request_queue_is_empty(client) && !client->should_exit) {
        pthread_cond_wait(&client->request_cond, &client->request_mutex);
    }

    if (client->should_exit) {
        pthread_mutex_unlock(&client->request_mutex);
        return NULL;
    }

    char *url = client->pending_requests[client->request_read_pos];
    client->pending_requests[client->request_read_pos] = NULL;
    client->request_read_pos = (client->request_read_pos + 1) % MAX_QUEUED_RESPONSES;

    pthread_mutex_unlock(&client->request_mutex);
    return url;
}

static void
response_queue_cleanup(struct response_queue *q) {
    if (!q)
        return;
    for (int i = 0; i < MAX_QUEUED_RESPONSES; i++) {
        if (q->responses[i]) {
            free(q->responses[i]->data);
            free(q->responses[i]->url);
            free(q->responses[i]);
            q->responses[i] = NULL;
        }
    }
    q->write_pos = 0;
    q->read_pos = 0;
}

static void
request_queue_cleanup(struct Http_client *client) {
    if (!client)
        return;
    for (int i = 0; i < MAX_QUEUED_RESPONSES; i++) {
        free(client->pending_requests[i]);
        client->pending_requests[i] = NULL;
    }
    client->request_write_pos = 0;
    client->request_read_pos = 0;
}

static void *
http_thread(void *arg) {
    struct Http_client *client = arg;
    if (!client || !client->curl) {
        ww_log(LOG_ERROR, "Invalid client in HTTP thread");
        return NULL;
    }

    while (!client->should_exit) {
        char *url = request_queue_pop(client);
        if (!url)
            break;

        struct response_buffer response = {0};

        // configure curl
        curl_easy_setopt(client->curl, CURLOPT_URL, url);
        curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(client->curl, CURLOPT_TIMEOUT, 30L);

        // make request
        CURLcode res = curl_easy_perform(client->curl);

        if (res != CURLE_OK) {
            const char *error_msg = curl_easy_strerror(res);
            response_queue_push(client, error_msg, strlen(error_msg), url);
            ww_log(LOG_WARN, "HTTP request failed: %s", error_msg);
        } else if (response.data && response.size > 0) {
            response_queue_push(client, response.data, response.size, url);
        } else {
            const char *empty = "";
            response_queue_push(client, empty, 0, url);
        }

        if (response.data) {
            free(response.data);
            response.data = NULL;
        }
        free(url);
    }

    return NULL;
}

struct Http_client *
http_client_create(int callback, lua_State *L) {
    if (!L) {
        ww_log(LOG_ERROR, "Invalid parameters for HTTP client creation");
        return NULL;
    }

    // Initialize curl globally if not already done
    static bool curl_global_init_done = false;
    if (!curl_global_init_done) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_global_init_done = true;
    }

    pthread_mutex_lock(&clients_mutex);

    if (client_count >= MAX_CLIENTS) {
        ww_log(LOG_ERROR, "Too many HTTP clients (max %d)", MAX_CLIENTS);
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!all_clients[i]) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        ww_log(LOG_ERROR, "No free HTTP client slots");
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        ww_log(LOG_ERROR, "Failed to initialize curl");
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    struct Http_client *client = calloc(1, sizeof(struct Http_client));
    if (!client) {
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    client->curl = curl;
    client->callback = callback;
    client->index = slot;
    client->thread_running = false;
    client->should_exit = false;
    response_queue_init(&client->response_queue);
    request_queue_init(client);
    pthread_mutex_init(&client->queue_mutex, NULL);
    pthread_mutex_init(&client->request_mutex, NULL);
    pthread_cond_init(&client->request_cond, NULL);
    client->vm = config_vm_from(L);

    all_clients[slot] = client;
    client_count++;

    if (pthread_create(&client->thread_id, NULL, http_thread, client) != 0) {
        ww_log(LOG_ERROR, "Failed to create HTTP thread");
        all_clients[slot] = NULL;
        client_count--;
        response_queue_cleanup(&client->response_queue);
        request_queue_cleanup(client);
        curl_easy_cleanup(curl);
        pthread_mutex_destroy(&client->queue_mutex);
        pthread_mutex_destroy(&client->request_mutex);
        pthread_cond_destroy(&client->request_cond);
        free(client);
        pthread_mutex_unlock(&clients_mutex);
        return NULL;
    }

    client->thread_running = true;
    pthread_mutex_unlock(&clients_mutex);
    return client;
}

void
http_client_get(struct Http_client *client, const char *url) {
    if (!client || !client->curl || !url) {
        ww_log(LOG_WARN, "Invalid parameters for HTTP GET");
        return;
    }

    if (!client->thread_running) {
        ww_log(LOG_WARN, "Cannot send request to stopped HTTP client");
        return;
    }

    request_queue_push(client, url);
}

void
http_client_destroy(struct Http_client *client) {
    if (!client)
        return;

    if (client->thread_running) {
        // Signal the thread to exit
        pthread_mutex_lock(&client->request_mutex);
        client->should_exit = true;
        pthread_cond_signal(&client->request_cond);
        pthread_mutex_unlock(&client->request_mutex);

        // Wait for thread to finish
        pthread_join(client->thread_id, NULL);
        client->thread_running = false;
    }

    if (client->curl) {
        curl_easy_cleanup(client->curl);
        client->curl = NULL;
    }

    response_queue_cleanup(&client->response_queue);
    request_queue_cleanup(client);

    pthread_mutex_destroy(&client->queue_mutex);
    pthread_mutex_destroy(&client->request_mutex);
    pthread_cond_destroy(&client->request_cond);

    luaL_unref(client->vm->L, LUA_REGISTRYINDEX, client->callback);

    pthread_mutex_lock(&clients_mutex);
    if (client->index >= 0 && client->index < MAX_CLIENTS) {
        all_clients[client->index] = NULL;
        client_count--;
    }
    pthread_mutex_unlock(&clients_mutex);

    free(client);
}

void
manage_new_responses() {
    struct Http_client *clients_snapshot[MAX_CLIENTS];
    int count = 0;

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (all_clients[i]) {
            clients_snapshot[count++] = all_clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    for (int i = 0; i < count; i++) {
        struct Http_client *client = clients_snapshot[i];
        pthread_mutex_lock(&client->queue_mutex);
        struct response_queue *q = &client->response_queue;

        while (!response_queue_is_empty(q)) {
            struct queued_response *qr = q->responses[q->read_pos];
            q->responses[q->read_pos] = NULL;
            q->read_pos = (q->read_pos + 1) % MAX_QUEUED_RESPONSES; // Move read position

            lua_rawgeti(client->vm->L, LUA_REGISTRYINDEX, client->callback);
            lua_pushlstring(client->vm->L, qr->data, qr->size);
            lua_pushstring(client->vm->L, qr->url);

            bool consumed = config_vm_try_callback_args2(client->vm);

            if (!consumed) {
                ww_log(LOG_WARN, "HTTP callback did not consume response");
            }

            free(qr->data);
            free(qr->url);
            free(qr);
            popped_count++;
        }

        pthread_mutex_unlock(&client->queue_mutex);
    }
}
