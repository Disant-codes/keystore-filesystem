#include "keystored.h"

static job_queue *g_job_queue = NULL;
int keep_running = 1;
storage_state_t g_storage;

// Safe pointer to the start of the superblock memory (block 0)
static inline uint8_t * sb_block_ptr(storage_state_t *st) { 
    return st ? (uint8_t*)st->mapped_ptr : NULL; 
}

// Forward declarations
static inline uint8_t * block_ptr(storage_state_t *state, uint32_t block_index);
static int hash_buckets_block_init(storage_state_t *st, uint32_t bucket_count);

// Forward declaration of functions
// Initialize bucket block on first create
static int hash_buckets_block_init(storage_state_t *st, uint32_t bucket_count) {
    if (!st) return -1;
    // allocate a block to hold buckets
    uint32_t blk = 0;
    if (storage_block_alloc(st, &blk) != 0) return -1;
    uint32_t *arr = (uint32_t*)block_ptr(st, blk);
    if (!arr) { storage_block_free(st, blk); return -1; }
    size_t need = (size_t)bucket_count * sizeof(uint32_t);
    if (need > st->super.block_size) { storage_block_free(st, blk); return -1; }
    memset(arr, 0, need);
    msync(arr, need, MS_SYNC);
    // record in superblock
    keystore_super_block_t *sb = (keystore_super_block_t*)sb_block_ptr(st);
    sb->hash_bucket_count = bucket_count;
    sb->hash_buckets_block = blk;
    msync(sb, sizeof(*sb), MS_SYNC);
    st->super.hash_bucket_count = bucket_count;
    st->super.hash_buckets_block = blk;
    return 0;
}

// ---------------- Free-list management ----------------
//   - Block 0 is the superblock (never on free list)
//   - For each FREE block i (i >= 1), the first 4 bytes store `next_free_block_index` (uint32_t)
//   - The superblock stores the head of the free list and the free block count

// Returns a pointer to the beginning of the block within the mmap, or NULL on error.
static uint8_t * block_ptr(storage_state_t *state, uint32_t block_index){
    if (!state || !state->mapped_ptr) return NULL;
    if (block_index >= state->super.num_blocks) return NULL;
    size_t offset = (size_t)block_index * (size_t)state->super.block_size;
    if (offset + sizeof(uint32_t) > state->mapped_size) return NULL;
    return (uint8_t*)state->mapped_ptr + offset;
}

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

int storage_open_or_create(const char *path,
                           uint32_t default_block_size,
                           uint32_t default_num_blocks,
                           storage_state_t *out_state) {
    if (!out_state) return -1;
    memset(out_state, 0, sizeof(*out_state));

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            syslog(LOG_ERR, "keystored::storage create failed: %m");
            return -1;
        }
        // Compute total size and resize
        uint64_t total_size = (uint64_t)default_block_size * (uint64_t)default_num_blocks;
        if (ftruncate(fd, (off_t)total_size) != 0) {
            syslog(LOG_ERR, "keystored::ftruncate failed: %m");
            close(fd);
            return -1;
        }

        // Map and write superblock
        void *map = mmap(NULL, (size_t)total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            syslog(LOG_ERR, "keystored::mmap failed: %m");
            close(fd);
            return -1;
        }
        keystore_super_block_t *sb = (keystore_super_block_t *)map;
        memset(sb, 0, sizeof(*sb));
        sb->magic = KEYSTORE_MAGIC;
        sb->version = KEYSTORE_VERSION;
        sb->total_size = total_size;
        sb->block_size = default_block_size;
        sb->num_blocks = default_num_blocks;
        sb->free_list_head_block = 0;
        sb->free_block_count = 0;
        msync(map, sizeof(*sb), MS_SYNC);

        out_state->fd = fd;
        out_state->mapped_ptr = map;
        out_state->mapped_size = (size_t)total_size;
        out_state->super = *sb;
        pthread_mutex_init(&out_state->freelist_mutex, NULL);
        // Format the free list now that the file exists
        freelist_format(out_state);
        return 0;
    } else if (fd < 0) {
        syslog(LOG_ERR, "keystored::storage open failed: %m");
        return -1;
    }

    // Existing file: map and validate superblock
    struct stat st;
    if (fstat(fd, &st) != 0) {
        syslog(LOG_ERR, "keystored::fstat failed: %m");
        close(fd);
        return -1;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        syslog(LOG_ERR, "keystored::mmap failed: %m");
        close(fd);
        return -1;
    }
    keystore_super_block_t *sb = (keystore_super_block_t *)map;
    if (sb->magic != KEYSTORE_MAGIC || sb->version != KEYSTORE_VERSION) {
        syslog(LOG_ERR, "keystored::invalid superblock (magic=%u version=%u)", sb->magic, sb->version);
        munmap(map, (size_t)st.st_size);
        close(fd);
        return -1;
    }
    out_state->fd = fd;
    out_state->mapped_ptr = map;
    out_state->mapped_size = (size_t)st.st_size;
    out_state->super = *sb;
    pthread_mutex_init(&out_state->freelist_mutex, NULL);
    return 0;
}

void storage_close(storage_state_t *state){
    if (!state) return;
    if (state->mapped_ptr && state->mapped_size) {
        msync(state->mapped_ptr, state->mapped_size, MS_SYNC);
        munmap(state->mapped_ptr, state->mapped_size);
    }
    if (state->fd > 0) close(state->fd);
    pthread_mutex_destroy(&state->freelist_mutex);
    memset(state, 0, sizeof(*state));
}

