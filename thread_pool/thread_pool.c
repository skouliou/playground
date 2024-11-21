#define _GNU_SOURCE

#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>


// TODO: thread safe logging
// TODO: implement an object pool?
#define xmalloc malloc
#define xfree free

#ifdef _THP_LOGGING
#include <errno.h>
#include <string.h>

#include "log.h"

# define thp_log_info(...) log_info(__VA_ARGS__)
# define thp_log_debug(...) log_debug(__VA_ARGS__)
# define thp_log_error(...) log_error(__VA_ARGS__)
#else
# define thp_log_info(...)
# define thp_log_debug(...)
# define thp_log_error(...)
#endif // _THC_LOGGING

///////////////////////////////////////////////////////////////////////
/// Typedefs
///////////////////////////////////////////////////////////////////////

typedef struct thp_sched_s thp_sched_t;
typedef struct thp_queue_s thp_queue_t;
typedef struct thp_futur_s thp_future_t;
typedef struct thp_task_s thp_task_t;
typedef struct thp_s thp_t;
typedef struct thp_attr_s thp_attr_t;

typedef enum thp_task_status_t (*thp_task_func)(void *args, void **result);
typedef void (*thp_task_cleanup_func)(void *args);

///////////////////////////////////////////////////////////////////////
/// Future
///////////////////////////////////////////////////////////////////////

struct thp_futur_s {
  void *value;
  sem_t ready;
};

/// allocate a new empty future
static thp_future_t *thp_future_new() {
  thp_future_t *future = xmalloc(sizeof(thp_future_t));
  if (future == NULL) {
    thp_log_error("could not create future (err: %s)", strerror(errno));
    return NULL;
  }

  // initialization
  if (sem_init(&future->ready, 0, 0) == -1) {
    thp_log_error("could not create future (err: %s)", strerror(errno));
    xfree(future);
    return NULL;
  }

  return future;
}

/// extract result from `future`, blocks if future is not ready
static void *thp_future_consume(thp_future_t *future) {
  assert(future);
  void *result = NULL;
  // wait for the task related to this future to complete
  thp_log_debug("future (%p) waiting...", future);
  sem_wait(&future->ready);
  // extract the result from teh future
  result = future->value;
  sem_destroy(&future->ready);
  xfree(future);
  thp_log_info("future (%p) ready", future);
  return result;
}

///////////////////////////////////////////////////////////////////////
/// Task
///////////////////////////////////////////////////////////////////////

#include <sys/queue.h>

struct thp_task_s {
  // circular queue links
  CIRCLEQ_ENTRY(thp_task_s) link;
  // future related to this task
  thp_future_t *future;
  // assinged work
  thp_task_func run;
  // assinged work args
  void *args;
  // user defined for memory dealocation
  thp_task_cleanup_func cleanup;
};

/// construct a `task` from the passed arguments
static thp_task_t *thp_task_new(thp_task_func work, void *args,
                                thp_task_cleanup_func cleanup,
                                thp_future_t *future) {
  assert(future);

  thp_task_t *task = xmalloc(sizeof(thp_task_t));
  if (task == NULL) {
    thp_log_error("could not create task (err: %s)", strerror(errno));
    return NULL;
  }

  // initialize task
  *task = (thp_task_t){
      .args = args,
      .future = future,
      .cleanup = cleanup,
      .run = work,
  };

  return task;
}

/// attach the result value to the future related to this task
/// and signal it's completion
static void thp_task_completed(thp_task_t *task, void *result) {
  assert(task);
  // attach the result value to the future related to
  // this task
  task->future->value = result;
  // signal that the task has completed
  sem_post(&task->future->ready);
  // clean up `args`
  if (task->cleanup != NULL)
    task->cleanup(task->args);

  xfree(task);
}

///////////////////////////////////////////////////////////////////////
/// Task Queue
///////////////////////////////////////////////////////////////////////
// TODO: improve internal queue, maybe a min heap of priorities?

