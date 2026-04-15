#include "Thread.h"
#include "Fault.h"
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#endif
#ifdef __linux__
#include <pthread.h>
#endif

using namespace std;
using namespace std::chrono;

#define MSG_POST_USER_DATA  1
#define MSG_EXIT_THREAD     2

static steady_clock::time_point GetNow()
{
    return steady_clock::now();
}

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------
Thread::Thread(const std::string& threadName, size_t maxQueueSize, FullPolicy fullPolicy)
    : m_thread(std::nullopt)
    , m_exit(false)
    , THREAD_NAME(threadName)
    , MAX_QUEUE_SIZE(maxQueueSize)
    , FULL_POLICY(fullPolicy)
    , m_watchdogExit(false)
    , m_lastAliveTime(steady_clock::time_point{})
    , m_watchdogTimeout(steady_clock::duration::zero())
{
}

//----------------------------------------------------------------------------
// ~Thread
//----------------------------------------------------------------------------
Thread::~Thread()
{
    ExitThread();
}

//----------------------------------------------------------------------------
// CreateThread
//----------------------------------------------------------------------------
bool Thread::CreateThread(std::optional<std::chrono::milliseconds> watchdogTimeout)
{
    if (!m_thread)
    {
        m_threadStartPromise.emplace();
        m_threadStartFuture.emplace(m_threadStartPromise->get_future());
        m_exit = false;

        m_thread.emplace(&Thread::Process, this);

        SetThreadName(m_thread->native_handle(), THREAD_NAME);

        // Wait until the worker thread has entered Process()
        m_threadStartFuture->get();

        m_lastAliveTime.store(GetNow());

        if (watchdogTimeout.has_value())
        {
            m_watchdogTimeout.store(watchdogTimeout.value());
            m_watchdogExit.store(false);
            m_watchdogThread.emplace(&Thread::WatchdogProcess, this);
        }
    }
    return true;
}

//----------------------------------------------------------------------------
// ExitThread
//----------------------------------------------------------------------------
void Thread::ExitThread()
{
    if (!m_thread)
        return;

    // Stop watchdog first so it doesn't fire during shutdown
    if (m_watchdogThread)
    {
        {
            lock_guard<mutex> lk(m_watchdogMutex);
            m_watchdogExit.store(true);
        }
        m_watchdogCv.notify_one();
        m_watchdogThread->join();
        m_watchdogThread.reset();
    }

    // Push high-priority exit message, bypassing MAX_QUEUE_SIZE limit
    auto exitMsg = make_shared<ThreadMsg>(MSG_EXIT_THREAD, nullptr, Priority::HIGH);
    {
        lock_guard<mutex> lock(m_mutex);
        m_exit.store(true);
        m_queue.push(exitMsg);
        m_cv.notify_one();
        m_cvNotFull.notify_all(); // unblock any blocked producers
    }

    if (m_thread->joinable())
    {
        if (std::this_thread::get_id() != m_thread->get_id())
            m_thread->join();
        else
            m_thread->detach(); // called from within the thread itself
    }

    {
        lock_guard<mutex> lock(m_mutex);
        m_thread.reset();
        while (!m_queue.empty())
            m_queue.pop();
        m_cvNotFull.notify_all();
    }
}

//----------------------------------------------------------------------------
// GetThreadId
//----------------------------------------------------------------------------
std::thread::id Thread::GetThreadId()
{
    ASSERT_TRUE(m_thread.has_value());
    return m_thread->get_id();
}

//----------------------------------------------------------------------------
// GetCurrentThreadId
//----------------------------------------------------------------------------
std::thread::id Thread::GetCurrentThreadId()
{
    return this_thread::get_id();
}

//----------------------------------------------------------------------------
// IsCurrentThread
//----------------------------------------------------------------------------
bool Thread::IsCurrentThread()
{
    if (!m_thread.has_value())
        return false;
    return GetThreadId() == GetCurrentThreadId();
}

//----------------------------------------------------------------------------
// GetQueueSize
//----------------------------------------------------------------------------
size_t Thread::GetQueueSize()
{
    lock_guard<mutex> lock(m_mutex);
    return m_queue.size();
}

//----------------------------------------------------------------------------
// SetThreadName
//----------------------------------------------------------------------------
void Thread::SetThreadName(std::thread::native_handle_type handle, const std::string& name)
{
#ifdef _WIN32
    wstring wstr(name.begin(), name.end());
    SetThreadDescription(handle, wstr.c_str());
#elif defined(__linux__)
    pthread_setname_np(handle, name.substr(0, 15).c_str());
#endif
}

//----------------------------------------------------------------------------
// PostMsg
//----------------------------------------------------------------------------
void Thread::PostMsg(std::shared_ptr<UserData> data, Priority priority)
{
    if (m_exit.load())
        return;
    ASSERT_TRUE(m_thread.has_value());

    unique_lock<mutex> lk(m_mutex);

    // [BACK PRESSURE / DROP LOGIC]
    if (MAX_QUEUE_SIZE > 0 && m_queue.size() >= MAX_QUEUE_SIZE)
    {
        if (FULL_POLICY == FullPolicy::DROP)
            return;  // silently discard — caller is not stalled

        // BLOCK: wait until the consumer drains a slot or the thread exits
        m_cvNotFull.wait(lk, [this]() {
            return m_queue.size() < MAX_QUEUE_SIZE || m_exit.load();
        });
    }

    if (m_exit.load())
        return;

    auto threadMsg = make_shared<ThreadMsg>(MSG_POST_USER_DATA, data, priority);
    m_queue.push(threadMsg);
    m_cv.notify_one();
}

