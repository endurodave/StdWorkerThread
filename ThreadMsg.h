#ifndef _THREAD_MSG_H
#define _THREAD_MSG_H

#include <memory>

struct UserData;

/// @brief Message priority levels for the worker thread queue.
enum class Priority { LOW = 0, NORMAL = 1, HIGH = 2 };

/// @brief A thread message container with priority support.
class ThreadMsg
{
public:
    ThreadMsg(int id, std::shared_ptr<UserData> data, Priority priority = Priority::NORMAL)
        : m_id(id), m_data(data), m_priority(priority) {}

    int GetId() const { return m_id; }
    std::shared_ptr<UserData> GetData() const { return m_data; }
    Priority GetPriority() const { return m_priority; }

private:
    int m_id;
    std::shared_ptr<UserData> m_data;
    Priority m_priority;
};

#endif
