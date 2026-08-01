#include "../../include/cli/ipc.h"
#include "../../include/recorder/error.h"
#include "../../include/recorder/replay_save.h"
#include "../../include/log.h"

#define SJ_IMPL
#include "../../external/sj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define GSR_IPC_MAX_REQUEST_NAME_SIZE 64
#define GSR_IPC_MAX_ESCAPED_ERROR_MESSAGE_SIZE (GSR_IPC_MAX_ERROR_MESSAGE_SIZE*6)
#define GSR_IPC_MAX_REPLY_SIZE (GSR_IPC_MAX_ESCAPED_ERROR_MESSAGE_SIZE + 128)
#define GSR_IPC_SEND_TIMEOUT_MILLISECONDS 1000
#define GSR_IPC_SOCKET_MODE 0600

typedef struct {
    int64_t id;
    char name[GSR_IPC_MAX_REQUEST_NAME_SIZE];
    sj_Value data;
    bool has_data;
} gsr_ipc_request;

static void json_escape_string(char *buffer, size_t buffer_size, const char *str) {
    char escape_buffer[8];
    size_t offset = 0;
    buffer[0] = '\0';

    for(size_t i = 0; str[i] != '\0'; ++i) {
        const unsigned char c = str[i];
        const char *escaped = escape_buffer;
        switch(c) {
            case '"':  escaped = "\\\""; break;
            case '\\': escaped = "\\\\"; break;
            case '\n': escaped = "\\n";  break;
            case '\r': escaped = "\\r";  break;
            case '\t': escaped = "\\t";  break;
            default: {
                if(c < 0x20)
                    snprintf(escape_buffer, sizeof(escape_buffer), "\\u%04x", c);
                else
                    snprintf(escape_buffer, sizeof(escape_buffer), "%c", c);
                break;
            }
        }

        const size_t escaped_size = strlen(escaped);
        if(offset + escaped_size >= buffer_size)
            break;

        memcpy(buffer + offset, escaped, escaped_size);
        offset += escaped_size;
        buffer[offset] = '\0';
    }
}

static bool json_string_equals(const sj_Value *value, const char *str) {
    const size_t value_size = value->end - value->start;
    return strlen(str) == value_size && memcmp(value->start, str, value_size) == 0;
}

static bool json_number_to_int64(const sj_Value *value, int64_t *result) {
    char buffer[32];
    const size_t value_size = value->end - value->start;
    if(value_size == 0 || value_size >= sizeof(buffer))
        return false;

    memcpy(buffer, value->start, value_size);
    buffer[value_size] = '\0';

    char *number_end = NULL;
    errno = 0;
    const long long parsed_value = strtoll(buffer, &number_end, 10);
    if(errno != 0 || number_end != buffer + value_size)
        return false;

    *result = parsed_value;
    return true;
}

static bool string_is_only_whitespace(const char *str, size_t size) {
    for(size_t i = 0; i < size; ++i) {
        if(str[i] != ' ' && str[i] != '\t' && str[i] != '\r' && str[i] != '\n')
            return false;
    }
    return true;
}

static bool ipc_send_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while(offset < size) {
        const ssize_t bytes_written = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if(bytes_written > 0) {
            offset += bytes_written;
            continue;
        }

        if(bytes_written == -1 && errno == EINTR)
            continue;

        if(bytes_written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd poll_fd;
            poll_fd.fd = fd;
            poll_fd.events = POLLOUT;
            poll_fd.revents = 0;

            const int poll_result = poll(&poll_fd, 1, GSR_IPC_SEND_TIMEOUT_MILLISECONDS);
            if(poll_result == -1 && errno == EINTR)
                continue;

            if(poll_result <= 0)
                return false;

            continue;
        }

        return false;
    }
    return true;
}

static bool ipc_client_send_reply(gsr_ipc_client *client, int64_t id, bool success, const char *error_message) {
    char reply[GSR_IPC_MAX_REPLY_SIZE];
    int reply_size = 0;

    if(success) {
        reply_size = snprintf(reply, sizeof(reply), "{\"id\":%" PRIi64 ",\"result\":\"ok\"}\n", id);
    } else {
        char escaped_error_message[GSR_IPC_MAX_ESCAPED_ERROR_MESSAGE_SIZE];
        json_escape_string(escaped_error_message, sizeof(escaped_error_message), error_message ? error_message : "");
        reply_size = snprintf(reply, sizeof(reply), "{\"id\":%" PRIi64 ",\"result\":\"error\",\"data\":\"%s\"}\n", id, escaped_error_message);
    }

    if(reply_size < 0 || reply_size >= (int)sizeof(reply)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc: failed to create a reply to request %" PRIi64, id);
        return false;
    }

    return ipc_send_all(client->fd, reply, reply_size);
}

