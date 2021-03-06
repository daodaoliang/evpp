#include "evpp/inner_pre.h"

#include "evpp/libevent_headers.h"
#include "evpp/libevent_watcher.h"
#include "evpp/event_loop.h"
#include "evpp/invoke_timer.h"

namespace evpp {
EventLoop::EventLoop()
    : create_evbase_myself_(true), notified_(false), pending_functor_count_(0) {
#if LIBEVENT_VERSION_NUMBER >= 0x02001500
    struct event_config* cfg = event_config_new();
    if (cfg) {
        // Does not cache time to get a preciser timer
        event_config_set_flag(cfg, EVENT_BASE_FLAG_NO_CACHE_TIME);
        evbase_ = event_base_new_with_config(cfg);
        event_config_free(cfg);
    }
#else
    evbase_ = event_base_new();
#endif
    Init();
}

EventLoop::EventLoop(struct event_base* base)
    : evbase_(base), create_evbase_myself_(false), notified_(false), pending_functor_count_(0) {

    Init();

    // When we build an EventLoop object from an existing event_base object
    // we won't call EventLoop::Run method.
    // So we need to do the initialization work of watcher_ here.
    InitEventWatcher();
}

void EventLoop::InitEventWatcher() {
    watcher_.reset(new PipeEventWatcher(this, std::bind(&EventLoop::DoPendingFunctors, this)));
    int rc = watcher_->Init();
    assert(rc);
    rc = rc && watcher_->AsyncWait();
    assert(rc);
    if (!rc) {
        LOG_FATAL << "PipeEventWatcher init failed.";
    }
}

EventLoop::~EventLoop() {
    if (!create_evbase_myself_) {
        assert(watcher_);
        watcher_.reset();
    }
    assert(!watcher_.get());

    if (evbase_ != nullptr && create_evbase_myself_) {
        event_base_free(evbase_);
        evbase_ = nullptr;
    }

    delete pending_functors_;
    pending_functors_ = nullptr;
}

void EventLoop::Init() {
#ifdef H_HAVE_BOOST
    const size_t kPendingFunctorCount = 1024 * 16;
    this->pending_functors_ = new boost::lockfree::queue<Functor*>(kPendingFunctorCount);
#elif defined(H_HAVE_CAMERON314_CONCURRENTQUEUE)
    this->pending_functors_ = new moodycamel::ConcurrentQueue<Functor>();
#else
    this->pending_functors_ = new std::vector<Functor>();
#endif

    running_ = false;
    tid_ = std::this_thread::get_id(); // The default thread id
}

void EventLoop::Run() {
    tid_ = std::this_thread::get_id(); // The actual thread id

    // Initialize it in the EventLoop thread
    InitEventWatcher();

    // After everything have initialized, mark this running_ flag to true
    running_ = true;

    int rc = event_base_dispatch(evbase_);
    if (rc == 1) {
        LOG_ERROR << "event_base_dispatch error: no event registered";
    } else if (rc == -1) {
        int serrno = errno;
        LOG_ERROR << "event_base_dispatch error " << serrno << " " << strerror(serrno);
    }

    // Make sure watcher_ does construct,initialize and destruct in the same thread.
    watcher_.reset();
    running_ = false;
    LOG_TRACE << "this=" << this << " EventLoop stopped, tid=" << std::this_thread::get_id();
}


void EventLoop::Stop() {
    assert(running_);
    QueueInLoop(std::bind(&EventLoop::StopInLoop, this));
}

void EventLoop::StopInLoop() {
    LOG_TRACE << "this=" << this << " EventLoop is stopping now, tid=" << std::this_thread::get_id();
    assert(running_);

    auto f = [this]() {
        for (int i = 0;;i++) {
            LOG_INFO << "this=" << this << " calling DoPendingFunctors index=" << i;
            DoPendingFunctors();
            if (IsPendingQueueEmpty()) {
                break;
            }
        }
    };

    LOG_INFO << "this=" << this << " before event_base_loopexit, we invoke DoPendingFunctors";

    f();

    LOG_INFO << "this=" << this << " start event_base_loopexit";
#ifdef H_BENCHMARK_TESTING
    event_base_loopexit(evbase_, NULL);
#else
    timeval tv = Duration(0.005).TimeVal(); // Trick : delay 0.005 second
    event_base_loopexit(evbase_, &tv);
#endif
    LOG_INFO << "this=" << this << " after event_base_loopexit, we invoke DoPendingFunctors";

    f();

    running_ = false;
    LOG_INFO << "this=" << this << " end of StopInLoop";
}

void EventLoop::AfterFork() {
    int rc = event_reinit(evbase_);
    assert(rc == 0);

    if (rc != 0) {
        LOG_FATAL << "event_reinit failed!";
        abort();
    }
}

InvokeTimerPtr EventLoop::RunAfter(double delay_ms, const Functor& f) {
    return RunAfter(Duration(delay_ms / 1000.0), f);
}

InvokeTimerPtr EventLoop::RunAfter(Duration delay, const Functor& f) {
    std::shared_ptr<InvokeTimer> t = InvokeTimer::Create(this, delay, f, false);
    t->Start();
    return t;
}

evpp::InvokeTimerPtr EventLoop::RunEvery(Duration interval, const Functor& f) {
    std::shared_ptr<InvokeTimer> t = InvokeTimer::Create(this, interval, f, true);
    t->Start();
    return t;
}

void EventLoop::RunInLoop(const Functor& functor) {
    assert(IsRunning());
    if (IsInLoopThread()) {
        functor();
    } else {
        QueueInLoop(functor);
    }
}

void EventLoop::QueueInLoop(const Functor& cb) {
    LOG_TRACE << "this=" << this << " QueueInLoop. pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();
    {
#ifdef H_HAVE_BOOST
        auto f = new Functor(cb);
        while (!pending_functors_->push(f)) {
        }
#elif defined(H_HAVE_CAMERON314_CONCURRENTQUEUE)
        while (!pending_functors_->enqueue(cb)) {
        }
#else
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_->emplace_back(cb);
#endif
    }
    ++pending_functor_count_;
    LOG_TRACE << "this=" << this << " QueueInLoop, queued a new Functor. pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();
    if (!notified_.load()) {
        LOG_TRACE << "this=" << this << " QueueInLoop call watcher_->Nofity() notified_.store(true)";

        // We must set notified_ to true before calling `watcher_->Nodify()`
        // otherwise there is a change that:
        //  1. We called watcher_- > Nodify() on thread1
        //  2. On thread2 we watched this event, so wakeup the CPU changed to run this EventLoop on thread2 and executed all the pending task
        //  3. Then the CPU changed to run on thread1 and set notified_ to true
        //  4. Then, some thread except thread2 call this QueueInLoop to push a task into the queue, and find notified_ is true, so there is no change to wakeup thread2 to execute this task
        notified_.store(true);
        watcher_->Notify();
    } else {
         LOG_TRACE << "this=" << this << " No need to call watcher_->Nofity()";
    }
}

void EventLoop::QueueInLoop(Functor&& cb) {
    LOG_TRACE << "this=" << this << " QueueInLoop. pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();
    {
#ifdef H_HAVE_BOOST
        auto f = new Functor(std::move(cb)); // TODO Add test code for it
        while (!pending_functors_->push(f)) {
        }
#elif defined(H_HAVE_CAMERON314_CONCURRENTQUEUE)
        while (!pending_functors_->enqueue(std::move(cb))) {
        }
#else
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_->emplace_back(std::move(cb));
#endif
    }
    ++pending_functor_count_;
    LOG_TRACE << "this=" << this << " QueueInLoop, queued a new Functor. pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();
    if (!notified_.load()) {
        LOG_TRACE << "this=" << this << " QueueInLoop call watcher_->Nofity() notified_.store(true)";
        notified_.store(true);
        watcher_->Notify();
    } else {
        LOG_TRACE << "this=" << this << " No need to call watcher_->Nofity()";
    }
}

InvokeTimerPtr EventLoop::RunAfter(double delay_ms, Functor&& f) {
    return RunAfter(Duration(delay_ms / 1000.0), std::move(f));
}

InvokeTimerPtr EventLoop::RunAfter(Duration delay, Functor&& f) {
    std::shared_ptr<InvokeTimer> t = InvokeTimer::Create(this, delay, std::move(f), false);
    t->Start();
    return t;
}

evpp::InvokeTimerPtr EventLoop::RunEvery(Duration interval, Functor&& f) {
    std::shared_ptr<InvokeTimer> t = InvokeTimer::Create(this, interval, std::move(f), true);
    t->Start();
    return t;
}

void EventLoop::RunInLoop(Functor&& functor) {
    if (IsInLoopThread()) {
        functor();
    } else {
        QueueInLoop(std::move(functor));
    }
}

void EventLoop::DoPendingFunctors() {
    LOG_TRACE << "this=" << this << " DoPendingFunctors pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();

#ifdef H_HAVE_BOOST
    notified_.store(false);
    Functor* f = nullptr;
    while (pending_functors_->pop(f)) {
        (*f)();
        delete f;
        --pending_functor_count_;
    }
#elif defined(H_HAVE_CAMERON314_CONCURRENTQUEUE)
    notified_.store(false);
    Functor f;
    while (pending_functors_->try_dequeue(f)) {
        f();
        --pending_functor_count_;
    }
#else
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notified_.store(false);
        pending_functors_->swap(functors);
        LOG_TRACE << "this=" << this << " DoPendingFunctors pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();
    }
    LOG_TRACE << "this=" << this << " DoPendingFunctors pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();
    for (size_t i = 0; i < functors.size(); ++i) {
        functors[i]();
        --pending_functor_count_;
    }
    LOG_TRACE << "this=" << this << " DoPendingFunctors pending_functor_count_=" << pending_functor_count_ << " PendingQueueSize=" << GetPendingQueueSize() << " notified_=" << notified_.load();
#endif
}

size_t EventLoop::GetPendingQueueSize() {
#ifdef H_HAVE_BOOST
    return static_cast<size_t>(pending_functor_count_.load());
#elif defined(H_HAVE_CAMERON314_CONCURRENTQUEUE)
    return pending_functors_->size_approx();
#else
    return pending_functors_->size();
#endif
}

bool EventLoop::IsPendingQueueEmpty() {
#ifdef H_HAVE_BOOST
    return pending_functors_->empty();
#elif defined(H_HAVE_CAMERON314_CONCURRENTQUEUE)
    return pending_functors_->size_approx() == 0;
#else
    return pending_functors_->empty();
#endif
}

void EventLoop::AssertInLoopThread() const {
    assert(IsInLoopThread());
}
}
