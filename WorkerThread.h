#ifndef _THREAD_STD_H
#define _THREAD_STD_H

#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>

struct UserData
{
	std::string msg;
	int year;
};

struct ThreadMsg;

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
	WorkerThread(const WorkerThread&);
	WorkerThread& operator=(const WorkerThread&);

	/// Entry point for the worker thread
	void Process();

	/// Entry point for timer thread
	void TimerThread();

	std::thread* m_thread;
	std::queue<ThreadMsg*> m_queue;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::atomic<bool> m_timerExit;
	const char* THREAD_NAME;
};

#endif 

