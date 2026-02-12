#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<vector>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<stdexcept>
#include<unordered_map>
#include<thread>
#include<future>
#include<iostream>

const int TASK_MAX_THRESHHOLD = 2; // INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60; // 60秒

// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,  // 固定数量的线程
    MODE_CACHED, // 线程数量可动态增长，适合小而快的任务，防止线程过多导致系统性能受损
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造函数
    Thread(ThreadFunc func)
        :func_(func)
        , threadId_(generatedId_++)
    {}

    // 线程析构
    ~Thread() = default;

    // 启动线程,函数逻辑为监视任务队列，有任务就消费，需要访问线程池的锁，所以需要在线程池中定义具体函数
    void start()
    {
        // 创建一个线程来执行一个线程函数
        std::thread t(func_, threadId_);

        // 设置分离线程
        t.detach();
    }

    // 获取线程ID
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generatedId_; // 产生递增的线程ID，因为只有线程池线程访问，所以无需原子变量
    int threadId_; // 保存线程id
};

int Thread::generatedId_ = 0;

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool()
        : initThreadSize_(4)
        , idleThreadSize_(0)
        , curThreadSize_(0)
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , taskSize_(0)
        , taskQueThreshHold_(TASK_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
    {}

    // 线程池析构，有构造必须析构
    ~ThreadPool()
    {
        // 优雅退出
        isPoolRunning_ = false;

        // 等待线程池中所有线程返回，有两种状态，等待和执行
        notEmpty_.notify_all();
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        exitCond_.wait(lock, [&]() -> bool
                    { return threads_.size() == 0; });
    }
    
    // 开启线程池
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // 设置线程池的启动状态
        isPoolRunning_ = true;

        // 记录初始线程个数
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // 创建线程对象，保证公平性，先集中创建再启动
        for (int i = 0; i < initThreadSize_; ++i)
        {
            // 创建thread线程对象的时候，把线程函数给到thread对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
        }

        // 启动所有线程
        for (int i = 0; i < initThreadSize_; ++i)
        {
            threads_[i]->start();
            idleThreadSize_++;
        }
    }
    
    // 设置线程池的工作模式
    void setMode(PoolMode mode)
    {
        // 不允许启动后再设置
        if(checkRunninngState())
        {
            return;
        }
        poolMode_ = mode;
    }

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshhold)
    {
        // 不允许启动后再设置
        if(checkRunninngState())
        {
            return;
        }
        taskQueThreshHold_ = threshhold;
    }

    // 设置线程池cached模式下线程阈值
    void setThreadSizeThreshHold(int threshhold)
    {
        // 不允许启动后再设置
        if(checkRunninngState())
        {
            return;
        }
        if(poolMode_ == PoolMode::MODE_CACHED)
        {
            threadSizeThreshHold_ = threshhold;
        }
    }

    // 给线程池提交任务，使用可变参模板编程，使其可以接收任意任务函数和任意数量的参数
    // pool.submitTask(sum1, 10, 20)
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) ->std::future<decltype(func(args...))>
    {
        // 打包任务，放入任务队列
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // 等待任务队列有空余
        if(!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                        { return taskQue_.size() < taskQueThreshHold_; }))
        {
            std::cerr << "task queue is full, submit task fail." << std::endl;
            // 任务提交失败，直接返回空的函数对象产生的空返回值；也可以直接抛异常
            auto task = std::make_shared<std::packaged_task<RType()>>([]() -> RType
                                                                      { return RType(); });
            // 注意一定要执行任务，否则get会出错
            (*task)();
            return task->get_future();
        }

        // 如果有空余，把任务放入任务队列中
        taskQue_.emplace([task]() { (*task)(); });
        taskSize_++;

        // 在notEmpty_上通知
        notEmpty_.notify_all();

        // cached模式：需要根据任务数量和空闲县茨城的数量，判断是否需要创建新的线程
        if(poolMode_ == PoolMode::MODE_CACHED 
            && taskSize_ > idleThreadSize_ 
            && curThreadSize_ < threadSizeThreshHold_)
        {
            std::cout << ">>>create new thread..." << std::endl;

            // 创建新线程
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // 启动线程
            threads_[threadId]->start();
            // 修改相关变量
            curThreadSize_++;
            idleThreadSize_++;
        }

        // 返回任务的Result对象
        return result;
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
private:
    // 定义线程函数
    void threadFunc(int threadId)
    {
        auto lastTime = std::chrono::high_resolution_clock().now();

        //循环等待
        for (;;)
        {
            Task task;
            {
                // 先获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务" << std::endl;

                // cached模式：有可能已经创建了很多线程，但是空闲时间超过60s，应该把多余的线程回收（超过initThreadSize_的数量要进行回收）
                while(taskQue_.size() == 0)
                {
                    // 线程池要结束，回收线程资源
                    if(!isPoolRunning_)
                    {
                        threads_.erase(threadId);
                        curThreadSize_--;
                        idleThreadSize_--;

                        std::cout << "threadid:" << std::this_thread::get_id() << " exit" << std::endl;
                        exitCond_.notify_all();
                        return;
                    }

                    if(poolMode_ == PoolMode::MODE_CACHED)
                    {
                        // 不能一直等待，需要每一秒检查一次
                        if(!notEmpty_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                            { return taskQue_.size() > 0; }))
                        {
                            auto now = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if(dur.count() >= THREAD_MAX_IDLE_TIME
                                && curThreadSize_ > initThreadSize_)
                            {
                                // 开始回收当前线程
                                // 记录线程数量变化
                                // 把线程对象从线程列表中删除，难点：怎么确定threadFunc对应的thread对象
                                threads_.erase(threadId);
                                curThreadSize_--;
                                idleThreadSize_--;

                                std::cout << "threadid:" << std::this_thread::get_id() << " exit" << std::endl;
                                exitCond_.notify_all();
                                return;
                            }
                        }
                    }
                    else
                    {
                        // 等待notEmpty条件
                        notEmpty_.wait(lock);
                    }
                }

                idleThreadSize_--;

                std::cout << "tid:" << std::this_thread::get_id() << "获取任务成功" << std::endl;

                // 取一个任务出来
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;

                // 如果依然有任务，通知其他线程执行任务
                if(taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

                // 在notFull上通知
                notFull_.notify_all();
            } // 应该释放锁，保证不影响其他线程执行

            // 当前线程负责执行任务
            if(task != nullptr){
                // 执行任务，把结果给Result
                task();
            }
            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
        }
    }

    // 检查Pool的运行状态
    // 检查Pool的运行状态
    bool checkRunninngState() const
    {
        return isPoolRunning_;
    }

private:
    //std::vector<std::unique_ptr<Thread>> threads_; // 线程列表，裸指针不安全，使用智能指针
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表，裸指针不安全，使用智能指针
    size_t initThreadSize_;         // 初始线程数量
    std::atomic_int curThreadSize_; //当前线程数量，防止vector线程不安全，size会出错
    size_t threadSizeThreshHold_; // 线程数量上限阈值
    std::atomic_int idleThreadSize_; //记录空闲线程的数量

    using Task = std::function<void()>; // 无法确定传入的函数类型，需要中间层解决，此处直接定义为void即可
    std::queue<Task> taskQue_; // 任务队列，此时Task完全属于线程池内部，无需智能指针延长其生命周期
    std::atomic_uint taskSize_; // 任务数量，被用户和线程池同时读写，需要线程安全
    size_t taskQueThreshHold_; // 任务数量上限阈值

    std::mutex taskQueMtx_; // 保证任务队列线程安全
    std::condition_variable notFull_; // 表示队列不满
    std::condition_variable notEmpty_; // 表示队列不空
    std::condition_variable exitCond_; //等待线程资源全部回收

    PoolMode poolMode_; // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};

#endif