static bool ipc_request_parse(char *data, size_t size, gsr_ipc_request *request, char *error_message, size_t error_message_size) {
    memset(request, 0, sizeof(*request));

    sj_Reader reader = sj_reader(data, size);
    const sj_Value root = sj_read(&reader);
    if(root.type != SJ_OBJECT) {
        snprintf(error_message, error_message_size, "expected the request to be a json object");
        return false;
    }

    sj_Value id_value;
    sj_Value name_value;
    bool has_id = false;
    bool has_name = false;

    sj_Value key;
    sj_Value value;
    while(sj_iter_object(&reader, root, &key, &value)) {
        if(json_string_equals(&key, "id")) {
            id_value = value;
            has_id = true;
        } else if(json_string_equals(&key, "name")) {
            name_value = value;
            has_name = true;
        } else if(json_string_equals(&key, "data")) {
            request->data = value;
            request->has_data = true;
        }
    }

    if(reader.error) {
        snprintf(error_message, error_message_size, "failed to parse the request: %s", reader.error);
        return false;
    }

    if(!has_id) {
        snprintf(error_message, error_message_size, "the request is missing the 'id' field");
        return false;
    }

    if(id_value.type != SJ_NUMBER || !json_number_to_int64(&id_value, &request->id)) {
        snprintf(error_message, error_message_size, "expected 'id' to be an integer");
        return false;
    }

    if(!has_name) {
        snprintf(error_message, error_message_size, "the request is missing the 'name' field");
        return false;
    }

    if(name_value.type != SJ_STRING) {
        snprintf(error_message, error_message_size, "expected 'name' to be a string");
        return false;
    }

    snprintf(request->name, sizeof(request->name), "%.*s", (int)(name_value.end - name_value.start), name_value.start);
    return true;
}

static bool ipc_request_get_save_replay_seconds(const gsr_ipc_request *request, int *seconds, char *error_message, size_t error_message_size) {
    *seconds = GSR_SAVE_REPLAY_SECONDS_FULL;
    if(!request->has_data || request->data.type == SJ_NULL)
        return true;

    int64_t data_seconds = 0;
    if(request->data.type != SJ_NUMBER || !json_number_to_int64(&request->data, &data_seconds) || data_seconds <= 0 || data_seconds > INT_MAX) {
        snprintf(error_message, error_message_size, "expected 'data' to be the number of seconds to save, which has to be larger than 0");
        return false;
    }

    *seconds = data_seconds;
    return true;
}

static bool ipc_handle_request(gsr_ipc *self, const gsr_ipc_request *request, char *error_message, size_t error_message_size) {
    if(strcmp(request->name, "stop") == 0)
        return self->handlers.stop(error_message, error_message_size, self->handlers.userdata);

    if(strcmp(request->name, "toggle-pause") == 0)
        return self->handlers.toggle_pause(error_message, error_message_size, self->handlers.userdata);

    if(strcmp(request->name, "toggle-replay-recording") == 0)
        return self->handlers.toggle_replay_recording(error_message, error_message_size, self->handlers.userdata);

    if(strcmp(request->name, "save-replay") == 0) {
        int seconds = GSR_SAVE_REPLAY_SECONDS_FULL;
        if(!ipc_request_get_save_replay_seconds(request, &seconds, error_message, error_message_size))
            return false;

        return self->handlers.save_replay(seconds, error_message, error_message_size, self->handlers.userdata);
    }

    snprintf(error_message, error_message_size, "unknown request name '%s'", request->name);
    return false;
}

static bool ipc_client_on_request(gsr_ipc *self, gsr_ipc_client *client) {
    if(client->request_too_large)
        return ipc_client_send_reply(client, 0, false, "the request is too large");

    if(string_is_only_whitespace(client->request, client->request_size))
        return ipc_client_send_reply(client, 0, false, "the request is empty");

    char error_message[GSR_IPC_MAX_ERROR_MESSAGE_SIZE];
    error_message[0] = '\0';

    gsr_ipc_request request;
    if(!ipc_request_parse(client->request, client->request_size, &request, error_message, sizeof(error_message)))
        return ipc_client_send_reply(client, request.id, false, error_message);

    if(!ipc_handle_request(self, &request, error_message, sizeof(error_message)))
        return ipc_client_send_reply(client, request.id, false, error_message);

    return ipc_client_send_reply(client, request.id, true, NULL);
}

