#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>

#define DAEMON_NAME "keyvalued"


void handle_signal(int sig);
int daemonize(void);