/// task queue
struct thp_queue_s {
  // queue mutex
  pthread_mutex_t lock;
  // queue nonempty conditional variable
  pthread_cond_t nonempty;
  // number of queued tasks
  size_t size;
  // queue backend
  CIRCLEQ_HEAD(/* anonymous */, thp_task_s) backend;
};

static int _thp_inner_cqueue_init(thp_queue_t *queue, size_t size) {
  (void)size;
  CIRCLEQ_INIT(&queue->backend);
  return 0;
}

static thp_task_t *_thp_inner_cqueue_pop_unchecked(thp_queue_t *queue) {
  assert(queue);
  // get the top most task
  thp_task_t *task = CIRCLEQ_FIRST(&queue->backend);
  // remove it from the queue
  CIRCLEQ_REMOVE(&queue->backend, task, link);
  // update size
  queue->size -= 1;
  return task;
}

/// initialize task queue
static inline int thp_init_queue(struct thp_queue_s *queue, size_t size,
                                 int (*init_backend)(thp_queue_t *, size_t)) {
  assert(queue);
  int r = 0;
  r = init_backend(queue, size);

  // inner queue initialization failed
  if (r == -1)
    return r;

  r = pthread_cond_init(&queue->nonempty, NULL);
  if (r == -1) {
    thp_log_error("unable to initialize conditional variable (err: %s)",
              strerror(errno));
    goto on_cond_error;
  }

  r = pthread_mutex_init(&queue->lock, NULL);
  if (r == -1) {
    thp_log_error("unable to initialize mutex (err: %s)", strerror(errno));
    goto on_mutex_error;
  }

on_mutex_error:
  pthread_cond_destroy(&queue->nonempty);
on_cond_error:
  return r;
}

///////////////////////////////////////////////////////////////////////
/// Scheduler
///////////////////////////////////////////////////////////////////////
/// TODO: implement better scheduling, maybe support different different
/// algorithms,
/// add a second queue for blocking tasks and add a mechanism so blocked
/// tasks could be moved from (blocking) queue to (ready to queue) queue
///

/// task scheduler, responsible for maintaining the task queue
struct thp_sched_s {
  // initialize queue on start up
  int (*init_queue_backend)(thp_queue_t *queue, size_t size_hint);
  // add task to queue
  void (*add_task)(thp_task_t *task, thp_queue_t *queue);
  // get task from queue
  thp_task_t *(*next_task)(thp_queue_t *queue);
  // pointer to the _external_ managed queue (not owned)
  thp_queue_t *queue;
};

static void _thp_sched_inner_cqueue_queue_task(thp_task_t *task,
                                               thp_queue_t *queue) {
  pthread_mutex_lock(&queue->lock);
  CIRCLEQ_INSERT_TAIL(&queue->backend, task, link);
  queue->size += 1;
  pthread_mutex_unlock(&queue->lock);
  pthread_cond_signal(&queue->nonempty);
}

static thp_task_t *_thp_sched_inner_cqueue_next_task(thp_queue_t *queue) {
  assert(queue);
  pthread_mutex_lock(&queue->lock);
  // block the calling thread untill a task is available
  while (queue->size < 1)
    pthread_cond_wait(&queue->nonempty, &queue->lock);

  // get the top most task
  thp_task_t *task = _thp_inner_cqueue_pop_unchecked(queue);
  pthread_mutex_unlock(&queue->lock);

  return task;
}

///////////////////////////////////////////////////////////////////////
/// Thread Pool
///////////////////////////////////////////////////////////////////////

/// default values
#define THP_DEFAULT_POOL_SIZE 4
#define THP_DEFAULT_QUEUE_SIZE 256

#define THP_DEFAULT_SCHED_ADD_TASK _thp_sched_inner_cqueue_queue_task
#define THP_DEFAULT_SCHED_GET_TASK _thp_sched_inner_cqueue_next_task
#define THP_DEFAULT_INIT_QUEUE _thp_inner_cqueue_init

