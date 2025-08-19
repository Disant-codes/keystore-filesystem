#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <errno.h>
#define MAX_KEY_LENGTH 128
#define MAX_VALUE_LENGTH 1024
#define JOB_WORKER_THREAD_COUNT 16

enum job_type{
    INVALID_TYPE = -1,
    PUT = 1,
    GET = 2,
    DELETE = 3,
    
};

enum job_error_code{
    INVALID_KEY,
    STORAGE_FULL,
    NO_ERROR
};

enum job_status{
    NOT_STARTED,
    SUBMITTED,
    PROCESSING,
    COMPLETED,
    FAILED
};

typedef struct job_request{
    enum job_type type;
    char key[MAX_KEY_LENGTH];
    char value[MAX_VALUE_LENGTH];
} job_request;

typedef struct job_response{
    enum job_type type;
    enum job_status status;
    enum job_error_code error;
    int data_len;
    char *data;
} job_response;

typedef struct job{
    int client_fd;
    job_request *request;
    job_response *response;
    struct job *next_job;
} job;

typedef struct job_queue{
    job *head, *tail;
    pthread_mutex_t p_mutex;
    pthread_cond_t p_cond;
} job_queue;

job_request * job_request_init(enum job_type type,char *key, char *value);
void job_request_free(job_request *req);

job_queue * job_queue_init(void);
void job_queue_free(job_queue *q);

void job_init(job_request *job_req);
void job_free(job *j);

void job_push(job_queue *q, job *j);
job * job_pop(job_queue *q);

job_response * job_response_init(enum job_type type);
void job_response_free(job_response *res);

void process_job(job *work_job);
void * job_worker_thread(void *arg);
int job_worker_pool_init(job_queue *queue, int num_threads);

void update_job_status(job *work_job,enum job_status);
void notify_job_status(job *work_job);