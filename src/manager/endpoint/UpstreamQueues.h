#ifndef WINJECT_MANAGER_UPSTREAM_QUEUES_H_
#define WINJECT_MANAGER_UPSTREAM_QUEUES_H_

#include "utils/RadioQueues.h"

namespace winject
{
using RadioRxQueue = ReactiveLossyRxQueue;
using RadioTxQueue = BoundedLossyTxQueue;
}  // namespace winject
#endif  // WINJECT_MANAGER_UPSTREAM_QUEUES_H_
