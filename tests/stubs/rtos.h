#pragma once
#include <stdint.h>
using UBaseType_t = uint32_t;
constexpr UBaseType_t TASK_PRIO_LOW = 1, TASK_PRIO_HIGH = 3;
inline UBaseType_t mock_task_priority = TASK_PRIO_LOW;
inline unsigned mock_priority_changes;
inline UBaseType_t uxTaskPriorityGet(void *) { return mock_task_priority; }
inline void vTaskPrioritySet(void *, UBaseType_t value)
{
	mock_task_priority = value; ++mock_priority_changes;
}
