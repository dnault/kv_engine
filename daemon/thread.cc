/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "connection.h"
#include "connections.h"
#include "cookie.h"
#include "front_end_thread.h"
#include "listening_port.h"
#include "log_macros.h"
#include "memcached.h"
#include "settings.h"
#include "stats.h"
#include "tracing.h"
#include <utilities/hdrhistogram.h>

#include <nlohmann/json.hpp>
#include <phosphor/phosphor.h>
#include <platform/strerror.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#ifndef WIN32
#include <netinet/tcp.h> // For TCP_NODELAY etc
#endif
#include <memory>

extern std::atomic<bool> memcached_shutdown;

/* An item in the connection queue. */
FrontEndThread::ConnectionQueue::~ConnectionQueue() {
    for (const auto& entry : connections) {
        safe_close(entry.first);
    }
}

void FrontEndThread::ConnectionQueue::push(SOCKET sock,
                                           SharedListeningPort interface) {
    std::lock_guard<std::mutex> guard(mutex);
    connections.emplace_back(sock, interface);
}

void FrontEndThread::ConnectionQueue::swap(
        std::vector<std::pair<SOCKET, SharedListeningPort>>& other) {
    std::lock_guard<std::mutex> guard(mutex);
    connections.swap(other);
}

static FrontEndThread dispatcher_thread;

void FrontEndThread::NotificationList::push(Connection* c) {
    std::lock_guard<std::mutex> lock(mutex);
    auto iter = std::find(connections.begin(), connections.end(), c);
    if (iter == connections.end()) {
        try {
            connections.push_back(c);
        } catch (const std::bad_alloc&) {
            // Just ignore and hopefully we'll be able to signal it at a later
            // time.
        }
    }
}

void FrontEndThread::NotificationList::remove(Connection* c) {
    std::lock_guard<std::mutex> lock(mutex);
    auto iter = std::find(connections.begin(), connections.end(), c);
    if (iter != connections.end()) {
        connections.erase(iter);
    }
}

void FrontEndThread::NotificationList::swap(std::vector<Connection*>& other) {
    std::lock_guard<std::mutex> lock(mutex);
    connections.swap(other);
}

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static std::vector<FrontEndThread> threads;
std::vector<Hdr1sfMicroSecHistogram> scheduler_info;

/*
 * Number of worker threads that have finished setting themselves up.
 */
static size_t init_count = 0;
static std::mutex init_mutex;
static std::condition_variable init_cond;

static void thread_libevent_process(evutil_socket_t, short, void*);

/*
 * Creates a worker thread.
 */
static void create_worker(void (*func)(void *), void *arg, cb_thread_t *id,
                          const char* name) {
    if (cb_create_named_thread(id, func, arg, 0, name) != 0) {
        FATAL_ERROR(EXIT_FAILURE,
                    "Can't create thread {}: {}",
                    name,
                    cb_strerror());
    }
}

/****************************** LIBEVENT THREADS *****************************/

void iterate_all_connections(std::function<void(Connection&)> callback) {
    for (auto& thr : threads) {
        TRACE_LOCKGUARD_TIMED(thr.mutex,
                              "mutex",
                              "iterate_all_connections::threadLock",
                              SlowMutexThreshold);
        iterate_thread_connections(&thr, callback);
    }
}

static void setup_dispatcher(struct event_base *main_base,
                             void (*dispatcher_callback)(evutil_socket_t, short, void *))
{
    dispatcher_thread.base = main_base;
	dispatcher_thread.thread_id = cb_thread_self();

    /* Listen for notifications from other threads */
        if ((event_assign(&dispatcher_thread.notify_event,
                          dispatcher_thread.base,
                          INVALID_SOCKET,
                          EV_READ | EV_PERSIST,
                          dispatcher_callback,
                          nullptr) == -1) ||
            (event_add(&dispatcher_thread.notify_event, nullptr) == -1)) {
            FATAL_ERROR(EXIT_FAILURE, "Can't monitor libevent notify pipe");
        }
    dispatcher_thread.running = true;
}

/*
 * Set up a thread's information.
 */
