#include "Thread.h"
#include <iostream>
#include <chrono>

// @see https://github.com/endurodave/StdWorkerThread
// David Lafreniere

// -----------------------------------------------------------------------------
// This example demonstrates a C++17 worker thread implementation built on
// std::thread with three key features over a basic event loop:
//
//   1. Priority Queue
//      Messages are held in a std::priority_queue rather than a plain
//      std::queue. When multiple messages are queued simultaneously the worker
//      thread always dequeues the highest-priority one first (HIGH > NORMAL >
//      LOW), making it straightforward to give urgent work preferential access
//      to the thread without a separate fast-path queue.
//
//   2. Back Pressure
//      An optional maxQueueSize cap limits how many messages may sit in the
//      queue at once. When the queue is full, PostMsg() blocks the calling
//      thread until the worker drains at least one message. This prevents a
//      fast producer from outrunning a slow consumer and exhausting memory.
//
//   3. Watchdog
//      An optional timeout launches a lightweight watchdog thread alongside
//      the worker. The worker updates a "last alive" timestamp on every loop
//      iteration. If the timestamp goes stale — because the thread is stuck
//      inside a handler (deadlock, infinite loop, etc.) — the watchdog logs
//      an error. In a production system the watchdog callback can trigger a
//      controlled shutdown or system reset instead.
// -----------------------------------------------------------------------------

using namespace std;

// Worker thread instances:
//   workerThread1 — unlimited queue, used for the priority demo.
//   workerThread2 — max 5 queued messages, used for the back pressure demo.
//   workerThread3 — watchdog enabled, used for the watchdog demo.
Thread workerThread1("WorkerThread1");
Thread workerThread2("WorkerThread2", 5);
Thread workerThread3("WorkerThread3");

//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
int main(void)
{
    // Start all three worker threads. Each thread signals via std::promise that
    // it is fully running before CreateThread() returns, so it is safe to post
    // messages immediately after this block.
    workerThread1.CreateThread();
    workerThread2.CreateThread();
    workerThread3.CreateThread(std::chrono::milliseconds(2000)); // 2 s watchdog

    // -------------------------------------------------------------------------
    // Priority queue demo (WorkerThread1)
    //
    // Three messages are posted in LOW -> NORMAL -> HIGH order. Because all
    // three are enqueued before the worker has a chance to drain them, the
    // priority queue reorders them so HIGH is processed first, then NORMAL,
    // then LOW — regardless of the order they were posted.
    // -------------------------------------------------------------------------
    cout << "\n-- Priority queue demo (WorkerThread1) --" << endl;

    auto lowData = make_shared<UserData>();
    lowData->msg  = "Low priority msg";
    lowData->year = 2026;
    workerThread1.PostMsg(lowData, Priority::LOW);

    auto normalData = make_shared<UserData>();
    normalData->msg  = "Normal priority msg";
    normalData->year = 2026;
    workerThread1.PostMsg(normalData, Priority::NORMAL);

    auto highData = make_shared<UserData>();
    highData->msg  = "High priority msg";
    highData->year = 2026;
    workerThread1.PostMsg(highData, Priority::HIGH);

    // -------------------------------------------------------------------------
    // Back pressure demo (WorkerThread2, maxQueueSize = 5)
    //
    // Ten messages are posted to a thread whose queue holds at most 5. Once
    // the queue is full PostMsg() blocks the main thread until the worker
    // processes a message and frees a slot. This cooperative throttling keeps
    // memory use bounded without dropping any messages.
    // -------------------------------------------------------------------------
    cout << "\n-- Back pressure demo (WorkerThread2, maxQueueSize=5) --" << endl;

    for (int i = 1; i <= 10; i++)
    {
        auto data  = make_shared<UserData>();
        data->msg  = "Back pressure message #" + to_string(i);
        data->year = 2026;
        workerThread2.PostMsg(data); // blocks the caller when the queue is full
    }

    // -------------------------------------------------------------------------
    // Watchdog demo (WorkerThread3, timeout = 2 000 ms)
    //
    // The thread is healthy here so the watchdog never fires. To observe a
    // watchdog alert, add a long sleep or spin loop inside the MSG_POST_USER_DATA
    // case in Thread.cpp — the watchdog will print an error after 2 seconds.
    // -------------------------------------------------------------------------
    cout << "\n-- Watchdog demo (WorkerThread3, watchdog=2000ms) --" << endl;

    auto wdData = make_shared<UserData>();
    wdData->msg  = "Watchdog-monitored message";
    wdData->year = 2026;
    workerThread3.PostMsg(wdData);

    // Give worker threads time to finish processing before shutting down.
    this_thread::sleep_for(chrono::milliseconds(500));

    workerThread1.ExitThread();
    workerThread2.ExitThread();
    workerThread3.ExitThread();

    return 0;
}