static bool ipc_client_on_byte(gsr_ipc *self, gsr_ipc_client *client, char c) {
    if(c != '\n') {
        if(client->request_size < GSR_IPC_MAX_REQUEST_SIZE)
            client->request[client->request_size++] = c;
        else
            client->request_too_large = true;
        return true;
    }

    const bool keep_client = ipc_client_on_request(self, client);
    client->request_size = 0;
    client->request_too_large = false;
    return keep_client;
}

static bool ipc_client_receive(gsr_ipc *self, gsr_ipc_client *client) {
    for(;;) {
        char buffer[1024];
        const ssize_t bytes_read = recv(client->fd, buffer, sizeof(buffer), 0);
        if(bytes_read == 0)
            return false;

        if(bytes_read == -1) {
            if(errno == EINTR)
                continue;

            return errno == EAGAIN || errno == EWOULDBLOCK;
        }

        for(ssize_t i = 0; i < bytes_read; ++i) {
            if(!ipc_client_on_byte(self, client, buffer[i]))
                return false;
        }
    }
}

static bool fd_set_cloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags != -1 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

static bool fd_set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

static void ipc_add_client(gsr_ipc *self, int client_fd) {
    if(self->num_clients == GSR_IPC_MAX_CLIENTS) {
        gsr_log(GSR_LOG_LEVEL_WARNING, "gsr_ipc: too many ipc clients are connected, rejecting the new connection");
        close(client_fd);
        return;
    }

    if(!fd_set_cloexec(client_fd) || !fd_set_nonblocking(client_fd)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc: failed to setup the ipc client socket, error: %s", strerror(errno));
        close(client_fd);
        return;
    }

    gsr_ipc_client *client = &self->clients[self->num_clients];
    client->fd = client_fd;
    client->request_size = 0;
    client->request_too_large = false;
    ++self->num_clients;
}

static void ipc_accept_client(gsr_ipc *self) {
    const int client_fd = accept(self->socket_fd, NULL, NULL);
    if(client_fd == -1)
        return;

    ipc_add_client(self, client_fd);
}

static void ipc_remove_client(gsr_ipc *self, int index) {
    close(self->clients[index].fd);
    for(int i = index; i < self->num_clients - 1; ++i) {
        self->clients[i] = self->clients[i + 1];
    }
    --self->num_clients;
}

static void* ipc_thread(void *userdata) {
    gsr_ipc *self = userdata;
    struct pollfd poll_fds[2 + GSR_IPC_MAX_CLIENTS];

    for(;;) {
        poll_fds[0].fd = self->wakeup_pipe[0];
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;
        poll_fds[1].fd = self->socket_fd;
        poll_fds[1].events = POLLIN;
        poll_fds[1].revents = 0;

        const int num_polled_clients = self->num_clients;
        for(int i = 0; i < num_polled_clients; ++i) {
            poll_fds[2 + i].fd = self->clients[i].fd;
            poll_fds[2 + i].events = POLLIN;
            poll_fds[2 + i].revents = 0;
        }

        if(poll(poll_fds, 2 + num_polled_clients, -1) == -1) {
            if(errno == EINTR)
                continue;

            gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc: failed to poll the ipc sockets, error: %s", strerror(errno));
            break;
        }

        if(poll_fds[0].revents != 0)
            break;

        if(poll_fds[1].revents & POLLIN)
            ipc_accept_client(self);

        for(int i = num_polled_clients - 1; i >= 0; --i) {
            bool keep_client = true;
            if(poll_fds[2 + i].revents & POLLIN)
                keep_client = ipc_client_receive(self, &self->clients[i]);
            else if(poll_fds[2 + i].revents & (POLLHUP | POLLERR | POLLNVAL))
                keep_client = false;

            if(!keep_client)
                ipc_remove_client(self, i);
        }
    }

    return NULL;
}

static bool ipc_socket_filepath_in_use(const char *socket_filepath) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_filepath);

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd == -1)
        return true;

    const bool in_use = connect(fd, (const struct sockaddr*)&addr, sizeof(addr)) == 0;
    close(fd);
    return in_use;
}

