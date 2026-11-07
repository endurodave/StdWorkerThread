#include "Thread.h"
#include <iostream>
#include <chrono>

// @see https://github.com/endurodave/StdWorkerThread
// David Lafreniere
//
// Demonstrates a C++17 worker thread built on std::thread:
//   1. Priority queue: HIGH > NORMAL > LOW, posting order within a level.
//   2. Back pressure:  a full queue blocks (BLOCK) or discards (DROP) PostMsg().
//   3. Watchdog:       reports a thread stuck inside a message handler.
// Then runs the unit tests (UnitTests.cpp) and returns non-zero on failure.

using namespace std;
using namespace std::chrono;

int RunUnitTests();

static shared_ptr<UserData> Msg(const string& text)
{
    auto data = make_shared<UserData>();
    data->msg = text;
    data->year = 2026;
    return data;
}

int main(void)
{
    Thread priorityThread("PriorityThread");                    // unlimited queue
    Thread blockThread("BlockThread", 5);                       // max 5 queued, BLOCK
    Thread dropThread("DropThread", 5, FullPolicy::DROP);       // max 5 queued, DROP
    Thread watchdogThread("WatchdogThread");                    // watchdog enabled
    Thread timerThread("TimerThread");                          // fed by a timer

    // CreateThread() returns once the thread is running, so posting right away is safe
    priorityThread.CreateThread();
    blockThread.CreateThread();
    dropThread.CreateThread();
    watchdogThread.CreateThread(milliseconds(2000));
    timerThread.CreateThread();

    // Posted LOW, NORMAL, HIGH; usually processed HIGH, NORMAL, LOW (the worker
    // may take the first message before the others are queued)
    cout << "\n-- Priority --" << endl;
    priorityThread.PostMsg(Msg("Low priority msg"), Priority::LOW);
    priorityThread.PostMsg(Msg("Normal priority msg"), Priority::NORMAL);
    priorityThread.PostMsg(Msg("High priority msg"), Priority::HIGH);

    // 10 messages into a queue of 5: PostMsg() blocks until a slot frees up
    cout << "\n-- Back pressure (BLOCK, max 5) --" << endl;
    for (int i = 1; i <= 10; i++)
        blockThread.PostMsg(Msg("Back pressure message #" + to_string(i)));

    // 10 messages into a queue of 5: once full, PostMsg() discards and returns
    cout << "\n-- Back pressure (DROP, max 5) --" << endl;
    for (int i = 1; i <= 10; i++)
        dropThread.PostMsg(Msg("Drop message #" + to_string(i)));

    // A healthy thread: the 2s watchdog stays silent. A handler that runs past
    // the timeout is reported (see Watchdog_StalledThreadReported in UnitTests.cpp).
    cout << "\n-- Watchdog (2000ms) --" << endl;
    watchdogThread.PostMsg(Msg("Watchdog-monitored message"));

    // A std::thread "timer" posts a LOW priority message every 250ms
    cout << "\n-- Timer (250ms) --" << endl;
    atomic<bool> timerExit(false);
    thread timer([&]() {
        while (!timerExit)
        {
            this_thread::sleep_for(milliseconds(250));
            if (!timerExit)
                timerThread.PostMsg(Msg("Timer expired"), Priority::LOW);
        }
    });
    this_thread::sleep_for(milliseconds(1000));
    timerExit = true;
    timer.join();   // stop posting before ExitThread()

    // ExitThread() processes everything still queued, then joins
    priorityThread.ExitThread();
    blockThread.ExitThread();
    dropThread.ExitThread();
    watchdogThread.ExitThread();
    timerThread.ExitThread();

    return RunUnitTests() == 0 ? 0 : 1;
}