static void setup_thread(FrontEndThread& me) {
    me.base = event_base_new();

    if (!me.base) {
        FATAL_ERROR(EXIT_FAILURE, "Can't allocate event base");
    }

    /* Listen for notifications from other threads */
    if ((event_assign(&me.notify_event,
                      me.base,
                      INVALID_SOCKET,
                      EV_READ | EV_PERSIST,
                      thread_libevent_process,
                      &me) == -1) ||
        (event_add(&me.notify_event, nullptr) == -1)) {
        FATAL_ERROR(EXIT_FAILURE, "Can't monitor libevent notify pipe");
    }
}

/*
 * Worker thread: main event loop
 */
static void worker_libevent(void *arg) {
    auto& me = *reinterpret_cast<FrontEndThread*>(arg);

    // Any per-thread setup can happen here; thread_init() will block until
    // all threads have finished initializing.
    {
        std::lock_guard<std::mutex> guard(init_mutex);
        me.running = true;
        init_count++;
        init_cond.notify_all();
    }

    event_base_loop(me.base, 0);
    me.running = false;
}

static void dispatch_new_connections(FrontEndThread& me) {
    std::vector<std::pair<SOCKET, SharedListeningPort>> connections;
    me.new_conn_queue.swap(connections);

    for (const auto& entry : connections) {
        if (conn_new(entry.first, *entry.second, me.base, me) == nullptr) {
            if (entry.second->system) {
                --stats.system_conns;
            }
            safe_close(entry.first);
        }
    }
}

/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(evutil_socket_t fd, short, void* arg) {
    auto& me = *reinterpret_cast<FrontEndThread*>(arg);

    if (memcached_shutdown) {
        // Someone requested memcached to shut down. The listen thread should
        // be stopped immediately.
        if (is_listen_thread()) {
            LOG_INFO("Stopping listen thread (thread.cc)");
            event_base_loopbreak(me.base);
            return;
        }

        if (signal_idle_clients(me, false) == 0) {
            LOG_INFO("Stopping worker thread {}", me.index);
            event_base_loopbreak(me.base);
            return;
        }
    }

    dispatch_new_connections(me);

    FrontEndThread::PendingIoMap pending;
    {
        std::lock_guard<std::mutex> lock(me.pending_io.mutex);
        me.pending_io.map.swap(pending);
    }

    TRACE_LOCKGUARD_TIMED(me.mutex,
                          "mutex",
                          "thread_libevent_process::threadLock",
                          SlowMutexThreshold);

    std::vector<Connection*> notify;
    me.notification.swap(notify);

    for (const auto& io : pending) {
        auto* c = io.first;

        // Remove from the notify list if it's there as we don't
        // want to run them twice
        {
            auto iter = std::find(notify.begin(), notify.end(), c);
            if (iter != notify.end()) {
                notify.erase(iter);
            }
        }

        if (c->getSocketDescriptor() != INVALID_SOCKET &&
            !c->isRegisteredInLibevent()) {
            /* The socket may have been shut down while we're looping */
            /* in delayed shutdown */
            c->registerEvent();
        }

        for (const auto& pair : io.second) {
            if (pair.first) {
                pair.first->setAiostat(pair.second);
                pair.first->setEwouldblock(false);
            }
        }

        /*
         * We don't want the thread to keep on serving all of the data
         * from the context of the notification pipe, so just let it
         * run one time to set up the correct mask in libevent
         */
        c->setNumEvents(1);
        run_event_loop(c, EV_READ | EV_WRITE);
    }

    // Notify the connections we haven't notified yet
    for (auto c : notify) {
        c->setNumEvents(1);
        run_event_loop(c, EV_READ | EV_WRITE);
    }

    if (memcached_shutdown) {
        // Someone requested memcached to shut down. If we don't have
        // any connections bound to this thread we can just shut down
        time_t now = time(nullptr);
        bool log = now > me.shutdown_next_log;
        if (log) {
            me.shutdown_next_log = now + 5;
        }

        auto connected = signal_idle_clients(me, log);
        if (connected == 0) {
            LOG_INFO("Stopping worker thread {}", me.index);
            event_base_loopbreak(me.base);
        } else if (log) {
            LOG_INFO("Waiting for {} connected clients on worker thread {}",
                     connected,
                     me.index);
        }
    }
}

