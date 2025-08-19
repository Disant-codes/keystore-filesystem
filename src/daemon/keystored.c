#include "keystored.h"

static job_queue *g_job_queue = NULL;
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

// Accept new client connection
int accept_client(int listen_socket, client_connection_t **client) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_fd = accept(listen_socket, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // No pending connections
        }
        syslog(LOG_ERR, "keystored::failed to accept client connection");
        return -1;
    }
    
    // Allocate client connection structure
    *client = malloc(sizeof(client_connection_t));
    if (!*client) {
        syslog(LOG_ERR, "keystored::failed to allocate client connection");
        close(client_fd);
        return -1;
    }
    
    // Initialize client connection
    (*client)->fd = client_fd;
    (*client)->addr = client_addr;
    inet_ntop(AF_INET, &client_addr.sin_addr, (*client)->client_ip, INET_ADDRSTRLEN);
    (*client)->port = ntohs(client_addr.sin_port);
    
    syslog(LOG_INFO, "keystored::accepted client connection from %s:%d", 
           (*client)->client_ip, (*client)->port);
    
    return 1;
}

// Handle client job request
int handle_client_request(client_connection_t *client) {
    job_request req;
    ssize_t bytes_received = recv(client->fd, &req, sizeof(job_request), MSG_DONTWAIT);
    
    if (bytes_received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // No data available
        }
        syslog(LOG_ERR, "keystored::failed to receive from client %s:%d", 
               client->client_ip, client->port);
        return -1;
    }
    
    if (bytes_received == 0) {
        syslog(LOG_INFO, "keystored::client %s:%d disconnected", 
               client->client_ip, client->port);
        return -1;
    }
    
    if (bytes_received != sizeof(job_request)) {
        syslog(LOG_WARNING, "keystored::incomplete job request from client %s:%d (expected %zu, got %zd)", 
               client->client_ip, client->port, sizeof(job_request), bytes_received);
        return 0;
    }
    
    // Create job from request
    job *new_job = malloc(sizeof(job));
    if (!new_job) {
        syslog(LOG_ERR, "keystored::failed to allocate job");
        return -1;
    }
    
    // Initialize job
    new_job->request = malloc(sizeof(job_request));
    if (!new_job->request) {
        syslog(LOG_ERR, "keystored::failed to allocate job request");
        free(new_job);
        return -1;
    }
    
    // Copy request data
    memcpy(new_job->request, &req, sizeof(job_request));
    
    // Create response
    new_job->response = job_response_init(req.type);
    if (!new_job->response) {
        syslog(LOG_ERR, "keystored::failed to create job response");
        free(new_job->request);
        free(new_job);
        return -1;
    }
    
    new_job->next_job = NULL;
    new_job->client_fd = client->fd;
    // Submit job to queue
    job_push(g_job_queue, new_job);
    syslog(LOG_INFO, "keystored::submitted job (type: %d, key: %s) from client %s:%d to queue", 
           req.type, req.key, client->client_ip, client->port);
    
    return 0;
}

// Clean up client connection
void cleanup_client(client_connection_t *client) {
    if (client) {
        close(client->fd);
        free(client);
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
    const char *bind_ip = "127.0.0.1";
    int port = 5000;
    
    //setup logs
    openlog(DAEMON_NAME, LOG_PID|LOG_CONS, LOG_DAEMON);
    
    //Daemonize the process
    rc = daemonize();
    if (rc < 0) {
        syslog(LOG_ERR, "keystored::failed to daemonize");
        return 1;
    }

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
        close(listen_socket);
        return 1;
    }
    
    //Add listen socket to epoll
    add_epoll_fd(epoll_fd, listen_socket, NULL, EPOLLIN);
    
    syslog(LOG_INFO, "keystored::started on %s:%d", bind_ip, port);

    g_job_queue = job_queue_init();
    if (!g_job_queue) {
        syslog(LOG_ERR, "keystored::failed to initialize job queue");
        close(listen_socket);
        close(epoll_fd);
        return 1;
    }

    rc = job_worker_pool_init(g_job_queue, NUM_THREADS);
    if(rc){
        syslog(LOG_ERR, "keystored::failed to create thead pool");
        job_queue_free(g_job_queue);
        close(listen_socket);
        close(epoll_fd);
        return 1;
    }
    
    // Handle signals 
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    // Main event loop
    struct epoll_event events[MAX_EVENT]; 
    
    while (keep_running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENT, 250); 
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "keystored::epoll_wait failed");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == NULL) {
                // Listen socket event - accept new connections
                client_connection_t *new_client = NULL;
                int accept_result = accept_client(listen_socket, &new_client);
                
                if (accept_result > 0 && new_client) {
                    // Add new client to epoll for read events
                    add_epoll_fd(epoll_fd, new_client->fd, new_client, EPOLLIN | EPOLLET);
                } else if (accept_result < 0) {
                    // Error accepting client
                    if (new_client) {
                        cleanup_client(new_client);
                    }
                }
            } else {
                // Client socket event - handle job request
                client_connection_t *client = (client_connection_t*)events[i].data.ptr;
                int handle_result = handle_client_request(client);
                
                if (handle_result < 0) {
                    // Client error or disconnect - remove from epoll and cleanup
                    remove_epoll_fd(epoll_fd, client->fd);
                    cleanup_client(client);
                }
            }
        }
    }
    
    // Cleanup
    syslog(LOG_INFO, "keystored::cleaning up");
    
    // Close epoll
    close(epoll_fd);
    
    // Close listen socket
    close(listen_socket);
    
    // Free job queue
    if (g_job_queue) {
        job_queue_free(g_job_queue);
    }
    
    closelog();
    return 0;
}