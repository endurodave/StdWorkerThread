# C++ std::thread Event Loop with Message Queue and Timer
Create a worker thread with an event loop, message queue and a timer using the C++11 thread support library.

Originally published on CodeProject at: <a href="http://www.codeproject.com/Articles/1169105/Cplusplus-std-thread-Event-Loop-with-Message-Queue"><strong>C++ std::thread Event Loop with Message Queue and Timer</strong></a>

<h2>Introduction</h2>

<p>An event loop, or sometimes called a message loop, is a thread that waits for and dispatches incoming events. The thread blocks waiting for requests to arrive and then dispatches the event to an event handler function. A message queue is typically used by the loop to hold incoming messages. Each message is sequentially dequeued, decoded, and then an action is performed. Event loops are one way to implement inter-process communication.</p>

<p>All operating systems provide support for multi-threaded applications. Each OS has unique function calls for creating threads, message queues and timers. With the advent of the C++11 thread support library, it&rsquo;s now possible to create portable code and avoid the OS-specific function calls. This article provides a simple example of how to create a thread event loop, message queue and timer services while only relying upon the C++ Standard Library. Any C++11 compiler supporting the thread library should be able to compile the attached source.</p>

<h2>Background</h2>

<p>Typically, I need a thread to operate as an event loop. Incoming messages are dequeued by the thread and data is dispatched to an appropriate function handler based on a unique message identifier. Timer support capable of invoking a function is handy for low speed polling or to generate a timeout if something doesn&rsquo;t happen in the expected amount of time. Many times, the worker thread is created at startup and isn&rsquo;t destroyed until the application terminates.</p>

<p>A key requirement for the implementation is that the incoming messages must execute on the same thread instance. Whereas say <code>std::async </code>may use a temporary thread from a pool, this class ensures that all incoming messages use the same thread. For instance, a subsystem could be implemented with code that is not thread-safe. A single <code>WorkerThread </code>instance is used to safely dispatch function calls into the subsystem.</p>

<p>At first glance, the C++ thread support seems to be missing some key features. Yes, <code>std::thread </code>is available to spin off a thread but there is no thread-safe queue and no timers &ndash; services that most OS&rsquo;s provide. I&rsquo;ll show how to use the C++ Standard Library to create these &ldquo;missing&rdquo; features and provide an event processing loop familiar to many programmers.</p>

<h2>WorkerThread</h2>

<p>The <code>WorkerThread </code>class encapsulates all the necessary event loop mechanisms. A simple class interface allows thread creation, posting messages to the event loop, and eventual thread termination. The interface is shown below:</p>

<pre lang="C++">
class WorkerThread
{
public:
    /// Constructor
    WorkerThread(const char* threadName);

    /// Destructor
    ~WorkerThread();

    /// Called once to create the worker thread
    /// @return TRUE if thread is created. FALSE otherwise.
    bool CreateThread();

    /// Called once a program exit to exit the worker thread
    void ExitThread();

    /// Get the ID of this thread instance
    /// @return The worker thread ID
    std::thread::id GetThreadId();

    /// Get the ID of the currently executing thread
    /// @return The current thread ID
    static std::thread::id GetCurrentThreadId();

    /// Add a message to thread queue.
    /// @param[in] data - thread specific information created on the heap using operator new.
    void PostMsg(const UserData* data);

private:
    WorkerThread(const WorkerThread&amp;);
    WorkerThread&amp; operator=(const WorkerThread&amp;);

    /// Entry point for the worker thread
    void Process();

    /// Entry point for timer thread
    void TimerThread();

    std::thread* m_thread;
    std::queue&lt;ThreadMsg*&gt; m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic&lt;bool&gt; m_timerExit;
    const char* THREAD_NAME;
};</pre>

<p>The first thing to notice is that <code>std::thread </code>is used to create a main worker thread. The main worker thread function is <code>Process()</code>.</p>

<pre lang="C++">
bool WorkerThread::CreateThread()
{
    if (!m_thread)
        m_thread = new thread(&amp;WorkerThread::Process, this);
    return true;
}</pre>

<h2>Event Loop</h2>