void notify_io_complete(gsl::not_null<const void*> void_cookie,
                        ENGINE_ERROR_CODE status) {
    auto* ccookie = reinterpret_cast<const Cookie*>(void_cookie.get());
    auto& cookie = const_cast<Cookie&>(*ccookie);

    auto& thr = cookie.getConnection().getThread();
    LOG_DEBUG("notify_io_complete: Got notify from {}, status {}",
              cookie.getConnection().getId(),
              status);

    /* kick the thread in the butt */
    if (add_conn_to_pending_io_list(&cookie.getConnection(), &cookie, status)) {
        notify_thread(thr);
    }
}

/* Which thread we assigned a connection to most recently. */
static size_t last_thread = 0;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, or because of an incoming connection.
 */
void dispatch_conn_new(SOCKET sfd, SharedListeningPort& interface) {
    size_t tid = (last_thread + 1) % Settings::instance().getNumWorkerThreads();
    auto& thread = threads[tid];
    last_thread = tid;

    try {
        thread.new_conn_queue.push(sfd, interface);
    } catch (const std::bad_alloc& e) {
        LOG_WARNING("dispatch_conn_new: Failed to dispatch new connection: {}",
                    e.what());

        if (interface->system) {
            --stats.system_conns;
        }
        safe_close(sfd);
        return ;
    }

    notify_thread(thread);
}

/*
 * Returns true if this is the thread that listens for new TCP connections.
 */
int is_listen_thread() {
    return dispatcher_thread.thread_id == cb_thread_self();
}

void notify_dispatcher() {
    if (dispatcher_thread.running) {
        notify_thread(dispatcher_thread);
    }
}

/******************************* GLOBAL STATS ******************************/

void threadlocal_stats_reset(std::vector<thread_stats>& thread_stats) {
    for (auto& ii : thread_stats) {
        ii.reset();
    }
}

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 * main_base Event base for main thread
 */
void thread_init(size_t nthr,
                 struct event_base* main_base,
                 void (*dispatcher_callback)(evutil_socket_t, short, void*)) {
    scheduler_info.resize(nthr);

    try {
        threads = std::vector<FrontEndThread>(nthr);
    } catch (const std::bad_alloc&) {
        FATAL_ERROR(EXIT_FAILURE, "Can't allocate thread descriptors");
    }

    setup_dispatcher(main_base, dispatcher_callback);

    for (size_t ii = 0; ii < nthr; ii++) {
        threads[ii].index = ii;
        setup_thread(threads[ii]);
    }

    /* Create threads after we've done all the libevent setup. */
    for (auto& thread : threads) {
        const std::string name = "mc:worker_" + std::to_string(thread.index);
        create_worker(
                worker_libevent, &thread, &thread.thread_id, name.c_str());
    }

    // Wait for all the threads to set themselves up before returning.
    std::unique_lock<std::mutex> lock(init_mutex);
    init_cond.wait(lock, [&nthr] { return !(init_count < nthr); });
}

void threads_shutdown() {
    // Notify all of the threads and let them shut down
    for (auto& thread : threads) {
        notify_thread(thread);
    }

    // Wait for all of them to complete
    for (auto& thread : threads) {
        // When using bufferevents we need to run a few iterations here.
        // Calling signalIfIdle won't run the event immediately, but when
        // the control goes back to libevent. That means that some of the
        // connections could be "stuck" for another round in the event loop.
        while (thread.running) {
            notify_thread(thread);
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        cb_join_thread(thread.thread_id);
    }
}

void threads_cleanup() {
    for (auto& thread : threads) {
        event_base_free(thread.base);
    }
}

FrontEndThread::~FrontEndThread() {
}

void notify_thread(FrontEndThread& thread) {
    event_active(&thread.notify_event, 0, 0);
}

int add_conn_to_pending_io_list(Connection* c,
                                Cookie* cookie,
                                ENGINE_ERROR_CODE status) {
    auto& thread = c->getThread();

    std::lock_guard<std::mutex> lock(thread.pending_io.mutex);
    auto iter = thread.pending_io.map.find(c);
    if (iter == thread.pending_io.map.end()) {
        thread.pending_io.map.emplace(
                c,
                std::vector<std::pair<Cookie*, ENGINE_ERROR_CODE>>{
                        {cookie, status}});
        return 1;
    }

    for (const auto& pair : iter->second) {
        if (pair.first == cookie) {
            // we've already got a pending notification for this
            // cookie.. Ignore it
            return 0;
        }
    }
    iter->second.emplace_back(cookie, status);
    return 1;
}
