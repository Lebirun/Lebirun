#ifndef LAUNCH_USER_H
#define LAUNCH_USER_H

#include <kernel/task.h>

task_t* launch_user_binary(const uint8_t *bin_start, const uint8_t *bin_end);

#endif
