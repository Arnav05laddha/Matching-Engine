#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include <iostream>

namespace hyperengine
{
    namespace concurrency
    {

        inline bool pinThreadToCore(int coreId)
        {
#ifdef _WIN32
            HANDLE thread = GetCurrentThread();
            DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << coreId);
            if (SetThreadAffinityMask(thread, mask) == 0)
            {
                std::cerr << "Failed to pin thread to core " << coreId << "\n";
                return false;
            }
            return true;
#else
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(coreId, &cpuset);

            pthread_t current_thread = pthread_self();
            int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

            if (result != 0)
            {
                std::cerr << "Failed to pin thread to core " << coreId << "\n";
                return false;
            }
            return true;
#endif
        }

    } // namespace concurrency
} // namespace hyperengine
