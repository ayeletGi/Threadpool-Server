#include "threadpool.h"
#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<stdlib.h>
#include<unistd.h>

/*
Name: Ayelet Gibli
Id: 208691675
*/

threadpool* create_threadpool(int num_threads_in_pool)
{
    int s;

    /* check the legacy of the parameter */
    if(num_threads_in_pool < 0 || num_threads_in_pool > MAXT_IN_POOL){
        return NULL;
    }

    /* create threadpool structure */
    threadpool* pool = (threadpool*)malloc(sizeof(threadpool));
    if(pool == NULL){
        perror("malloc.");
        return NULL;
    }
    /* create threadpool structure */
    pool->num_threads = num_threads_in_pool;
    pool->qsize = 0;
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads_in_pool);
    if(pool->threads == NULL){
        free(pool);
        perror("malloc.");
        return NULL;
    }
    pool->qhead = NULL;
    pool->qtail = NULL;
    s = pthread_mutex_init(&pool->qlock, NULL);
    if( s != 0){
        free(pool);
        perror("pthread_mutex_init.");
        return NULL;
    }
    s = pthread_cond_init(&pool->q_not_empty, NULL);
    if( s != 0){
        free(pool);
        perror("pthread_cond_init.");
        return NULL;
    }
    s = pthread_cond_init(&pool->q_empty, NULL);
    if( s != 0){
        free(pool);
        perror("pthread_cond_init.");
        return NULL;
    }
    pool->shutdown = 0;
    pool->dont_accept = 0;
    for(int i = 0; i < num_threads_in_pool; i++){
       s = pthread_create(&pool->threads[i], NULL ,do_work, (void*)pool);
       if(s != 0){
            free(pool);
            perror("pthread_create");
            return NULL;
        }
    }
    return pool;
}

void dispatch(threadpool* from_me, dispatch_fn dispatch_to_here, void *arg)
{
    int s;
    /* create work_t structure and init it with the routine and argument. */
    work_t* work = (work_t*)malloc(sizeof(work_t));
    if( work == NULL){
        perror("malloc.");
        return;
    }
    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;
    //lock
    s = pthread_mutex_lock(&from_me->qlock);
    if(s != 0){
        free(work);
        perror("pthread_mutex_lock");
        return ;
    }
    /* if destroy function has begun, don't accept new item to the queue */
    if(from_me->dont_accept == 1){
        //unlock
        s = pthread_mutex_unlock(&from_me->qlock);
        if(s != 0){
            free(work);
            perror("pthread_mutex_unlock");
            return;
        }
        free(work);
        return;
    }
    /* add item to the queue */
    //add to the queue
    if(from_me->qsize == 0){
        from_me->qhead = work;
        from_me->qtail = work;
    }
    else{
        from_me->qtail->next = work;
        from_me->qtail = work;
        from_me->qtail->next = NULL;
    }
    from_me->qsize ++;
    //signal queue not empty
    s = pthread_cond_signal(&from_me->q_not_empty);
    if(s != 0){
        perror("pthread_cond_signal");
        return;
    }
    //unlock
    s = pthread_mutex_unlock(&from_me->qlock);
    if(s != 0){
        perror("pthread_mutex_unlock");
        return;
    }
}

void* do_work(void* p)
{
    threadpool* pool= (threadpool*)p;
    int s ;
    while(1)
    {
        //lock
        s = pthread_mutex_lock(&pool->qlock);
        if(s != 0){
            perror("pthread_mutex_lock");
            return NULL;
        }
        //destruction process has begun?
        if(pool->shutdown == 1){
            s = pthread_mutex_unlock(&pool->qlock);
            if(s != 0){
                perror("pthread_mutex_unlock");
                return NULL;
            }
            return NULL;
        }
        //queue is empty?
        if(pool->qsize == 0){
            s = pthread_cond_wait(&pool->q_not_empty,&pool->qlock);
            if(s != 0){
                perror("pthread_cond_wait");
                return NULL;
            }
        }
        //check again destructor flag
        if(pool->shutdown == 1){
            s = pthread_mutex_unlock(&pool->qlock);
            if(s != 0){
                perror("pthread_mutex_unlock");
                return NULL;
            }
            return NULL;
        }

        //take the first element from the queue.
        work_t* curr = pool->qhead;
        //update head
        pool->qsize --;
        if(pool->qsize != 0)
            pool->qhead = pool->qhead->next;
        else{
            pool->qhead = NULL;
            pool->qtail = NULL;        
        }

        //if the queue becomes empty and destruction process wait to begin, signal destruction process.
        if((pool->qsize == 0) && (pool->dont_accept == 1)){
            s = pthread_cond_signal(&pool->q_empty);
            if(s != 0){
                perror("pthread_cond_signal");
                return NULL;
            }
        }
        //unlock
        s = pthread_mutex_unlock(&pool->qlock);
        if(s != 0){
            perror("pthread_mutex_unlock");
            return NULL;
        }
        //call the thread routine.
        curr->routine(curr->arg);
        free(curr);
    }

    return NULL;
}

void destroy_threadpool(threadpool* destroyme)
{
    int s;
    //lock
    s = pthread_mutex_lock(&destroyme->qlock);
    if(s != 0){
        perror("pthread_mutex_lock");
        return;
    }
    /* Set don’t_accept flag to 1 */
    destroyme->dont_accept = 1;
    /* Wait for queue to become empty */
    if(destroyme->qsize >0){
        s = pthread_cond_wait(&destroyme->q_empty,&destroyme->qlock);
        if(s != 0){
            perror("pthread_cond_wait");
            return;
        }
    }
    /* Set shutdown flag to 1 */
    destroyme->shutdown = 1;
    //unlock
    s = pthread_mutex_unlock(&destroyme->qlock);
    if(s != 0){
        perror("pthread_mutex_unlock");
        return;
    }
    /* Signal threads that wait on empty queue, so they can wake up, see shutdown flag and exit. */
    s = pthread_cond_broadcast(&destroyme->q_not_empty);
    if(s != 0){
        perror("pthread_cond_broadcast");
        return;
    }
    /* Join all threads */
    for(int i = 0; i < destroyme->num_threads; i++){
        s = pthread_join(destroyme->threads[i], NULL);
        if(s != 0){
            perror("pthread_join");
            return;
        }
    }

    /* Free whatever you have to free. */
    free(destroyme->threads);
    pthread_cond_destroy(&destroyme->q_not_empty);
    pthread_cond_destroy(&destroyme->q_empty);
    pthread_mutex_destroy(&destroyme->qlock);
    free(destroyme);
}
