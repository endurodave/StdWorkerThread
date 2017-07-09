#include "WorkerThread.h"
#include "Fault.h"
#include <iostream>

using namespace std;

// Worker thread instances
WorkerThread workerThread1("WorkerThread1");
WorkerThread workerThread2("WorkerThread2");

//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
int main(void)
{	
	// Create worker threads
	workerThread1.CreateThread();
	workerThread2.CreateThread();

	// Create message to send to worker thread 1
	UserData* userData1 = new UserData();
	userData1->msg = "Hello world";
	userData1->year = 2017;

	// Post the message to worker thread 1
	workerThread1.PostMsg(userData1);

	// Create message to send to worker thread 2
	UserData* userData2 = new UserData();
	userData2->msg = "Goodbye world";
	userData2->year = 2017;

	// Post the message to worker thread 2
	workerThread2.PostMsg(userData2);

	// Give time for messages processing on worker threads
	this_thread::sleep_for(1s);

	workerThread1.ExitThread();
	workerThread2.ExitThread();

	return 0;
}