//----------------------------------------------------------------------------
// WatchdogProcess
// Runs on a dedicated watchdog thread. Wakes up every timeout/2 and checks
// whether the worker thread has completed a loop iteration recently.
//----------------------------------------------------------------------------
void Thread::WatchdogProcess()
{
    auto timeout      = m_watchdogTimeout.load();
    auto sleepInterval = timeout / 2;

    while (true)
    {
        unique_lock<mutex> lk(m_watchdogMutex);
        m_watchdogCv.wait_for(lk, sleepInterval);

        if (m_watchdogExit.load())
            break;

        auto delta = GetNow() - m_lastAliveTime.load();
        if (delta > timeout)
        {
            cerr << "Watchdog: thread '" << THREAD_NAME
                 << "' is unresponsive!" << endl;
            // Optionally trigger a hard fault: ASSERT();
        }
    }
}

//----------------------------------------------------------------------------
// Process
// The worker thread event loop. Runs for the lifetime of the thread,
// blocking when there is nothing to do and waking up to process one message
// at a time in priority order (HIGH before NORMAL before LOW).
//
// Watchdog interaction
// --------------------
// m_lastAliveTime is written at the top of every loop iteration. The
// watchdog thread reads this timestamp and raises an alert if it has not
// been refreshed within the configured timeout — indicating the thread is
// stuck inside a message handler (deadlock, infinite loop, etc.).
//
// When a watchdog is active the blocking wait is a timed wait_for() rather
// than an indefinite wait(). The timeout is set to timeout/4 so the loop
// cycles at least four times per watchdog period even when the queue is
// empty, keeping m_lastAliveTime fresh. Without this, an idle thread would
// look identical to a stalled one from the watchdog's perspective.
//
// Back pressure interaction
// -------------------------
// After dequeuing a message, m_cvNotFull is signalled. This wakes any
// producer that blocked in PostMsg() because the queue was full, allowing
// it to enqueue its next message now that a slot is free.
//----------------------------------------------------------------------------
void Thread::Process()
{
    // Unblock CreateThread(), which is waiting on this promise to confirm
    // the thread is running before it returns to the caller.
    m_threadStartPromise->set_value();

    while (1)
    {
        // --- Watchdog heartbeat -------------------------------------------
        // Record the current time before blocking. This is the timestamp the
        // watchdog checks. As long as the loop keeps cycling (either woken by
        // a message or by the timed heartbeat below), this stays current and
        // the watchdog stays silent.
        m_lastAliveTime.store(GetNow());

        // --- Wait for a message -------------------------------------------
        shared_ptr<ThreadMsg> msg;
        {
            unique_lock<mutex> lk(m_mutex);

            if (m_watchdogTimeout.load() > steady_clock::duration::zero())
            {
                // Watchdog is active: use a timed wait so the loop wakes
                // periodically even when the queue is empty. Without this,
                // m_lastAliveTime would never be refreshed while idle and the
                // watchdog would incorrectly report the thread as unresponsive.
                auto heartbeat = m_watchdogTimeout.load() / 4;
                m_cv.wait_for(lk, heartbeat, [this]() {
                    return !m_queue.empty() || m_exit.load();
                });
            }
            else
            {
                // No watchdog: block indefinitely until a message arrives or
                // ExitThread() sets m_exit and notifies.
                m_cv.wait(lk, [this]() {
                    return !m_queue.empty() || m_exit.load();
                });
            }

            if (m_queue.empty())
            {
                // Woken with no message — either the watchdog heartbeat fired
                // (loop back to refresh m_lastAliveTime) or ExitThread() was
                // called with nothing left in the queue (exit the loop).
                if (m_exit.load()) return;
                continue;
            }

            // Dequeue the highest-priority waiting message.
            // std::priority_queue::top() returns the greatest element per the
            // ThreadMsgComparator, i.e. HIGH > NORMAL > LOW.
            msg = m_queue.top();
            m_queue.pop();

            // --- Back pressure: notify a blocked producer -----------------
            // A producer in PostMsg() may be sleeping on m_cvNotFull because
            // the queue was at MAX_QUEUE_SIZE. Removing one message opens a
            // slot, so wake one waiting producer.
            if (MAX_QUEUE_SIZE > 0)
                m_cvNotFull.notify_one();
        }

        // --- Dispatch -------------------------------------------------------
        switch (msg->GetId())
        {
            case MSG_POST_USER_DATA:
            {
                ASSERT_TRUE(msg->GetData());
                auto userData = msg->GetData();
                cout << userData->msg << " " << userData->year
                     << " on " << THREAD_NAME << endl;
                break;
            }

            case MSG_EXIT_THREAD:
                return;

            default:
                ASSERT();
        }
    }
}