void storage_print_superblock_ascii(const storage_state_t *state){
    if (!state) return;
    const keystore_super_block_t *sb = &state->super;
    printf("+----------------------+------------------------------+\n");
    printf("| %-20s | %-28s |\n", "Field", "Value");
    printf("+----------------------+------------------------------+\n");
    printf("| %-20s | 0x%08X                   |\n", "magic", sb->magic);
    printf("| %-20s | %10u                    |\n", "version", sb->version);
    printf("| %-20s | %10llu bytes          |\n", "total_size", (unsigned long long)sb->total_size);
    printf("| %-20s | %10u bytes/block     |\n", "block_size", sb->block_size);
    printf("| %-20s | %10u blocks          |\n", "num_blocks", sb->num_blocks);
    printf("| %-20s | %10u (block index)  |\n", "free_head", sb->free_list_head_block);
    printf("| %-20s | %10u blocks          |\n", "free_count", sb->free_block_count);
    printf("+----------------------+------------------------------+\n");
}



// Small helpers to read/write the `next` pointer inside a block
static inline int freelist_read_next(storage_state_t *state, uint32_t block_index, uint32_t *out_next){
    void *ptr = block_ptr(state, block_index);
    if (!ptr || !out_next) return -1;
    memcpy(out_next, ptr, sizeof(uint32_t));
    return 0;
}

static inline int freelist_write_next(storage_state_t *state, uint32_t block_index, uint32_t next_index){
    void *ptr = block_ptr(state, block_index);
    if (!ptr) return -1;
    memcpy(ptr, &next_index, sizeof(uint32_t));
    return 0;
}

// Formats the free list over data blocks [1 .. num_blocks-1].
// This is called when creating a brand new storage image.
int freelist_format(storage_state_t *state){
    if (!state || !state->mapped_ptr) return -1;
    const uint32_t first_data = 1;
    const uint32_t last_data = (state->super.num_blocks == 0) ? 0 : (state->super.num_blocks - 1);

    // Make a simple chain: i -> i+1, last -> 0 (end)
    for (uint32_t i = first_data; i <= last_data; i++) {
        uint32_t next = (i < last_data) ? (i + 1) : 0;
        if (freelist_write_next(state, i, next) != 0) return -1;
    }

    keystore_super_block_t *live_sb = (keystore_super_block_t*)state->mapped_ptr;
    if (state->super.num_blocks > 1) {
        live_sb->free_list_head_block = first_data;
        live_sb->free_block_count = state->super.num_blocks - 1;
    } else {
        live_sb->free_list_head_block = 0;
        live_sb->free_block_count = 0;
    }
    msync(live_sb, sizeof(*live_sb), MS_SYNC);
    state->super.free_list_head_block = live_sb->free_list_head_block;
    state->super.free_block_count = live_sb->free_block_count;
    return 0;
}

// Pops a block from the free list. Returns 0 on success and writes the block index.
int storage_block_alloc(storage_state_t *state, uint32_t *out_block_index){
    if (!state || !out_block_index) return -1;
    pthread_mutex_lock(&state->freelist_mutex);

    keystore_super_block_t *live_sb = (keystore_super_block_t*)state->mapped_ptr;
    const uint32_t head = live_sb->free_list_head_block;
    if (head == 0 || live_sb->free_block_count == 0) {
        pthread_mutex_unlock(&state->freelist_mutex);
        return -1; // No free blocks
    }

    uint32_t next = 0;
    if (freelist_read_next(state, head, &next) != 0) {
        pthread_mutex_unlock(&state->freelist_mutex);
        return -1;
    }

    live_sb->free_list_head_block = next;
    live_sb->free_block_count -= 1;
    msync(live_sb, sizeof(*live_sb), MS_SYNC);
    state->super.free_list_head_block = live_sb->free_list_head_block;
    state->super.free_block_count = live_sb->free_block_count;
    pthread_mutex_unlock(&state->freelist_mutex);
    *out_block_index = head;
    return 0;
}

// Pushes a block back onto the free list (LIFO). Returns 0 on success.
int storage_block_free(storage_state_t *state, uint32_t block_index){
    if (!state) return -1;
    if (block_index == 0 || block_index >= state->super.num_blocks) return -1; 

    pthread_mutex_lock(&state->freelist_mutex);
    keystore_super_block_t *live_sb = (keystore_super_block_t*)state->mapped_ptr;

    // The freed block points to the current head
    if (freelist_write_next(state, block_index, live_sb->free_list_head_block) != 0) {
        pthread_mutex_unlock(&state->freelist_mutex);
        return -1;
    }
    // Update head and count
    live_sb->free_list_head_block = block_index;
    live_sb->free_block_count += 1;
    msync(live_sb, sizeof(*live_sb), MS_SYNC);
    state->super.free_list_head_block = live_sb->free_list_head_block;
    state->super.free_block_count = live_sb->free_block_count;
    pthread_mutex_unlock(&state->freelist_mutex);
    return 0;
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

int main(){
    int rc;
    const char *bind_ip = "127.0.0.1";
    int port = 5000;
    
    //setup logs
    openlog(DAEMON_NAME, LOG_PID|LOG_CONS, LOG_DAEMON);

    // Initialize storage BEFORE daemonizing so errors are visible in foreground
    rc = storage_open_or_create(KEYSTORE_IMG_PATH, DEFAULT_BLOCK_SIZE, DEFAULT_NUM_BLOCKS, &g_storage);
    if (rc != 0) {
        syslog(LOG_ERR, "keystored::storage initialization failed");
        return 1;
    }
    // Initialize hash bucket block on first create; if already set, skip
    if (g_storage.super.hash_buckets_block == 0){
        if (hash_buckets_block_init(&g_storage, DEFAULT_HASH_BUCKETS) != 0){
            syslog(LOG_ERR, "keystored::failed to init hash bucket block");
            return 1;
        }
    }
    
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
    
    // Close storage mapping/file
    storage_close(&g_storage);
    closelog();
    return 0;
}