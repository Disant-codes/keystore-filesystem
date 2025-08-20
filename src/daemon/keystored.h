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
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>

#include "job_executor.h"

#define DAEMON_NAME "keyvalued"
#define NUM_THREADS     16
#define MAX_EVENT       16
// Persistent block storage configuration
#define KEYSTORE_IMG_PATH "/tmp/keystored.img"
#define KEYSTORE_MAGIC 0x4B455953 /* 'KEYS' */
#define KEYSTORE_VERSION 1
#define DEFAULT_BLOCK_SIZE 4096U
#define DEFAULT_NUM_BLOCKS 16384U /* 64 MiB total */
#define DEFAULT_HASH_BUCKETS 512u

typedef struct keystore_super_block {
    uint32_t magic;         /* KEYSTORE_MAGIC */
    uint32_t version;       /* structure version */
    uint64_t total_size;    /* total file size in bytes */
    uint32_t block_size;    /* bytes per block */
    uint32_t num_blocks;    /* number of blocks including superblock */
    uint32_t free_list_head_block; /* head block index of free list (0 == none) */
    uint32_t free_block_count;     /* number of free blocks available */
    uint32_t hash_bucket_count;    /* number of hash buckets */
    uint32_t hash_buckets_block;   /* block index holding the hash bucket array */
    uint8_t  reserved[32];  /* future use */
} keystore_super_block_t;

typedef struct storage_state {
    int fd;
    void *mapped_ptr;
    size_t mapped_size;
    keystore_super_block_t super;
    pthread_mutex_t freelist_mutex;
} storage_state_t;

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

// Storage lifecycle
int storage_open_or_create(const char *path,
                           uint32_t default_block_size,
                           uint32_t default_num_blocks,
                           storage_state_t *out_state);
void storage_close(storage_state_t *state);
void storage_print_superblock_ascii(const storage_state_t *state);

// Free-list management (persistent on-disk singly-linked list of free blocks)
int freelist_format(storage_state_t *state);
int storage_block_alloc(storage_state_t *state, uint32_t *out_block_index);
int storage_block_free(storage_state_t *state, uint32_t block_index);