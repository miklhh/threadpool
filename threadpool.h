/*
 * A simple header only threadpool using the C++11 standard thread and 
 * synchronization primitives. It's a minimal implementation with bare minimum 
 * threadpool features. To be linked with pthread ( -lpthread ) on unix based
 * systems.
 *
 * ThreadPool(size_t workers);
 * bool     working()     const noexcept;    -- Returns true if pool is working.
 * size_t   jobs()        const noexcept;    -- Returns jobs in the queue.
 * size_t   threadCount() const noexcept;    -- Returns size of pool.
 *
 * Enqueuing tasks is done with eather of the two enqueing methods. The first
 * method is used for queuing tasks of type 'void()' and the the second 
 * templated method is used for queinig tasks with return types. The second 
 * enqueue method returns a future object associated with return value of the 
 * enqueued tasks return value.
 *
 * (1)  void enqueue(std::function<void()> task);
 * (2)  auto enqueue(T task) -> std::future<decltype(task())>;
 *
 * Author: Mikael Henriksson - www.github.com/miklhh
 * Licenced under MIT Licence.
 */

#ifndef _THREAD_POOL_H
#define _THREAD_POOL_H

// Helgrind support. These helps remove some of the false negatives when runing
// helgrind with some C++ synchronization primitives.
#ifdef RUN_WITH_HELGRIND
    #include <valgrind/drd.h>
    #define _GLIBCXX_SYNCHRONIZATION_HAPPENS_BEFORE(addr) ANNOTATE_HAPPENS_BEFORE(addr)
    #define _GLIBCXX_SYNCHRONIZATION_HAPPENS_AFTER(addr) ANNOTATE_HAPPENS_AFTER(addr)
    #define _GLIBCXX_EXTERN_TEMPLATE -1
#endif

// Includes.
#include <cstddef>
#include <condition_variable>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <queue>
#include <future>

class ThreadPool
{
public:
    explicit ThreadPool(std::size_t threads);
    ~ThreadPool();

    // Test if pool is doing any work what so ever. Returns false if the all
    // threads in the pool are considered free.
    bool working() const noexcept;

    // Returns current size of task queue.
    std::size_t jobs() const noexcept;

    // Returns the thread pool thread count.
    std::size_t threadCount() const noexcept;

    // Enqueue method.
    void enqueue(std::function<void()> task);

    // Enqueue method for functions with return types.
    template <typename T>
    auto enqueue(T task) -> std::future<decltype(task())>;

private:
    bool running{ true };
    bool poolWorking{ false };
    size_t activeThreads{ 0 };
    std::condition_variable cv{};
    mutable std::mutex task_lock{};
    std::queue<std::function<void()>> tasks{};
    std::function<void()> callback{};
    std::vector<std::thread> threads;
};


// Threadpool constructor.
inline ThreadPool::ThreadPool(std::size_t workers)
    : threads(workers)
{
    // Add the callbackfunction lambda.
    callback = [&]()
    {
        std::unique_lock<std::mutex> lock(task_lock);
        while(running)
        {
            // Wait for a new task/workload.
            cv.wait(lock, [&]() { return !running || !tasks.empty(); });
            activeThreads++;
            poolWorking = true;

            // Test if we need to break.
            if (!running) break;

            // Loop as long as there are tasks to do.
            while (!tasks.empty())
            {
                // Acquire the task.
                std::function<void()> workload = std::move(tasks.front());
                tasks.pop();

                // Excecute the task.
                lock.unlock();
                workload();
                lock.lock();
            }

            // We are going to sleep (or exiting), decrement the active threads.
            if ( --activeThreads == 0 )
            {
                poolWorking = false;
            }
        }
    }; // End of lambda callback.

    for (std::size_t i = 0; i < workers; ++i)
    {
        // Launch threads into pool.
        threads.at(i) = std::thread(callback);
    }
}

// Threadpool destructor.
inline ThreadPool::~ThreadPool()
{
    {
        // Inform threads that no new jobs should be launched.
        std::lock_guard<std::mutex> lock(task_lock);
        running = false;

        // Signal all threads to quit.
        cv.notify_all();
    }

    // Join all threads.
    for (auto &thread : threads)
    {
        thread.join();
    }
}

inline bool ThreadPool::working() const noexcept
{
    // This yield is an attempt to dissallow the threadpool user from starving
    // the threadpool by always holding the 'task_lock'. This could accidentaly
    // happen if the user invokes this method repitedly in a spinlock fashion.
    std::this_thread::yield();

    // Return the working status.
    std::lock_guard<std::mutex> lock(task_lock);
    return poolWorking;
}

inline std::size_t ThreadPool::jobs() const noexcept
{
    std::lock_guard<std::mutex> lock(task_lock);
    return tasks.size();
}

inline std::size_t ThreadPool::threadCount() const noexcept
{
    return threads.size();
}

inline void ThreadPool::enqueue(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(task_lock); poolWorking = true;
    tasks.emplace(std::move(task));
    cv.notify_one();
}

template <typename T>
inline auto ThreadPool::enqueue(T task) -> std::future<decltype(task())>
{
    // Queue up the task.
    std::lock_guard<std::mutex> lock(task_lock); poolWorking = true;
    using Task = std::packaged_task<decltype(task())()>;
    auto wrapper = std::make_shared<Task>(std::move(task));
    auto future = wrapper->get_future();
    tasks.emplace( [=]() { (*wrapper)(); } );
    cv.notify_one();
    return future;
}

#endif