<p>The <code>Process() </code>event loop is shown below. The thread relies upon a <code>std::queue&lt;ThreadMsg*&gt; </code>for the message queue. <code>std::queue </code>is not thread-safe so all access to the queue must be protected by mutex. A <code>std::condition_variable </code>is used to suspend the thread until notified that a new message has been added to the queue.</p>

<pre lang="C++">
void WorkerThread::Process()
{
    m_timerExit = false;
    std::thread timerThread(&amp;WorkerThread::TimerThread, this);

    while (1)
    {
        ThreadMsg* msg = 0;
        {
            // Wait for a message to be added to the queue
            std::unique_lock&lt;std::mutex&gt; lk(m_mutex);
            while (m_queue.empty())
                m_cv.wait(lk);

            if (m_queue.empty())
                continue;

            msg = m_queue.front();
            m_queue.pop();
        }

        switch (msg-&gt;id)
        {
            case MSG_POST_USER_DATA:
            {
                ASSERT_TRUE(msg-&gt;msg != NULL);

                // Convert the ThreadMsg void* data back to a UserData*
                const UserData* userData = static_cast&lt;const UserData*&gt;(msg-&gt;msg);

                cout &lt;&lt; userData-&gt;msg.c_str() &lt;&lt; &quot; &quot; &lt;&lt; userData-&gt;year &lt;&lt; &quot; on &quot; &lt;&lt; THREAD_NAME &lt;&lt; endl;

                // Delete dynamic data passed through message queue
                delete userData;
                delete msg;
                break;
            }

            case MSG_TIMER:
                cout &lt;&lt; &quot;Timer expired on &quot; &lt;&lt; THREAD_NAME &lt;&lt; endl;
                delete msg;
                break;

            case MSG_EXIT_THREAD:
            {
                m_timerExit = true;
                timerThread.join();

                delete msg;
                std::unique_lock&lt;std::mutex&gt; lk(m_mutex);
                while (!m_queue.empty())
                {
                    msg = m_queue.front();
                    m_queue.pop();
                    delete msg;
                }

                cout &lt;&lt; &quot;Exit thread on &quot; &lt;&lt; THREAD_NAME &lt;&lt; endl;
                return;
            }

            default:
                ASSERT();
        }
    }
}</pre>

<p><code>PostMsg() </code>creates a new <code>ThreadMsg </code>on the heap, adds the message to the queue, and then notifies the worker thread using a condition variable.</p>

<pre lang="C++">
void WorkerThread::PostMsg(const UserData* data)
{
    ASSERT_TRUE(m_thread);

    ThreadMsg* threadMsg = new ThreadMsg(MSG_POST_USER_DATA, data);

    // Add user data msg to queue and notify worker thread
    std::unique_lock&lt;std::mutex&gt; lk(m_mutex);
    m_queue.push(threadMsg);
    m_cv.notify_one();
}</pre>

<p>The loop will continue to process messages until the <code>MSG_EXIT_THREAD </code>is received and the thread exits.</p>

<pre lang="C++">
void WorkerThread::ExitThread()
{
    if (!m_thread)
        return;

    // Create a new ThreadMsg
    ThreadMsg* threadMsg = new ThreadMsg(MSG_EXIT_THREAD, 0);

    // Put exit thread message into the queue
    {
        lock_guard&lt;mutex&gt; lock(m_mutex);
        m_queue.push(threadMsg);
        m_cv.notify_one();
    }

    m_thread-&gt;join();
    delete m_thread;
    m_thread = 0;
}</pre>

<h3>Event Loop (Win32)</h3>

<p>The code snippet below contrasts the <code>std::thread </code>event loop above with a similar Win32 version using the Windows API. Notice <code>GetMessage() </code>API is used in lieu of the <code>std::queue</code>. Messages are posted to the OS message queue using <code>PostThreadMessage()</code>. And finally, <code>timerSetEvent() </code>is used to place <code>WM_USER_TIMER </code>messages into the queue. All of these services are provided by the OS. The <code>std::thread WorkerThread </code>implementation presented here avoids the raw OS calls yet the implementation functionality is the same as the Win32 version while relying only upon only the C++ Standard Library.</p>

