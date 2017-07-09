#include "WorkerThread.h"
#include "Fault.h"
#include <iostream>

using namespace std;

#define MSG_EXIT_THREAD			1
#define MSG_POST_USER_DATA		2
#define MSG_TIMER				3

struct ThreadMsg
{
	ThreadMsg(int i, const void* m) { id = i; msg = m; }
	int id;
	const void* msg;
};

//----------------------------------------------------------------------------
// WorkerThread
//----------------------------------------------------------------------------
WorkerThread::WorkerThread(const char* threadName) : m_thread(0), m_timerExit(false), THREAD_NAME(threadName)
{
}

//----------------------------------------------------------------------------
// ~WorkerThread
//----------------------------------------------------------------------------
WorkerThread::~WorkerThread()
{
	ExitThread();
}

//----------------------------------------------------------------------------
// CreateThread
//----------------------------------------------------------------------------
bool WorkerThread::CreateThread()
{
	if (!m_thread)
		m_thread = new thread(&WorkerThread::Process, this);
	return true;
}

//----------------------------------------------------------------------------
// GetThreadId
//----------------------------------------------------------------------------
std::thread::id WorkerThread::GetThreadId()
{
	ASSERT_TRUE(m_thread != 0);
	return m_thread->get_id();
}

//----------------------------------------------------------------------------
// GetCurrentThreadId
//----------------------------------------------------------------------------
std::thread::id WorkerThread::GetCurrentThreadId()
{
	return this_thread::get_id();
}

//----------------------------------------------------------------------------
// ExitThread
//----------------------------------------------------------------------------
void WorkerThread::ExitThread()
{
	if (!m_thread)
		return;

	// Create a new ThreadMsg
	ThreadMsg* threadMsg = new ThreadMsg(MSG_EXIT_THREAD, 0);

	// Put exit thread message into the queue
	{
		lock_guard<mutex> lock(m_mutex);
		m_queue.push(threadMsg);
		m_cv.notify_one();
	}

	m_thread->join();
	delete m_thread;
	m_thread = 0;
}

//----------------------------------------------------------------------------
// PostMsg
//----------------------------------------------------------------------------
void WorkerThread::PostMsg(const UserData* data)
{
	ASSERT_TRUE(m_thread);

	ThreadMsg* threadMsg = new ThreadMsg(MSG_POST_USER_DATA, data);

	// Add user data msg to queue and notify worker thread
	std::unique_lock<std::mutex> lk(m_mutex);
	m_queue.push(threadMsg);
	m_cv.notify_one();
}

//----------------------------------------------------------------------------
// TimerThread
//----------------------------------------------------------------------------
void WorkerThread::TimerThread()
{
	while (!m_timerExit)
	{
		// Sleep for 250ms then put a MSG_TIMER message into queue
		std::this_thread::sleep_for(250ms);

		ThreadMsg* threadMsg = new ThreadMsg(MSG_TIMER, 0);

		// Add timer msg to queue and notify worker thread
		std::unique_lock<std::mutex> lk(m_mutex);
		m_queue.push(threadMsg);
		m_cv.notify_one();
	}
}

//----------------------------------------------------------------------------
// Process
//----------------------------------------------------------------------------
void WorkerThread::Process()
{
	m_timerExit = false;
	std::thread timerThread(&WorkerThread::TimerThread, this);

	while (1)
	{
		ThreadMsg* msg = 0;
		{
			// Wait for a message to be added to the queue
			std::unique_lock<std::mutex> lk(m_mutex);
			while (m_queue.empty())
				m_cv.wait(lk);

			if (m_queue.empty())
				continue;

			msg = m_queue.front();
			m_queue.pop();
		}

		switch (msg->id)
		{
			case MSG_POST_USER_DATA:
			{
				ASSERT_TRUE(msg->msg != NULL);

				// Convert the ThreadMsg void* data back to a UserData* 
				const UserData* userData = static_cast<const UserData*>(msg->msg);

				cout << userData->msg.c_str() << " " << userData->year << " on " << THREAD_NAME << endl;

				// Delete dynamic data passed through message queue
				delete userData;
				delete msg;
				break;
			}

			case MSG_TIMER:
				cout << "Timer expired on " << THREAD_NAME << endl;
				delete msg;
				break;

			case MSG_EXIT_THREAD:
			{
				m_timerExit = true;
				timerThread.join();

				delete msg;
				std::unique_lock<std::mutex> lk(m_mutex);
				while (!m_queue.empty())
				{
					msg = m_queue.front();
					m_queue.pop();
					delete msg;
				}

				cout << "Exit thread on " << THREAD_NAME << endl;
				return;
			}

			default:
				ASSERT();
		}
	}
}


