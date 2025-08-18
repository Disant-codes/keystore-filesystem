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
    while (!q->head) {
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