/// thread pool attributes
struct thp_attr_s {
  /// thread pool size
  size_t pool_size;
  /// task queue size hint (could be ignored)
  size_t queue_size_hint;
  /// worker thread attributes
  pthread_attr_t *worker_attr;

  thp_sched_t scheduler;

  // INFO:  queue type? (heap, circular, linear, fixed, dynamic)
};

/// default attribute for thread pool creation
static const thp_attr_t THP_DEFAULT_ATTR = {
    .pool_size = THP_DEFAULT_POOL_SIZE,
    .queue_size_hint = THP_DEFAULT_QUEUE_SIZE,
    .worker_attr = NULL,
    .scheduler = {
        .add_task = THP_DEFAULT_SCHED_ADD_TASK,
        .next_task = THP_DEFAULT_SCHED_GET_TASK,
        .init_queue_backend = THP_DEFAULT_INIT_QUEUE,
    }};

/// thread pool control block
struct thp_s {
  // task queue
  thp_queue_t queue;
  // scheduler
  thp_sched_t scheduler;
  // thread pool size
  size_t size;
  // thread pool
  pthread_t *pool;
  // stop flag
  _Atomic(int) halt;
};

enum thp_task_status_t {
  // XXX: add other states (blocked, repeat..)
  THP_TASK_FINISHED,
  THP_TASK_PENDING,
};

static inline void _thp_worker_thread_cleanup(void *queue_lock) {
  thp_log_debug("thread worker (%p) cleanup", pthread_self());
  if (pthread_mutex_unlock(queue_lock) == -1) {
    thp_log_error("unable to unlock queue mutex");
  }
}

static void *thp_worker_loop(void *thp) {
  thp_log_info("thread (%lu) running fetch execute cycle...", pthread_self());
  thp_t *owner = thp;

  thp_sched_t *sched = &owner->scheduler;
  thp_task_t *task = NULL;
  void *result = NULL;
  enum thp_task_status_t status;

  // INFO: unlock queue mutex before exiting to avoid deadlock
  pthread_cleanup_push(_thp_worker_thread_cleanup, &sched->queue->lock);
  // should stop?
  while (owner->halt != 1) {
    // fetch next task
    task = sched->next_task(sched->queue);
    // run task
    thp_log_debug("thread(%lu) running task (%p)", pthread_self(), task);
    status = task->run(task->args, &result);

    switch (status) {
    case THP_TASK_FINISHED:
      // mark task as completed and notify the related the future
      thp_task_completed(task, result);
      thp_log_debug("task (%p) completed", task);
      break;

    case THP_TASK_PENDING:
      // requeue task to be run again
      owner->scheduler.add_task(task, &owner->queue);
      thp_log_debug("task (%p) requeued", task);
      break;
    }
  } // repeat cycle

  // INFO: mutex should be unlocked by now
  pthread_cleanup_pop(0);

  return NULL;
}

/// initialize thread pool
static inline int thp_init_pool(thp_t *thp, const pthread_attr_t *attr) {
  assert(thp);
  int r = 0;
  size_t init = 0;
  // initialize thread pool
  for (; init < thp->size; ++init) {
    r = pthread_create(&thp->pool[init], attr, thp_worker_loop, thp);
    // not enough resources? possible `EAGAIN`
    if (r == -1) {
      thp_log_error("thread creation failed (err: %s)", strerror(errno));
      break;
    }
    thp_log_debug("thread (%zu) created (id: %lu)", init, thp->pool[init]);
  }

  // cancel initialized threads in case of error
  if (r == -1) {
    thp_log_error("cancelling other threads");
    for (int j = 0; j < init; ++j) {
      // FIXME: change thread cancellation attribute
      pthread_cancel(thp->pool[j]);
    }
  }

  return r;
}

