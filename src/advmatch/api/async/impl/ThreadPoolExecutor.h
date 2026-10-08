#ifndef ADVMATCH_THREAD_POOL_EXECUTOR_H
#define ADVMATCH_THREAD_POOL_EXECUTOR_H

#include "../Executor.h"

#include <cstddef>

namespace advmatch::api::async
{
    class ThreadPoolExecutor final : public Executor
    {
        struct Impl;
        std::unique_ptr<Impl> impl;

    public:
        explicit ThreadPoolExecutor(std::size_t threads = 2);
        ~ThreadPoolExecutor() override;
        ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
        ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;

        void execute(std::function<void()> work) override;
        void shutdown() noexcept;
    };
}

#endif // ADVMATCH_THREAD_POOL_EXECUTOR_H
