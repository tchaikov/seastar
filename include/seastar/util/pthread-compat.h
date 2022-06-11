#pragma once

#include <mach/thread_act.h>
#include <mach/thread_policy.h>

using cpu_set_t = thread_affinity_policy_data_t;

void CPU_ZERO(cpu_set_t* policy);
void CPU_SET(int cpu_id, cpu_set_t* policy);
bool CPU_ISSET(int cpu_id, cpu_set_t* policy);

#define CPU_SETSIZE 128

int CPU_COUNT(cpu_set_t* policy);

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t* policy);
int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t* policy);
