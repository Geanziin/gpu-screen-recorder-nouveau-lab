#ifndef GSR_CLI_IPC_H
#define GSR_CLI_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <pthread.h>

#define GSR_IPC_MAX_CLIENTS 8
#define GSR_IPC_MAX_REQUEST_SIZE 4096
#define GSR_IPC_MAX_ERROR_MESSAGE_SIZE 256

/*
    These are called from the ipc thread while the recording is running.
    Return false and write a message to |error_message| to reply to the request with an error.
*/
typedef struct {
    bool (*stop)(char *error_message, size_t error_message_size, void *userdata);
    bool (*toggle_pause)(char *error_message, size_t error_message_size, void *userdata);
    bool (*toggle_replay_recording)(char *error_message, size_t error_message_size, void *userdata);
    /* |seconds| is GSR_SAVE_REPLAY_SECONDS_FULL when the whole replay buffer should be saved */
    bool (*save_replay)(int seconds, char *error_message, size_t error_message_size, void *userdata);
    void *userdata;
} gsr_ipc_handlers;

typedef struct {
    int fd;
    char request[GSR_IPC_MAX_REQUEST_SIZE];
    size_t request_size;
    bool request_too_large;
} gsr_ipc_client;

/* Receives newline terminated json requests on a unix domain socket and replies to every request */
typedef struct {
    bool initialized;
    int socket_fd;
    bool socket_bound;
    char socket_filepath[PATH_MAX];
    int wakeup_pipe[2];
    gsr_ipc_client clients[GSR_IPC_MAX_CLIENTS];
    int num_clients;
    gsr_ipc_handlers handlers;
    pthread_t thread;
    bool thread_running;
} gsr_ipc;

/* Returns a |gsr_error| value. Creates the socket, requests are not handled until gsr_ipc_start is called */
int gsr_ipc_init(gsr_ipc *self, const char *socket_filepath);
void gsr_ipc_deinit(gsr_ipc *self);

/* Returns a |gsr_error| value. Starts handling requests. Does nothing if |self| hasn't been initialized */
int gsr_ipc_start(gsr_ipc *self, const gsr_ipc_handlers *handlers);
/* Stops handling requests. Does nothing if the ipc hasn't been started */
void gsr_ipc_stop(gsr_ipc *self);

#endif /* GSR_CLI_IPC_H */
