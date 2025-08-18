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

#include "job_executor.h"

#define DAEMON_NAME "keyvalued"


void handle_signal(int sig);
int daemonize(void);
int create_socket(const char *bind_ip, int port);
int create_epoll(void);
void add_epoll_fd(int epfd, int fd, void *ptr, uint32_t events);
void remove_epoll_fd(int epfd, int fd);