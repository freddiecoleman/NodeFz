/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv-common.h"

#if !defined(_WIN32)
# include "unix/internal.h"
#else
# include "win/req-inl.h"
/* TODO(saghul): unify internal req functions */
static void uv__req_init(uv_loop_t* loop,
                         uv_req_t* req,
                         uv_req_type type) {
  uv_req_init(loop, req);
  req->type = type;
  uv__req_register(loop, req);
}
# define uv__req_init(loop, req, type) \
    uv__req_init((loop), (uv_req_t*)(req), (type))
#endif

#include <stdlib.h>
#include "scheduler.h"
#include "logical-callback-node.h"
#include "unified-callback-enums.h"

#define MAX_THREADPOOL_SIZE 1

static uv_once_t once = UV_ONCE_INIT;
static uv_cond_t cond;
static uv_mutex_t mutex;
int n_work_items = 0; /* The total number of work items retrieved from wq by a worker thread. Protected by mutex. */
static unsigned int idle_threads;
static unsigned int nthreads;
static uv_thread_t* threads;
static uv_thread_t default_threads[1];
static QUEUE exit_message;
static QUEUE wq;
static volatile int initialized;

static void uv__queue_work(struct uv__work* w);
static void uv__queue_done(struct uv__work* w, int err);

static void uv__cancelled(struct uv__work* w) {
  abort();
}

static void post(QUEUE* q) {
  uv_mutex_lock(&mutex);
  QUEUE_INSERT_TAIL(&wq, q);
  if (idle_threads > 0)
  {
    mylog(LOG_THREADPOOL, 9, "Signal'ing the threadpool work cond\n");
    uv_cond_signal(&cond);
  }
  uv_mutex_unlock(&mutex);
}


/* To avoid deadlock with uv_cancel() it's crucial that the worker
 * never holds the global mutex and the loop-local mutex at the same time.
 */
static void worker(void* arg) {
  /* JD: TODO: Add calls to scheduler. */
  struct uv__work* w;
  QUEUE* q;
  int work_item_number = -1; /* Holds value of n_work_items when worker gets each work item. */

  /* Scheduler supplies. */
  spd_got_work_t spd_got_work;

  (void) arg;

  scheduler_register_thread(THREAD_TYPE_THREADPOOL);

  for (;;) {
    mylog(LOG_THREADPOOL, 1, "worker: begins\n");
    uv_mutex_lock(&mutex);

    while (QUEUE_EMPTY(&wq)) {
      mylog(LOG_THREADPOOL, 1, "worker: No work, waiting. %i LCBNs already executed.\n", scheduler_n_executed());
      idle_threads += 1;
      uv_cond_wait(&cond, &mutex);
      idle_threads -= 1;
    }
    mylog(LOG_THREADPOOL, 9, "worker: There is something in the queue\n");

    q = QUEUE_HEAD(&wq);

    if (q == &exit_message)
      uv_cond_signal(&cond);
    else {
      QUEUE_REMOVE(q);
      QUEUE_INIT(q);  /* Signal uv_cancel() that the work req is
                             executing. */
      work_item_number = n_work_items;
      n_work_items++;
    }

    uv_mutex_unlock(&mutex);

    if (q == &exit_message)
      break;

    w = QUEUE_DATA(q, struct uv__work, wq);

    /* Yield to scheduler. */
    spd_got_work_init(&spd_got_work);
    spd_got_work.work_item = w;
    spd_got_work.work_item_num = work_item_number;
    scheduler_thread_yield(SCHEDULE_POINT_TP_GOT_WORK, &spd_got_work);

#if UNIFIED_CALLBACK
    invoke_callback_wrap((any_func) w->work, UV__WORK_WORK, (long int) w);
#else
    w->work(w);
#endif

    uv_mutex_lock(&w->loop->wq_mutex);
    w->work = NULL;  /* Signal uv_cancel() that the work req is done
                        executing. */
    QUEUE_INSERT_TAIL(&w->loop->wq, &w->wq);
    uv_async_send(&w->loop->wq_async);
    uv_mutex_unlock(&w->loop->wq_mutex);
  }
}


#ifndef _WIN32
UV_DESTRUCTOR(static void cleanup(void)) {
  unsigned int i;

  if (initialized == 0)
    return;

  post(&exit_message);

  for (i = 0; i < nthreads; i++)
    if (uv_thread_join(threads + i))
      abort();

  if (threads != default_threads)
    uv__free(threads);

  uv_mutex_destroy(&mutex);
  uv_cond_destroy(&cond);

  threads = NULL;
  nthreads = 0;
  initialized = 0;
}
#endif