<pre lang="C++">
unsigned long WorkerThread::Process(void* parameter)
{
    MSG msg;
    BOOL bRet;

    // Start periodic timer
    MMRESULT timerId = timeSetEvent(250, 10, &amp;WorkerThread::TimerExpired, 
                       reinterpret_cast&lt;DWORD&gt;(this), TIME_PERIODIC);

    while ((bRet = GetMessage(&amp;msg, NULL, WM_USER_BEGIN, WM_USER_END)) != 0)
    {
        switch (msg.message)
        {
            case WM_DISPATCH_DELEGATE:
            {
                ASSERT_TRUE(msg.wParam != NULL);

                // Convert the ThreadMsg void* data back to a UserData*
                const UserData* userData = static_cast&lt;const UserData*&gt;(msg.wParam);

                cout &lt;&lt; userData-&gt;msg.c_str() &lt;&lt; &quot; &quot; &lt;&lt; userData-&gt;year &lt;&lt; &quot; on &quot; &lt;&lt; THREAD_NAME &lt;&lt; endl;

                // Delete dynamic data passed through message queue
                delete userData;
                break;
            }

            case WM_USER_TIMER:
                cout &lt;&lt; &quot;Timer expired on &quot; &lt;&lt; THREAD_NAME &lt;&lt; endl;
                break;

            case WM_EXIT_THREAD:
                timeKillEvent(timerId);
                return 0;

            default:
                ASSERT();
        }
    }
    return 0;
}</pre>

<h2>Timer</h2>

<p>A low-resolution periodic timer message is inserted into the queue using a secondary private thread. The timer thread is created inside <code>Process()</code>.</p>

<pre lang="C++">
void WorkerThread::Process()
{
    m_timerExit = false;
    std::thread timerThread(&amp;WorkerThread::TimerThread, this);

...</pre>

<p>The timer thread&rsquo;s sole responsibility is to insert a <code>MSG_TIMER </code>message every 250ms. In this implementation, there&rsquo;s no protection against the timer thread injecting more than one timer message into the queue. This could happen if the worker thread falls behind and can&rsquo;t service the message queue fast enough. Depending on the worker thread, processing load, and how fast the timer messages are inserted, additional logic could be employed to prevent flooding the queue.</p>

<pre lang="C++">
void WorkerThread::TimerThread()
{
    while (!m_timerExit)
    {
        // Sleep for 250ms then put a MSG_TIMER message into queue
        std::this_thread::sleep_for(250ms);

        ThreadMsg* threadMsg = new ThreadMsg(MSG_TIMER, 0);

        // Add timer msg to queue and notify worker thread
        std::unique_lock&lt;std::mutex&gt; lk(m_mutex);
        m_queue.push(threadMsg);
        m_cv.notify_one();
    }
}</pre>

<h2>Usage</h2>

<p>The <code>main()</code> function below shows how to use the <code>WorkerThread </code>class. Two worker threads are created and a message is posted to each one. After a short delay, both threads exit.</p>

<pre lang="C++">
// Worker thread instances
WorkerThread workerThread1(&quot;WorkerThread1&quot;);
WorkerThread workerThread2(&quot;WorkerThread2&quot;);

int main(void)
{  
    // Create worker threads
    workerThread1.CreateThread();
    workerThread2.CreateThread();

    // Create message to send to worker thread 1
    UserData* userData1 = new UserData();
    userData1-&gt;msg = &quot;Hello world&quot;;
    userData1-&gt;year = 2017;

    // Post the message to worker thread 1
    workerThread1.PostMsg(userData1);

    // Create message to send to worker thread 2
    UserData* userData2 = new UserData();
    userData2-&gt;msg = &quot;Goodbye world&quot;;
    userData2-&gt;year = 2017;

    // Post the message to worker thread 2
    workerThread2.PostMsg(userData2);

    // Give time for messages processing on worker threads
    this_thread::sleep_for(1s);

    workerThread1.ExitThread();
    workerThread2.ExitThread();

    return 0;
}</pre>

<h2>Conclusion</h2>

<p>The C++ thread support library offers a platform independent way to write multi-threaded application code without reliance upon OS-specific API&rsquo;s. The <code>WorkerThread </code>class presented here is a bare-bones implementation of an event loop, yet all the basics are there ready to be expanded upon.</p>