static bool ipc_bind(gsr_ipc *self, const struct sockaddr_un *addr) {
    const mode_t prev_mask = umask(0777 & ~GSR_IPC_SOCKET_MODE);
    int bind_result = bind(self->socket_fd, (const struct sockaddr*)addr, sizeof(*addr));
    if(bind_result == -1 && errno == EADDRINUSE && !ipc_socket_filepath_in_use(self->socket_filepath)) {
        unlink(self->socket_filepath);
        bind_result = bind(self->socket_fd, (const struct sockaddr*)addr, sizeof(*addr));
    }
    const int bind_error = errno;
    umask(prev_mask);

    if(bind_result == -1) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_init: failed to bind the ipc socket to \"%s\", error: %s", self->socket_filepath, strerror(bind_error));
        return false;
    }

    self->socket_bound = true;
    return true;
}

static void ipc_close(gsr_ipc *self) {
    for(int i = 0; i < self->num_clients; ++i) {
        close(self->clients[i].fd);
    }
    self->num_clients = 0;

    for(int i = 0; i < 2; ++i) {
        if(self->wakeup_pipe[i] != -1) {
            close(self->wakeup_pipe[i]);
            self->wakeup_pipe[i] = -1;
        }
    }

    if(self->socket_fd != -1) {
        close(self->socket_fd);
        self->socket_fd = -1;
    }

    if(self->socket_bound) {
        unlink(self->socket_filepath);
        self->socket_bound = false;
    }
}

int gsr_ipc_init(gsr_ipc *self, const char *socket_filepath) {
    memset(self, 0, sizeof(*self));
    self->socket_fd = -1;
    self->wakeup_pipe[0] = -1;
    self->wakeup_pipe[1] = -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if(snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_filepath) >= (int)sizeof(addr.sun_path)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_init: the ipc socket path is too long, it can be at most %d characters: \"%s\"", (int)sizeof(addr.sun_path) - 1, socket_filepath);
        goto err;
    }

    snprintf(self->socket_filepath, sizeof(self->socket_filepath), "%s", socket_filepath);

    if(pipe(self->wakeup_pipe) == -1) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_init: failed to create the ipc wakeup pipe, error: %s", strerror(errno));
        self->wakeup_pipe[0] = -1;
        self->wakeup_pipe[1] = -1;
        goto err;
    }

    if(!fd_set_cloexec(self->wakeup_pipe[0]) || !fd_set_cloexec(self->wakeup_pipe[1])) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_init: failed to setup the ipc wakeup pipe, error: %s", strerror(errno));
        goto err;
    }

    self->socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(self->socket_fd == -1) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_init: failed to create the ipc socket, error: %s", strerror(errno));
        goto err;
    }

    if(!ipc_bind(self, &addr))
        goto err;

    if(listen(self->socket_fd, GSR_IPC_MAX_CLIENTS) == -1) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_init: failed to listen on the ipc socket, error: %s", strerror(errno));
        goto err;
    }

    self->initialized = true;
    return GSR_ERROR_OK;

    err:
    ipc_close(self);
    return GSR_ERROR_GENERIC;
}

void gsr_ipc_deinit(gsr_ipc *self) {
    if(!self->initialized)
        return;

    gsr_ipc_stop(self);
    ipc_close(self);
    self->initialized = false;
}

int gsr_ipc_start(gsr_ipc *self, const gsr_ipc_handlers *handlers) {
    if(!self->initialized)
        return GSR_ERROR_OK;

    self->handlers = *handlers;

    /* Block all signals in the ipc thread to keep the signal handlers running on the main thread */
    sigset_t all_signals;
    sigset_t prev_signals;
    sigfillset(&all_signals);
    pthread_sigmask(SIG_SETMASK, &all_signals, &prev_signals);
    const int thread_create_result = pthread_create(&self->thread, NULL, ipc_thread, self);
    pthread_sigmask(SIG_SETMASK, &prev_signals, NULL);

    if(thread_create_result != 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_start: failed to create the ipc thread, error: %s", strerror(thread_create_result));
        return GSR_ERROR_GENERIC;
    }

    self->thread_running = true;
    return GSR_ERROR_OK;
}

void gsr_ipc_stop(gsr_ipc *self) {
    if(!self->thread_running)
        return;

    const char wakeup_value = 1;
    ssize_t bytes_written = 0;
    do {
        bytes_written = write(self->wakeup_pipe[1], &wakeup_value, 1);
    } while(bytes_written == -1 && errno == EINTR);

    if(bytes_written == -1)
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_ipc_stop: failed to wake up the ipc thread, error: %s", strerror(errno));

    pthread_join(self->thread, NULL);
    self->thread_running = false;
}
