#include "keystored.h"

static job_queue *g_job_queue=NULL;
int keep_running = 1;

void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        syslog(LOG_INFO, "keystored::shutting down");
        keep_running = 0;
    }
}

int create_socket(const char *bind_ip, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sockfd < 0) {
        syslog(LOG_ERR, "keystored::failed to create socket");
        return -1;
    }
    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) <= 0) { 
        syslog(LOG_ERR, "keystored::failed to convert IP address");
        return -2;
    }
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "keystored::failed to bind socket");
        return -3;
    }
    if (listen(sockfd, SOMAXCONN) < 0) {
        syslog(LOG_ERR, "keystored::failed to listen on socket");
        return -4;
    }
    return sockfd;
}

int create_epoll(void) {
    int epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (epollfd < 0) {
        syslog(LOG_ERR, "keystored::failed to create epoll");
        return -1;
    }
    return epollfd;
}

void add_epoll_fd(int epfd, int fd, void *ptr, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.ptr = ptr;
    ev.events = events;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        syslog(LOG_ERR, "keystored::failed to add fd to epoll");
    }
}

void remove_epoll_fd(int epfd, int fd) {
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        syslog(LOG_ERR, "keystored::failed to remove fd from epoll");
    }
}

int daemonize(void) {
    pid_t process_id, session_id;
    int rc = 0;
    syslog(LOG_INFO, "keystored::daemonizing");
    //Create process
    process_id = fork();
    if (process_id < 0)
        return -1;

    //Exit Parent process
    if (process_id > 0)
        return 0;
    
    //Set new session
    session_id = setsid();
    if (session_id < 0)
        return -2;
    
    //Change directory
    rc = chdir("/");
    if (rc < 0)
        return -3;
    
    //Close file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    return 0;
}

int main(int argc, char *argv[]){
	int rc;
	if (argc < 3) {
        fprintf(stderr, "Usage: %s <bind_ip> <port>\n", argv[0]);
        return 2;
    }
    const char *bind_ip = argv[1];
    int port = atoi(argv[2]);
	
    //setup logs
	openlog(DAEMON_NAME, LOG_PID|LOG_CONS, LOG_DAEMON);
    
    //Create socket
    int listen_socket = create_socket(bind_ip, port);
    if (listen_socket < 0) {
        syslog(LOG_ERR, "keystored::failed to create socket");
        return 1;
    }
    //Create epoll
    int epoll_fd = create_epoll();
    if (epoll_fd < 0) {
        syslog(LOG_ERR, "keystored::failed to create epoll");
        return 1;
    }
    //Add listen socket to epoll
    add_epoll_fd(epoll_fd, listen_socket, NULL, EPOLLIN);
    //Daemonize the process
    rc = daemonize();
    if (rc < 0) {
        syslog(LOG_ERR, "keystored::failed to daemonize");
        return 1;
    }
    syslog(LOG_INFO, "keystored::started");

	// Handle termination signals 
	signal(SIGTERM, handle_signal);
	signal(SIGINT, handle_signal);


	while (keep_running) {
		sleep(1);
	}

	closelog();
	return 0;
}