static void init_once(void) {
  unsigned int i;
  const char* val;

  mylog_init();

  nthreads = ARRAY_SIZE(default_threads);
  val = getenv("UV_THREADPOOL_SIZE");
  if (val != NULL)
    nthreads = atoi(val);
  if (nthreads == 0)
    nthreads = 1;
  if (nthreads > MAX_THREADPOOL_SIZE)
    nthreads = MAX_THREADPOOL_SIZE;

  threads = default_threads;
  if (nthreads > ARRAY_SIZE(default_threads)) {
    threads = uv__malloc(nthreads * sizeof(threads[0]));
    if (threads == NULL) {
      nthreads = ARRAY_SIZE(default_threads);
      threads = default_threads;
    }
  }

  if (uv_cond_init(&cond))
    abort();

  if (uv_mutex_init(&mutex))
    abort();

  QUEUE_INIT(&wq);

  for (i = 0; i < nthreads; i++)
    if (uv_thread_create(threads + i, worker, NULL))
      abort();

  initialized = 1;
}


void uv__work_submit(uv_loop_t* loop,
                     struct uv__work* w,
                     void (*work)(struct uv__work* w),
                     void (*done)(struct uv__work* w, int status)) {
  uv_once(&once, init_once);
  w->loop = loop;
  w->work = work;
  w->done = done;

  post(&w->wq);
}

static int uv__work_cancel(uv_loop_t* loop, uv_req_t* req, struct uv__work* w) {
  int can_cancel;

  uv_mutex_lock(&mutex);
  uv_mutex_lock(&w->loop->wq_mutex);

  can_cancel = !QUEUE_EMPTY(&w->wq) && w->work != NULL;
  if (can_cancel)
    QUEUE_REMOVE(&w->wq);

  uv_mutex_unlock(&w->loop->wq_mutex);
  uv_mutex_unlock(&mutex);

  if (!can_cancel)
    return UV_EBUSY;

  w->work = uv__cancelled;
  uv_mutex_lock(&loop->wq_mutex);
  QUEUE_INSERT_TAIL(&loop->wq, &w->wq);
  uv_async_send(&loop->wq_async);
  mylog(LOG_THREADPOOL, 1, "uv__work_cancel: signal'd a cancelled 'done' item (w %p)\n", w);
  uv_mutex_unlock(&loop->wq_mutex);

  return 0;
}


void uv__work_done(uv_async_t* handle) {
  struct uv__work* w;
  uv_loop_t* loop;
  QUEUE* q;
  QUEUE wq;
  int err;
  enum callback_type next_lcbn_type = CALLBACK_TYPE_ANY;
  int replay_mode = (scheduler_get_scheduler_mode() == SCHEDULER_MODE_REPLAY);
  int done = 0;

  loop = container_of(handle, uv_loop_t, wq_async);

  /* In REPLAY mode, spin until the next CB is a non-AFTER_WORK looper CB. 
   * The next non-threadpool CB might be us, and if we don't spin, returning introduces an unexpected UV_ASYNC_CB into the schedule.
   */
  do
  {
    mylog(LOG_THREADPOOL, 5, "uv__work_done: Checking for done LCBNs\n");
    QUEUE_INIT(&wq);

    uv_mutex_lock(&loop->wq_mutex);
    if (!QUEUE_EMPTY(&loop->wq)) {
      q = QUEUE_HEAD(&loop->wq);
      QUEUE_SPLIT(&loop->wq, q, &wq);
    }
    uv_mutex_unlock(&loop->wq_mutex);

    while (!QUEUE_EMPTY(&wq)) {
      q = QUEUE_HEAD(&wq);
      QUEUE_REMOVE(q);

      w = container_of(q, struct uv__work, wq);
      err = (w->work == uv__cancelled) ? UV_ECANCELED : 0;
#ifdef UNIFIED_CALLBACK
      invoke_callback_wrap((any_func) w->done, UV__WORK_DONE, (long int) w, (long int) err);
#else
      w->done(w, err);
#endif

      if (replay_mode)
      {
        /* We might wish to defer the remaining done items.
         * This is legal because it simulates a delayed placement of done items into the wq.
         */
        next_lcbn_type = scheduler_next_lcbn_type();
        done = !(next_lcbn_type == UV_AFTER_WORK_CB || is_threadpool_cb(next_lcbn_type));
        if (done)
          break;
      }
    }

    if (replay_mode)
    {
      next_lcbn_type = scheduler_next_lcbn_type();
      done = !(next_lcbn_type == UV_AFTER_WORK_CB || is_threadpool_cb(next_lcbn_type));
    }
    else
      done = 1;
  } while (!done);

  mylog(LOG_THREADPOOL, 5, "uv__work_done: Next type is %s, so I can return\n", callback_type_to_string(scheduler_next_lcbn_type()));

  if (replay_mode)
  {
    /* Unshift any deferred done items onto wq. */
    int len = 0;
    uv_mutex_lock(&loop->wq_mutex);
    QUEUE_LEN(len, q, &wq);
    mylog(LOG_THREADPOOL, 3, "uv__work_done: Replacing the %i deferred 'done' items\n", len);
    while (!QUEUE_EMPTY(&wq)) {
      q = QUEUE_HEAD(&wq);
      QUEUE_REMOVE(q);
      QUEUE_INIT(q);
      QUEUE_INSERT_HEAD(&loop->wq, q);
    }
    uv_mutex_unlock(&loop->wq_mutex);

    uv_async_send(&loop->wq_async); /* Pending done items! */
  }

}

