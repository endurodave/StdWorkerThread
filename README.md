![License MIT](https://img.shields.io/github/license/BehaviorTree/BehaviorTree.CPP?color=blue)
[![conan Ubuntu](https://github.com/endurodave/StdWorkerThread/actions/workflows/cmake_ubuntu.yml/badge.svg)](https://github.com/endurodave/StdWorkerThread/actions/workflows/cmake_ubuntu.yml)
[![conan Ubuntu](https://github.com/endurodave/StdWorkerThread/actions/workflows/cmake_clang.yml/badge.svg)](https://github.com/endurodave/StdWorkerThread/actions/workflows/cmake_clang.yml)
[![conan Windows](https://github.com/endurodave/StdWorkerThread/actions/workflows/cmake_windows.yml/badge.svg)](https://github.com/endurodave/StdWorkerThread/actions/workflows/cmake_windows.yml)

# C++ std::thread Event Loop

A worker thread with an event loop, priority message queue, back pressure, and watchdog using the C++17 standard library.

# Table of Contents

- [C++ std::thread Event Loop](#c-stdthread-event-loop)
- [Table of Contents](#table-of-contents)
- [Getting Started](#getting-started)
- [Introduction](#introduction)
- [Background](#background)
- [Thread Class](#thread-class)
- [Event Loop](#event-loop)
- [Priority Queue](#priority-queue)
- [Back Pressure](#back-pressure)
- [Watchdog](#watchdog)
- [Usage](#usage)
- [Portable C/C++ Libraries](#portable-cc-libraries)
- [References](#references)
- [Conclusion](#conclusion)

# Getting Started

[CMake](https://cmake.org/) is used to create the project build files on any Windows or Linux machine. The source code works on any C++17 compiler with `std::thread` support.

1. Clone the repository.
2. From the repository root, run the following CMake command:
   `cmake -B Build .`
3. Build and run the project within the `Build` directory.

# Introduction

An event loop, or sometimes called a message loop, is a thread that waits for and dispatches incoming events. The thread blocks waiting for requests to arrive and then dispatches the event to an event handler function. A message queue is typically used by the loop to hold incoming messages. Each message is sequentially dequeued, decoded, and then an action is performed. Event loops are one way to implement inter-process communication.

All operating systems provide support for multi-threaded applications. Each OS has unique function calls for creating threads, message queues, and timers. With the advent of the C++11 thread support library, it is now possible to create portable code and avoid the OS-specific function calls. This article provides an example of how to create a thread event loop and message queue while relying only upon the C++ standard library. Any C++17 compiler supporting the thread library should be able to compile the attached source.

# Background

Typically, a thread needs to operate as an event loop. Incoming messages are dequeued by the thread and data is dispatched to an appropriate function handler based on a unique message identifier. Many times, the worker thread is created at startup and is not destroyed until the application terminates.

A key requirement for the implementation is that incoming messages must execute on the same thread instance. Whereas `std::async` may use a temporary thread from a pool, this class ensures that all incoming messages use the same thread. For instance, a subsystem could be implemented with code that is not thread-safe. A single `Thread` instance is used to safely dispatch function calls into the subsystem.

At first glance, the C++ thread support seems to be missing some key features. Yes, `std::thread` is available to spin off a thread but there is no thread-safe queue and no built-in back pressure or watchdog — services that many OS primitives provide. The sections below show how to use the C++ standard library to create these features and provide an event processing loop familiar to many programmers.

# Thread Class

The `Thread` class encapsulates all the necessary event loop mechanisms. A simple interface allows thread creation, posting messages to the event loop, and eventual thread termination.

```cpp
class Thread
{
public:
    Thread(const std::string& threadName, size_t maxQueueSize = 0);
    ~Thread();

    bool CreateThread(std::optional<std::chrono::milliseconds> watchdogTimeout = std::nullopt);
    void ExitThread();

    std::thread::id GetThreadId();
    static std::thread::id GetCurrentThreadId();
    bool IsCurrentThread();

    std::string GetThreadName();
    size_t GetQueueSize();

    void PostMsg(std::shared_ptr<UserData> msg, Priority priority = Priority::NORMAL);
};
```

`Thread` uses `std::optional<std::thread>` internally, which allows the thread handle to be cleanly reset between `CreateThread()` and `ExitThread()` calls without requiring heap allocation.

`CreateThread()` uses `std::promise` and `std::future` to guarantee the worker thread has entered its event loop before returning, so callers can safely post messages immediately after creation.

# Event Loop

`Process()` is the worker thread's event loop. It runs for the lifetime of the thread, blocking when there is nothing to do and waking to process one message at a time in priority order.

The loop has three distinct responsibilities that interact with each other, explained below.

**1. Watchdog heartbeat**

`m_lastAliveTime` is written at the top of every iteration before the thread blocks. This is the timestamp the watchdog checks. As long as the loop keeps cycling, this stays current and the watchdog stays silent. If the thread gets stuck inside a message handler and stops reaching the top of the loop, the timestamp goes stale and the watchdog fires.

**2. Blocking wait — with or without a heartbeat timeout**

When no watchdog is configured the thread blocks indefinitely with `m_cv.wait()` until a message arrives or `ExitThread()` wakes it. When a watchdog is active the wait is replaced with `m_cv.wait_for()` using a heartbeat interval of `timeout / 4`. This causes the loop to wake periodically even when the queue is empty, so `m_lastAliveTime` stays fresh. Without this, an idle thread would look identical to a stalled one from the watchdog's perspective.

When the timed wait expires with an empty queue, the loop simply continues back to the top to refresh the timestamp and wait again.

**3. Dequeue and back pressure release**

Messages are dequeued with `m_queue.top()` / `m_queue.pop()`, which returns the highest-priority waiting message (`HIGH` before `NORMAL` before `LOW`). After removing a message, `m_cvNotFull` is signalled to wake any producer that blocked in `PostMsg()` because the queue was at its `MAX_QUEUE_SIZE` limit.

```cpp
void Thread::Process()
{
    // Unblock CreateThread(), which waits on this promise before returning.
    m_threadStartPromise->set_value();

    while (1)
    {
        // Refresh watchdog timestamp before blocking.
        m_lastAliveTime.store(GetNow());

        shared_ptr<ThreadMsg> msg;
        {
            unique_lock<mutex> lk(m_mutex);

            if (m_watchdogTimeout.load() > steady_clock::duration::zero())
            {
                // Timed wait: wake periodically to keep m_lastAliveTime
                // current even when the queue is empty.
                auto heartbeat = m_watchdogTimeout.load() / 4;
                m_cv.wait_for(lk, heartbeat, [this]() {
                    return !m_queue.empty() || m_exit.load();
                });
            }
            else
            {
                // No watchdog: block until a message arrives or exit is set.
                m_cv.wait(lk, [this]() {
                    return !m_queue.empty() || m_exit.load();
                });
            }

            if (m_queue.empty())
            {
                // Either the heartbeat fired (loop back, refresh timestamp)
                // or ExitThread() was called with nothing left (exit).
                if (m_exit.load()) return;
                continue;
            }

            // Dequeue highest-priority message (HIGH > NORMAL > LOW).
            msg = m_queue.top();
            m_queue.pop();

            // Wake a producer blocked on a full queue.
            if (MAX_QUEUE_SIZE > 0)
                m_cvNotFull.notify_one();
        }

        switch (msg->GetId())
        {
            case MSG_POST_USER_DATA:
            {
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
```

`PostMsg()` adds a message to the queue and wakes the consumer:

```cpp
void Thread::PostMsg(std::shared_ptr<UserData> data, Priority priority)
{
    auto threadMsg = make_shared<ThreadMsg>(MSG_POST_USER_DATA, data, priority);

    unique_lock<mutex> lk(m_mutex);

    if (MAX_QUEUE_SIZE > 0)
    {
        m_cvNotFull.wait(lk, [this]() {
            return m_queue.size() < MAX_QUEUE_SIZE || m_exit.load();
        });
    }

    m_queue.push(threadMsg);
    m_cv.notify_one();
}
```

`ExitThread()` pushes a `MSG_EXIT_THREAD` message at `Priority::HIGH`, sets the exit flag inside the lock, and notifies both the consumer and any blocked producers before joining the thread.

# Priority Queue

Messages are stored in a `std::priority_queue` rather than a plain `std::queue`. A custom comparator ensures messages with a higher `Priority` value are dequeued first.

```cpp
enum class Priority { LOW = 0, NORMAL = 1, HIGH = 2 };

struct ThreadMsgComparator {
    bool operator()(const std::shared_ptr<ThreadMsg>& a,
                    const std::shared_ptr<ThreadMsg>& b) const {
        return static_cast<int>(a->GetPriority()) < static_cast<int>(b->GetPriority());
    }
};

std::priority_queue<
    std::shared_ptr<ThreadMsg>,
    std::vector<std::shared_ptr<ThreadMsg>>,
    ThreadMsgComparator> m_queue;
```

When several messages are in the queue simultaneously, the worker thread always processes the `HIGH` priority message first, then `NORMAL`, then `LOW`, regardless of the order they were posted. This is useful for giving urgent work — such as a shutdown or error signal — preferential access to the thread without a separate fast-path queue.

# Back Pressure

An optional `maxQueueSize` constructor argument caps how many messages may sit in the queue at once. When the queue is full, `PostMsg()` blocks the calling thread on a second condition variable (`m_cvNotFull`) until the worker drains at least one message and signals that space is available.

```cpp
Thread workerThread("MyThread", 5); // back pressure kicks in at 5 queued messages
```

This cooperative throttling prevents a fast producer from outrunning a slow consumer and exhausting memory. When no limit is needed, pass `0` (the default) and `PostMsg()` never blocks.

During shutdown `ExitThread()` sets the exit flag inside the lock and calls `m_cvNotFull.notify_all()`, ensuring any thread blocked in `PostMsg()` unblocks immediately rather than waiting for queue space that will never arrive.

# Watchdog

Passing a timeout to `CreateThread()` enables a lightweight watchdog that runs on a dedicated thread alongside the worker.

```cpp
workerThread.CreateThread(std::chrono::milliseconds(2000)); // 2-second watchdog
```

The worker thread stores a `steady_clock::time_point` (`m_lastAliveTime`) at the top of every loop iteration. The watchdog wakes every `timeout / 2` and compares the current time against that timestamp:

```cpp
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
        }
    }
}
```

If the timestamp goes stale — because the thread is stuck inside a message handler (deadlock, infinite loop, etc.) — the watchdog logs an error. In a production system the watchdog callback can be extended to trigger a controlled shutdown or system reset. The watchdog exits promptly when `ExitThread()` is called by signalling `m_watchdogCv` directly rather than waiting for the next sleep to expire.

# Usage

The `main()` function below shows how to use the `Thread` class. Three threads are created to demonstrate the priority queue, back pressure, and watchdog features respectively.

```cpp
// WorkerThread1 — unlimited queue, priority demo
// WorkerThread2 — back pressure enabled (max 5 queued messages)
// WorkerThread3 — watchdog enabled (2-second timeout)
Thread workerThread1("WorkerThread1");
Thread workerThread2("WorkerThread2", 5);
Thread workerThread3("WorkerThread3");

int main(void)
{
    workerThread1.CreateThread();
    workerThread2.CreateThread();
    workerThread3.CreateThread(std::chrono::milliseconds(2000));

    // Priority queue demo — LOW posted first, HIGH processed first
    auto lowData = make_shared<UserData>("Low priority msg", 2026);
    workerThread1.PostMsg(lowData, Priority::LOW);

    auto normalData = make_shared<UserData>("Normal priority msg", 2026);
    workerThread1.PostMsg(normalData, Priority::NORMAL);

    auto highData = make_shared<UserData>("High priority msg", 2026);
    workerThread1.PostMsg(highData, Priority::HIGH);

    // Back pressure demo — blocks main when queue is full
    for (int i = 1; i <= 10; i++)
    {
        auto data = make_shared<UserData>("Back pressure message #" + to_string(i), 2026);
        workerThread2.PostMsg(data);
    }

    // Watchdog demo — healthy thread, watchdog should not fire
    auto wdData = make_shared<UserData>("Watchdog-monitored message", 2026);
    workerThread3.PostMsg(wdData);

    this_thread::sleep_for(chrono::milliseconds(500));

    workerThread1.ExitThread();
    workerThread2.ExitThread();
    workerThread3.ExitThread();

    return 0;
}
```

# Portable C/C++ Libraries

Explore [portable-c-cpp-libs](https://github.com/endurodave/portable-c-cpp-libs) for reusable C/C++ components, including state machines, callbacks, threading, memory management, and more.

# References

* [Active Object C++ State Machine](https://github.com/DelegateMQ/active-fsm) — Active-object C++ finite state machine with async dispatch and pub/sub signals.
* [DelegateMQ](https://github.com/DelegateMQ/DelegateMQ) - The DelegateMQ C++ library can invoke any callable function synchronously, asynchronously, or on a remote endpoint.
* [Integration Test Framework](https://github.com/DelegateMQ/IntegrationTestFramework) - Integration test framework using Google Test and delegates.

# Conclusion

The C++ standard library offers a platform-independent way to write multi-threaded application code without reliance on OS-specific APIs. The `Thread` class presented here builds on the basic event loop pattern to add a priority queue for ordered message processing, back pressure to protect against memory exhaustion, and an optional watchdog to detect stalled threads — all using only the C++17 standard library.
