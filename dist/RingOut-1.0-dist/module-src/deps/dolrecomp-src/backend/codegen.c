#include "backend/codegen.h"
#include "backend/emitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct {
    const ChunkJob* jobs;
    u32 job_count;
    u32 next_job;
    int failed;
    pthread_mutex_t lock;
} WorkerQueue;

int emit_chunk_file(const ChunkJob* job) {
    FILE* chunk = fopen(job->path, "w");
    if (!chunk) {
        fprintf(stderr, "error: can't open output '%s'\n", job->path);
        return 0;
    }

    fprintf(chunk, "// DolRecomp output\n");
    fprintf(chunk, "#include \"../%s\"\n\n", job->include_name);
    emit_function(chunk, job->insts, job->count, job->func_addr);

    if (fclose(chunk) != 0) {
        fprintf(stderr, "error: failed writing '%s'\n", job->path);
        return 0;
    }

    return 1;
}

int queue_init(WorkerQueue* queue, const ChunkJob* jobs, u32 job_count) {
    queue->jobs = jobs;
    queue->job_count = job_count;
    queue->next_job = 0;
    queue->failed = 0;
    return pthread_mutex_init(&queue->lock, NULL) == 0;
}

void queue_destroy(WorkerQueue* queue) {
    pthread_mutex_destroy(&queue->lock);
}

void queue_lock(WorkerQueue* queue) {
    pthread_mutex_lock(&queue->lock);
}

void queue_unlock(WorkerQueue* queue) {
    pthread_mutex_unlock(&queue->lock);
}

int queue_take_job(WorkerQueue* queue, const ChunkJob** job) {
    int ok = 0;

    queue_lock(queue);
    if (!queue->failed && queue->next_job < queue->job_count) {
        *job = &queue->jobs[queue->next_job++];
        ok = 1;
    }
    queue_unlock(queue);

    return ok;
}

void queue_mark_failed(WorkerQueue* queue) {
    queue_lock(queue);
    queue->failed = 1;
    queue_unlock(queue);
}

void* chunk_worker_main(void* arg) {
    WorkerQueue* queue = (WorkerQueue*)arg;
    const ChunkJob* job = NULL;

    while (queue_take_job(queue, &job)) {
        if (!emit_chunk_file(job)) {
            queue_mark_failed(queue);
            break;
        }
    }

    return NULL;
}

int run_chunk_jobs(const ChunkJob* jobs, u32 job_count, u32 requested_jobs) {
    if (job_count == 0)
        return 1;

    if (requested_jobs == 0)
        requested_jobs = 1;
    if (requested_jobs > job_count)
        requested_jobs = job_count;

    if (requested_jobs == 1) {
        for (u32 i = 0; i < job_count; i++) {
            if (!emit_chunk_file(&jobs[i]))
                return 0;
        }
        return 1;
    }

    WorkerQueue queue;
    if (!queue_init(&queue, jobs, job_count)) {
        fprintf(stderr, "error: can't start worker queue\n");
        return 0;
    }

    pthread_t* threads = (pthread_t*)calloc(requested_jobs, sizeof(pthread_t));
    if (!threads) {
        queue_destroy(&queue);
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    u32 created = 0;
    for (; created < requested_jobs; created++) {
        if (pthread_create(&threads[created], NULL, chunk_worker_main, &queue) != 0) {
            queue_mark_failed(&queue);
            fprintf(stderr, "error: can't start worker thread\n");
            break;
        }
    }

    for (u32 i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    free(threads);

    int ok = !queue.failed;
    queue_destroy(&queue);
    return ok;
}

u32 effective_chunk_jobs(u32 job_count, u32 requested_jobs) {
    if (job_count == 0)
        return 0;
    if (requested_jobs == 0)
        requested_jobs = 1;
    if (requested_jobs > job_count)
        requested_jobs = job_count;
    return requested_jobs;
}