static void uv__queue_work(struct uv__work* w) {
  uv_work_t* req = container_of(w, uv_work_t, work_req);

#ifdef UNIFIED_CALLBACK
  invoke_callback_wrap((any_func) req->work_cb, UV_WORK_CB, (long) req);
#else
  req->work_cb(req);
#endif
}

any_func uv_uv__queue_work_ptr (void)
{
  return (any_func) uv__queue_work;
}

static void uv__queue_done(struct uv__work* w, int err) {
  uv_work_t* req;

  req = container_of(w, uv_work_t, work_req);
  uv__req_unregister(req->loop, req);

  if (req->after_work_cb == NULL)
    return;

#ifdef UNIFIED_CALLBACK
  invoke_callback_wrap((any_func) req->after_work_cb, UV_AFTER_WORK_CB, (long int) req, (long int) err);
#else
  req->after_work_cb(req, err);
#endif
}

any_func uv_uv__queue_done_ptr (void)
{
  return (any_func) uv__queue_done;
}

/* Add extra LCBN dependencies if REQ is of one of the 'wrapped' paths
    (fs, getaddrinfo, getnameinfo). */ 
void uv__add_other_dependencies (uv_work_t *req)
{
  enum callback_type wrapped_work_type, wrapped_done_type;
  lcbn_t *work_lcbn;
  uv_req_t *wrapped_req;

  assert(req != NULL);

  /* Identify the type. */
  work_lcbn = lcbn_get(req->cb_type_to_lcbn, UV_WORK_CB);
  if (work_lcbn->cb == uv_uv__fs_work_wrapper_ptr())
  {
    wrapped_work_type = UV_FS_WORK_CB;
    wrapped_done_type = UV_FS_CB;
  }
  else if (work_lcbn->cb == uv_uv__getaddrinfo_work_wrapper_ptr())
  {
    wrapped_work_type = UV_GETADDRINFO_WORK_CB;
    wrapped_done_type = UV_GETADDRINFO_CB;
  }
  else if (work_lcbn->cb == uv_uv__getnameinfo_work_wrapper_ptr())
  {
    wrapped_work_type = UV_GETNAMEINFO_WORK_CB;
    wrapped_done_type = UV_GETNAMEINFO_CB;
  }
  else
    /* Not a wrapped request, nothing to do. */
    return;

  wrapped_req = (uv_req_t *) req->data;

  /* WORK -> wrapped_work_type */
  lcbn_add_dependency(lcbn_get(req->cb_type_to_lcbn, UV_WORK_CB),
                      lcbn_get(wrapped_req->cb_type_to_lcbn, wrapped_work_type));
  /* wrapped_work_type -> AFTER_WORK */
  lcbn_add_dependency(lcbn_get(wrapped_req->cb_type_to_lcbn, wrapped_work_type),
                      lcbn_get(req->cb_type_to_lcbn, UV_AFTER_WORK_CB));
  /* AFTER_WORK -> wrapped_done_type */
  lcbn_add_dependency(lcbn_get(req->cb_type_to_lcbn, UV_AFTER_WORK_CB),
                      lcbn_get(wrapped_req->cb_type_to_lcbn, wrapped_done_type));
  return;
}

int uv_queue_work(uv_loop_t* loop,
                  uv_work_t* req,
                  uv_work_cb work_cb,
                  uv_after_work_cb after_work_cb) {
  if (work_cb == NULL)
    return UV_EINVAL;

  uv__req_init(loop, req, UV_WORK);
  req->loop = loop;
  req->work_cb = work_cb;
  req->after_work_cb = after_work_cb;

#ifdef UNIFIED_CALLBACK
  uv__register_callback(req, (any_func) work_cb, UV_WORK_CB);
  uv__register_callback(req, (any_func) after_work_cb, UV_AFTER_WORK_CB);
  /* WORK -> AFTER_WORK. */
  lcbn_add_dependency(lcbn_get(req->cb_type_to_lcbn, UV_WORK_CB),
                      lcbn_get(req->cb_type_to_lcbn, UV_AFTER_WORK_CB));
  uv__add_other_dependencies(req);
#endif

  uv__work_submit(loop, &req->work_req, uv__queue_work, uv__queue_done);
  return 0;
}


int uv_cancel(uv_req_t* req) {
  struct uv__work* wreq;
  uv_loop_t* loop;

  switch (req->type) {
  case UV_FS:
    loop =  ((uv_fs_t*) req)->loop;
    wreq = &((uv_fs_t*) req)->work_req;
    break;
  case UV_GETADDRINFO:
    loop =  ((uv_getaddrinfo_t*) req)->loop;
    wreq = &((uv_getaddrinfo_t*) req)->work_req;
    break;
  case UV_GETNAMEINFO:
    loop = ((uv_getnameinfo_t*) req)->loop;
    wreq = &((uv_getnameinfo_t*) req)->work_req;
    break;
  case UV_WORK:
    loop =  ((uv_work_t*) req)->loop;
    wreq = &((uv_work_t*) req)->work_req;
    break;
  default:
    return UV_EINVAL;
  }

  return uv__work_cancel(loop, req, wreq);
}
