#include <sys/time.h>
#include <seastar/util/timer-compat.h>


int timer_create(clockid_t clockid, struct sigevent* sev, timer_t* timerid)
{
    return 0;
}

struct timeval timeval_from_timespec(const timespec& its)
{
  return {its.tv_sec, static_cast<int>(its.tv_nsec / 1000)};
}

int timer_settime(timer_t timerid,
                  int /* flags */,
                  const itimerspec* new_value,
                  itimerspec* /* old_value */)
{
  struct itimerval itv = {
    // timer interval
    timeval_from_timespec(new_value->it_interval),
    // current value
    timeval_from_timespec(new_value->it_value)
  };
  return setitimer(ITIMER_PROF, &itv, nullptr);
}

int timer_delete(timer_t timerid)
{
  struct itimerval itv {};
  return setitimer(ITIMER_PROF, &itv, nullptr);
}
