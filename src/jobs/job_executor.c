#include "job_executor.h"


job_queue * job_queue_init(void){
    job_queue *q = (job_queue *)calloc(1,sizeof(job_queue));
    if(!q) return NULL;
    pthread_mutex_init(&q->p_mutex,NULL);
    pthread_cond_init(&q->p_cond,NULL);
    return q;
}

void job_queue_free(job_queue *q){
    if(!q) return;
    pthread_mutex_lock(&q->p_mutex);
    while (q->head) {
        job * j = q->head->next_job;
        job_free(q->head);
        q->head = j;
    }
    pthread_mutex_unlock(&q->p_mutex);
    pthread_mutex_destroy(&q->p_mutex);
    pthread_cond_destroy(&q->p_cond);
}

void job_init(job_request *req){
    if(!req) return;
    job *j = (job *)calloc(1,sizeof(j));
    if(!j) return;
    j->request = req;
    j->response = job_response_init(req->type);
    j->next_job = NULL;
}

void job_free(job *j){
    if(!j) return;
    if(j->request) free(j->request);
    if(j->response) free(j->response);
    free(j);
    j=NULL;
}

void job_push(job_queue *q,job *j){
    j->next_job = NULL;
    pthread_mutex_lock(&q->p_mutex);
    if (q->tail) q->tail->next_job = j; else q->head = j;
    q->tail = j;
    pthread_cond_signal(&q->p_cond);
    pthread_mutex_unlock(&q->p_mutex);
    update_job_status(j,SUBMITTED);
    notify_job_status(j);
}

job * job_pop(job_queue *q){
    pthread_mutex_lock(&q->p_mutex);
    while (!q->head) {
        pthread_cond_wait(&q->p_cond, &q->p_mutex);
    }
    job *j = q->head;
    if (j) {
        q->head = j->next_job;
        if (!q->head) q->tail = NULL;
    }
    pthread_mutex_unlock(&q->p_mutex);
    update_job_status(j,PROCESSING);
    notify_job_status(j);
    return j;
}

job_response * job_response_init(enum job_type type){
    job_response *res = (job_response *) calloc(1,sizeof(job_response));
    if(!res) return NULL;

    res->type =  type;
    res->status = NOT_STARTED;
    res->error = NO_ERROR;
    res->data_len = 0;
    res->data = NULL;

    return res;
}

void job_response_free(job_response *res){
    free(res);
    res = NULL;
}

job_request * job_request_init(enum job_type type,char *key, char *value){
    job_request * req = (job_request *)calloc(1,sizeof(job_request));
    if (!req) return NULL;
    req->type = type;
    snprintf(req->key, strlen(key), "%s", key);
    snprintf(req->value, strlen(value), "%s", value);
    return req;
}

void job_request_free(job_request *req){
    free(req);
    req=NULL;
}

void process_job(job *work_job){
    int rc = 0;
    if (!work_job || !work_job->response || !work_job->request) {
        return;
    }
    // Mark as processing
    update_job_status(work_job,PROCESSING);
    notify_job_status(work_job);

    // For now, we simply mark the job as completed immediately.
    switch (work_job->request->type) {
        case PUT:
        case DELETE:
        case GET:
        default:
            rc = 0;
            break;
    }
    rc == 0 ? update_job_status(work_job,COMPLETED): update_job_status(work_job,FAILED);
    notify_job_status(work_job);
}

void * job_worker_thread(void *arg){
    job_queue *queue = (job_queue *)arg;
    for(;;){
        job *work_job = job_pop(queue);
        if (!work_job) {
            continue;
        }
        process_job(work_job);
        job_free(work_job);
    }
    return NULL;
}

int job_worker_pool_init(job_queue *queue, int num_threads){
    if (!queue) return 0;
    if (num_threads <= 0) num_threads = JOB_WORKER_THREAD_COUNT;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int started = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_t tid;
        int rc = pthread_create(&tid, &attr, job_worker_thread, (void *)queue);
        if (rc == 0) {
            started++;
        } else {
            // Stop attempting further threads on failure
            break;
        }
    }

    pthread_attr_destroy(&attr);
    return started;
}

void update_job_status(job *work_job,enum job_status status){
    if(!work_job) return;
    work_job->response->status = status;
}

void notify_job_status(job *work_job){
    if(!work_job) return;
    if (send(work_job->client_fd, work_job->response, sizeof(job_response), 0) < 0) {
        syslog(LOG_ERR, "keystored::failed to send response to client %d", 
            work_job->response->status);
    }
}