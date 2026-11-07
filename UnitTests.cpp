// Unit tests for Thread: priority order, FullPolicy (BLOCK / DROP), ExitThread()
// draining, and the watchdog. Adapted from the DelegateMQ StdlibThread tests
// (test/unit-tests/DelegateThreadsTests.cpp) that apply to this class.
//
// Each test uses a local Thread with a message handler (SetMsgHandler()) and
// checks results after ExitThread(), which drains the queue and joins, so no
// test reads state the worker thread may still be writing.

#include "Thread.h"
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>

using namespace std;
using namespace std::chrono;

static int s_failures = 0;

// Record a failure and continue, so one failure doesn't hide the rest
#define CHECK(cond) \
    do { if (!(cond)) { cout << "  FAILED: " #cond " (" __FILE__ ":" << __LINE__ << ")" << endl; s_failures++; } } while (0)

static shared_ptr<UserData> Msg(const string& text)
{
    auto data = make_shared<UserData>();
    data->msg = text;
    data->year = 2026;
    return data;
}

// Holds the worker inside a "block" message until Release(), so messages
// posted meanwhile are all still queued.
struct Blocker
{
    atomic<bool> running{ false };
    atomic<bool> release{ false };

    // Call from the message handler. Returns true if this was the blocker.
    bool Handle(const UserData& data)
    {
        if (data.msg != "block")
            return false;
        running = true;
        while (!release)
            this_thread::sleep_for(milliseconds(1));
        return true;
    }

    // Post the blocker and wait until the worker is inside it
    void Start(Thread& thread)
    {
        thread.PostMsg(Msg("block"));
        while (!running)
            this_thread::sleep_for(milliseconds(1));
    }

    // Release from another thread after `ms`, e.g. while ExitThread() joins
    thread ReleaseAfter(int ms)
    {
        return thread([this, ms]() { this_thread::sleep_for(milliseconds(ms)); release = true; });
    }
};

//------------------------------------------------------------------------------
// Priority
//------------------------------------------------------------------------------

// HIGH before NORMAL before LOW, and posting order within a level
static void Priority_OrderAcrossAndWithinLevels()
{
    Thread thread("PriorityThread");
    Blocker blocker;
    vector<string> order;   // written only on the worker thread
    thread.SetMsgHandler([&](shared_ptr<UserData> d) { if (!blocker.Handle(*d)) order.push_back(d->msg); });
    thread.CreateThread();

    blocker.Start(thread);
    thread.PostMsg(Msg("L1"), Priority::LOW);
    thread.PostMsg(Msg("N1"), Priority::NORMAL);
    thread.PostMsg(Msg("H1"), Priority::HIGH);
    thread.PostMsg(Msg("L2"), Priority::LOW);
    thread.PostMsg(Msg("N2"), Priority::NORMAL);
    thread.PostMsg(Msg("H2"), Priority::HIGH);
    CHECK(thread.GetQueueSize() == 6);
    blocker.release = true;
    thread.ExitThread();

    CHECK((order == vector<string>{ "H1", "H2", "N1", "N2", "L1", "L2" }));
}

//------------------------------------------------------------------------------
// FullPolicy
//------------------------------------------------------------------------------

// DROP: flooding a full queue drops messages, and the producer never stalls
static void FullPolicy_Drop_DropsWhenFull()
{
    Thread thread("DropThread", 3, FullPolicy::DROP);
    atomic<int> delivered{ 0 };
    thread.SetMsgHandler([&](shared_ptr<UserData>) { this_thread::sleep_for(milliseconds(50)); delivered++; });
    thread.CreateThread();

    auto start = steady_clock::now();
    for (int i = 0; i < 10; i++)
        thread.PostMsg(Msg("drop"));
    auto elapsed = steady_clock::now() - start;

    // Posting 10 must take well under one 50ms message: the producer never blocked
    CHECK(elapsed < milliseconds(30));

    thread.ExitThread();
    CHECK(delivered >= 3);  // the queue held at least 3
    CHECK(delivered < 10);  // and the rest were dropped
}

// DROP: if the queue never fills, every message is delivered
static void FullPolicy_Drop_DeliversAllWhenBelowLimit()
{
    Thread thread("DropBelowLimitThread", 20, FullPolicy::DROP);
    atomic<int> delivered{ 0 };
    thread.SetMsgHandler([&](shared_ptr<UserData>) { delivered++; });
    thread.CreateThread();

    for (int i = 0; i < 10; i++)
        thread.PostMsg(Msg("msg"));

    thread.ExitThread();
    CHECK(delivered == 10);
}

// BLOCK (default): a flooded queue delivers every message by stalling the producer
static void FullPolicy_Block_DeliversAllAndStallsProducer()
{
    Thread thread("BlockThread", 2);
    atomic<int> delivered{ 0 };
    thread.SetMsgHandler([&](shared_ptr<UserData>) { this_thread::sleep_for(milliseconds(20)); delivered++; });
    thread.CreateThread();

    auto start = steady_clock::now();
    for (int i = 0; i < 10; i++)
        thread.PostMsg(Msg("block policy"));
    auto elapsed = steady_clock::now() - start;

    // With room for 2, the producer waited for at least ~7 of the 20ms messages
    CHECK(elapsed >= milliseconds(100));

    thread.ExitThread();
    CHECK(delivered == 10);
}

// maxQueueSize 0: no limit, a burst is all delivered
static void FullPolicy_Unlimited_DeliversAll()
{
    Thread thread("UnlimitedThread");
    atomic<int> delivered{ 0 };
    Blocker blocker;
    thread.SetMsgHandler([&](shared_ptr<UserData> d) { if (!blocker.Handle(*d)) delivered++; });
    thread.CreateThread();

    blocker.Start(thread);
    for (int i = 0; i < 500; i++)
        thread.PostMsg(Msg("burst"));
    CHECK(thread.GetQueueSize() == 500);
    blocker.release = true;

    thread.ExitThread();
    CHECK(delivered == 500);
}

// BLOCK: ExitThread() releases a producer blocked on a full queue; its message is discarded
static void FullPolicy_Block_ExitReleasesBlockedProducer()
{
    Thread thread("BlockExitThread", 1);
    atomic<int> delivered{ 0 };
    Blocker blocker;
    thread.SetMsgHandler([&](shared_ptr<UserData> d) { if (!blocker.Handle(*d)) delivered++; });
    thread.CreateThread();

    blocker.Start(thread);
    thread.PostMsg(Msg("fills the queue"));

    atomic<bool> producerDone{ false };
    std::thread producer([&]() { thread.PostMsg(Msg("blocked")); producerDone = true; });
    this_thread::sleep_for(milliseconds(50));
    CHECK(!producerDone);   // still waiting for space

    auto releaser = blocker.ReleaseAfter(50);
    thread.ExitThread();
    producer.join();
    releaser.join();

    CHECK(producerDone);
    CHECK(delivered == 1);  // the queued message ran; the blocked one was discarded
}

//------------------------------------------------------------------------------
// ExitThread
//------------------------------------------------------------------------------

// Every message queued before ExitThread() runs, at any priority
static void Exit_DrainsQueuedMessages()
{
    Thread thread("ExitDrainThread");
    atomic<int> delivered{ 0 };
    Blocker blocker;
    thread.SetMsgHandler([&](shared_ptr<UserData> d) { if (!blocker.Handle(*d)) delivered++; });
    thread.CreateThread();

    blocker.Start(thread);
    const Priority priorities[] = { Priority::LOW, Priority::NORMAL, Priority::HIGH };
    for (int i = 0; i < 9; i++)
        thread.PostMsg(Msg("queued"), priorities[i % 3]);

    // ExitThread() joins, so release the blocker from another thread
    auto releaser = blocker.ReleaseAfter(20);
    thread.ExitThread();
    releaser.join();

    CHECK(delivered == 9);
    CHECK(thread.GetProcessedCount() == 10);    // the blocker, plus 9 queued
}

// PostMsg() after ExitThread() is discarded; CreateThread() again works
static void Exit_PostAfterExitDiscardedAndRecreate()
{
    Thread thread("RecreateThread");
    atomic<int> delivered{ 0 };
    thread.SetMsgHandler([&](shared_ptr<UserData>) { delivered++; });
    thread.CreateThread();
    thread.ExitThread();

    thread.PostMsg(Msg("after exit"));
    CHECK(thread.GetQueueSize() == 0);

    thread.CreateThread();
    thread.PostMsg(Msg("after recreate"));
    thread.ExitThread();
    CHECK(delivered == 1);
}

// IsCurrentThread() is true only on the worker
static void Thread_IsCurrentThread()
{
    Thread thread("CurrentThread");
    atomic<bool> onWorker{ false };
    thread.SetMsgHandler([&](shared_ptr<UserData>) { onWorker = thread.IsCurrentThread(); });
    thread.CreateThread();
    CHECK(!thread.IsCurrentThread());
    thread.PostMsg(Msg("where am I"));
    thread.ExitThread();
    CHECK(onWorker);
    CHECK(!thread.IsCurrentThread());
}

//------------------------------------------------------------------------------
// Watchdog
//------------------------------------------------------------------------------

// A watched thread that never receives a message still refreshes its heartbeat
// (the timeout is set before the thread starts, so its first queue wait is timed)
static void Watchdog_IdleThreadNotReported()
{
    Thread thread("WatchdogIdleThread");
    thread.CreateThread(milliseconds(100));
    this_thread::sleep_for(milliseconds(400));
    thread.ExitThread();
    CHECK(thread.GetWatchdogAlarmCount() == 0);
}

// After ExitThread(), a Thread object that still exists is no longer checked
static void Watchdog_ExitedThreadNotReported()
{
    Thread thread("WatchdogExitedThread");
    thread.CreateThread(milliseconds(100));
    thread.ExitThread();
    this_thread::sleep_for(milliseconds(400));
    CHECK(thread.GetWatchdogAlarmCount() == 0);
}

// Re-created without a timeout, a thread drops the watchdog it had before
static void Watchdog_RecreatedWithoutTimeoutNotReported()
{
    Thread thread("WatchdogRecreatedThread");
    thread.CreateThread(milliseconds(100));
    thread.ExitThread();
    thread.CreateThread();
    this_thread::sleep_for(milliseconds(400));
    thread.ExitThread();
    CHECK(thread.GetWatchdogAlarmCount() == 0);
}

// A handler that runs past the timeout is reported
static void Watchdog_StalledThreadReported()
{
    Thread thread("WatchdogStalledThread");
    thread.SetMsgHandler([](shared_ptr<UserData>) { this_thread::sleep_for(milliseconds(400)); });
    thread.CreateThread(milliseconds(100));
    cout << "  (an \"unresponsive\" watchdog message is expected here)" << endl;
    thread.PostMsg(Msg("stall"));
    thread.ExitThread();
    CHECK(thread.GetWatchdogAlarmCount() > 0);
}

//------------------------------------------------------------------------------
// RunUnitTests
//------------------------------------------------------------------------------
int RunUnitTests()
{
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        { "Priority_OrderAcrossAndWithinLevels",            Priority_OrderAcrossAndWithinLevels },
        { "FullPolicy_Drop_DropsWhenFull",                  FullPolicy_Drop_DropsWhenFull },
        { "FullPolicy_Drop_DeliversAllWhenBelowLimit",      FullPolicy_Drop_DeliversAllWhenBelowLimit },
        { "FullPolicy_Block_DeliversAllAndStallsProducer",  FullPolicy_Block_DeliversAllAndStallsProducer },
        { "FullPolicy_Unlimited_DeliversAll",               FullPolicy_Unlimited_DeliversAll },
        { "FullPolicy_Block_ExitReleasesBlockedProducer",   FullPolicy_Block_ExitReleasesBlockedProducer },
        { "Exit_DrainsQueuedMessages",                      Exit_DrainsQueuedMessages },
        { "Exit_PostAfterExitDiscardedAndRecreate",         Exit_PostAfterExitDiscardedAndRecreate },
        { "Thread_IsCurrentThread",                         Thread_IsCurrentThread },
        { "Watchdog_IdleThreadNotReported",                 Watchdog_IdleThreadNotReported },
        { "Watchdog_ExitedThreadNotReported",               Watchdog_ExitedThreadNotReported },
        { "Watchdog_RecreatedWithoutTimeoutNotReported",    Watchdog_RecreatedWithoutTimeoutNotReported },
        { "Watchdog_StalledThreadReported",                 Watchdog_StalledThreadReported },
    };

    cout << "\n-- Unit tests --" << endl;
    s_failures = 0;
    for (const auto& test : tests)
    {
        int before = s_failures;
        test.fn();
        cout << (s_failures == before ? "PASS " : "FAIL ") << test.name << endl;
    }
    cout << (s_failures == 0 ? "All unit tests passed." : "Unit tests FAILED.") << endl;
    return s_failures;
}
