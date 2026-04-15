#ifndef _THREAD_STD_H
#define _THREAD_STD_H

/// @file Thread.h
/// @see https://github.com/endurodave/StdWorkerThread
/// David Lafreniere, 2026.
///
/// @brief Standard C++ worker thread with event loop, priority queue,
///        back pressure, and optional watchdog using only C++17 std library.
///
/// @details
/// Key features:
/// * **Priority Queue:** Uses std::priority_queue so high-priority messages
///   are processed before lower-priority ones.
/// * **Back Pressure:** Configurable maxQueueSize. When the queue is full,
///   PostMsg() blocks the caller until space is available.
/// * **Watchdog:** Optional timeout detects a stalled thread (deadlock or
///   infinite loop inside message processing).
/// * **Synchronized Start:** std::promise/std::future ensures the thread is
///   fully running before CreateThread() returns.
/// * **Debug Support:** Sets the native thread name on Windows and Linux.

#include "ThreadMsg.h"
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <string>
#include <future>
#include <optional>
#include <chrono>

struct UserData
{
    std::string msg;
    int year;
};

// Comparator: highest Priority value is processed first
struct ThreadMsgComparator {
    bool operator()(const std::shared_ptr<ThreadMsg>& a,
                    const std::shared_ptr<ThreadMsg>& b) const {
        return static_cast<int>(a->GetPriority()) < static_cast<int>(b->GetPriority());
    }
};

/// @brief Cross-platform worker thread for any system supporting C++17 std::thread.
class Thread
{
public:
    /// Constructor.
    /// @param threadName  Thread name shown in debugger.
    /// @param maxQueueSize  Max queued messages before back pressure kicks in
    ///                      (0 = unlimited).
    Thread(const std::string& threadName, size_t maxQueueSize = 0);

    /// Destructor — calls ExitThread() if not already stopped.
    ~Thread();

    /// Create and start the worker thread.
    /// @param watchdogTimeout  If set, a watchdog detects when the thread loop
    ///                         has not completed an iteration within this duration.
    /// @return True if the thread was created successfully.
    bool CreateThread(std::optional<std::chrono::milliseconds> watchdogTimeout = std::nullopt);

    /// Shut down the worker thread.  Safe to call from any thread.
    void ExitThread();

    /// Get the ID of this thread instance.
    std::thread::id GetThreadId();

    /// Get the ID of the currently executing thread.
    static std::thread::id GetCurrentThreadId();

    /// Returns true if the calling thread is this thread instance.
    bool IsCurrentThread();

    /// Get thread name.
    std::string GetThreadName() { return THREAD_NAME; }

    /// Get the current number of messages in the queue.
    size_t GetQueueSize();

    /// Post a message to the worker thread.
    /// If back pressure is enabled and the queue is full, this call blocks
    /// until space is available or ExitThread() is called.
    /// @param msg       Message data.
    /// @param priority  Processing priority (default: NORMAL).
    void PostMsg(std::shared_ptr<UserData> msg, Priority priority = Priority::NORMAL);

private:
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    /// Worker thread entry point.
    void Process();

    /// Watchdog thread entry point.
    void WatchdogProcess();

    void SetThreadName(std::thread::native_handle_type handle, const std::string& name);

    std::optional<std::thread> m_thread;
    std::atomic<bool> m_exit;

    std::priority_queue<
        std::shared_ptr<ThreadMsg>,
        std::vector<std::shared_ptr<ThreadMsg>>,
        ThreadMsgComparator> m_queue;

    std::mutex m_mutex;
    std::condition_variable m_cv;       // notifies consumer of new messages
    std::condition_variable m_cvNotFull; // notifies producers when space opens up

    const std::string THREAD_NAME;
    const size_t MAX_QUEUE_SIZE;

    std::optional<std::promise<void>> m_threadStartPromise;
    std::optional<std::future<void>>  m_threadStartFuture;

    // Watchdog
    std::optional<std::thread>                             m_watchdogThread;
    std::mutex                                             m_watchdogMutex;
    std::condition_variable                                m_watchdogCv;
    std::atomic<bool>                                      m_watchdogExit;
    std::atomic<std::chrono::steady_clock::time_point>     m_lastAliveTime;
    std::atomic<std::chrono::steady_clock::duration>       m_watchdogTimeout;
};

#endif