/// create a thread pool based on `attr`
thp_t *thp_create(thp_attr_t *attr) {
  // default values
  if (attr == NULL) {
    attr = (thp_attr_t *)&THP_DEFAULT_ATTR;
  }

  // thread pool control block
  thp_t *thp = xmalloc(sizeof(thp_t));
  assert(thp);

  // worker threads pool
  pthread_t *pool = xmalloc(attr->pool_size * sizeof(pthread_t));
  assert(pool);

  // initialize the thread pool control block
  *thp = (thp_t){
      .halt = 0,
      .pool = pool,
      .size = attr->pool_size,
  };

  thp_log_info("initializing scheduler...");
  thp->scheduler = (thp_sched_t){
      .queue = &thp->queue,
      .init_queue_backend = attr->scheduler.init_queue_backend,
      .add_task = attr->scheduler.add_task,
      .next_task = attr->scheduler.next_task,
  };

  int r;
  thp_log_info("initializing task queue...");
  r = thp_init_queue(&thp->queue, thp->size, thp->scheduler.init_queue_backend);
  assert(r != -1);

  thp_log_info("initializing worker threads...");
  r = thp_init_pool(thp, attr->worker_attr);
  assert(r != -1);

  // TODO: install a signal handler for single/batch thread termination
  // TODO: install a signal handler for task cancelation

  return thp;
}

/// assign a task to the thread pool `thp`
thp_future_t *thp_queue_task(thp_t *thp, thp_task_func work, void *args,
                             thp_task_cleanup_func cleanup) {
  assert(thp && work);

  thp_log_info("queuing task...");
  thp_future_t *future = thp_future_new();
  thp_log_debug("future (%p) created", future);
  assert(future);

  thp_task_t *task = thp_task_new(work, args, cleanup, future);
  thp_log_debug("task (%p) created", task);
  assert(task);

  thp->scheduler.add_task(task, &thp->queue);
  // return the future related to this task to the user
  return future;
}

/// wait for a future to complete and retrieve the result
void *thp_wait(thp_future_t *future) {
  assert(future);
  // wait for the task related to this future to complete
  return thp_future_consume(future);
}


// FIXME: implement this correctly
void thp_destroy(thp_t *thp) {
  assert(thp);
  thp_log_info("terminating thread pool (%p)", thp);
  thp->halt = 1;
  thp_log_info("cancelling worker threads");
  for (int i = 0; i < thp->size; ++i) {
    // pthread_sigqueue(thp->pool[i], SIGABRT, (union sigval){.sival_ptr =
    // NULL});
    pthread_cancel(thp->pool[i]);
    thp_log_debug("worker thread (%p) cancelled", thp->pool[i]);
  }

  thp_log_info("joining worker threads");
  for (int i = 0; i < thp->size; ++i) {
    pthread_join(thp->pool[i], NULL);
    thp_log_debug("worker thread (%p) joined", thp->pool[i]);
  }
}

///////////////////////////////////////////////////////////////////////
/// Test Run
///////////////////////////////////////////////////////////////////////

#include <unistd.h>

enum thp_task_status_t work(void *arg, void **res) {
  printf("Hello from thread! (%d)\n", *(int *)arg);
  *res = arg;
  return THP_TASK_FINISHED;
}

enum thp_task_status_t wait(void *arg, void **res) {
  sleep(5);
  printf("hello from wait! (%d)\n", *(int *)arg);
  *res = arg;
  return THP_TASK_FINISHED;
}

int main(void) {
  thp_t *thp = thp_create(NULL);
  int arg[10] = {0};
  thp_future_t *f = thp_queue_task(thp, wait, &arg[9], NULL);
  for (int i = 0; i < 10; ++i) {
    arg[i] = i;
    thp_queue_task(thp, work, &arg[i], NULL);
  }

  int r = *(int *)thp_wait(f);
  printf("returned from wait (%d)\n", r);

  thp_destroy(thp);
  return 0;
}
