#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <errno.h>

#include "job_executor.h"

#define DAEMON_NAME "keyvalued"

// Client connection structure
typedef struct client_connection {
    int fd;
    struct sockaddr_in addr;
    char client_ip[INET_ADDRSTRLEN];
    int port;
} client_connection_t;

void handle_signal(int sig);
int daemonize(void);
int create_socket(const char *bind_ip, int port);
int create_epoll(void);
void add_epoll_fd(int epfd, int fd, void *ptr, uint32_t events);
void remove_epoll_fd(int epfd, int fd);
int accept_client(int listen_socket, client_connection_t **client);
int handle_client_request(client_connection_t *client);
void cleanup_client(client_connection_t *client);