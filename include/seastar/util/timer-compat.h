#pragma once

#include <sys/time.h>

struct timer_t {};

struct itimerspec {
  struct timespec it_interval;
  struct timespec it_value;
};

int timer_create(clockid_t clockid, struct sigevent* sev, timer_t* timerid);
int timer_settime(timer_t timerid, int flags,
                  const itimerspec* new_value,
                  itimerspec* old_value);
int timer_delete(timer_t timerid);
