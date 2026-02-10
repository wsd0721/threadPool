#include"../include/threadpool.h"
#include<functional>
#include<iostream>
#include<thread>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60; // 60秒

// 线程池构造，根据成员变量决定哪些需要初始化
ThreadPool::ThreadPool()
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
ThreadPool::~ThreadPool()
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
void ThreadPool::start(int initThreadSize)
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
void ThreadPool::setMode(PoolMode mode)
{
    // 不允许启动后再设置
    if(checkRunninngState())
    {
        return;
    }
    poolMode_ = mode;
}

// 设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
    // 不允许启动后再设置
    if(checkRunninngState())
    {
        return;
    }
    taskQueThreshHold_ = threshhold;
}

// 设置线程池cached模式下线程阈值
void ThreadPool::setThreadSizeThreshHold(int threshhold)
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

// 给线程池提交任务，允许提交失败，返回结果即可
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    // 获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // 等待任务队列有空余
    // 原始版本
    //while(taskQue_.size() == taskQueThreshHold_)
    //{
    //    notFull_.wait(lock);
    //}
    if(!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                      { return taskQue_.size() < taskQueThreshHold_; }))
    {
        std::cerr << "task queue is full, submit task fail." << std::endl;
        return Result(sp, false);
    }

    // 如果有空余，把任务放入任务队列中
    taskQue_.emplace(sp);
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
    return Result(sp);
}

// 定义线程函数
void ThreadPool::threadFunc(int threadId)
{
    // 调试代码
    //std::cout << "begin threadFunc tid:" << std::this_thread::get_id() << std::endl;
    //std::cout << "end threadFunc tid:" << std::this_thread::get_id() << std::endl;

    auto lastTime = std::chrono::high_resolution_clock().now();

    //循环等待
    while (isPoolRunning_)
    {
        std::shared_ptr<Task> task;
        {
            // 先获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务" << std::endl;

            // cached模式：有可能已经创建了很多线程，但是空闲时间超过60s，应该把多余的线程回收（超过initThreadSize_的数量要进行回收）
            while(taskQue_.size() == 0)
            {
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
                            return;
                        }
                    }
                }
                else
                {
                    // 等待notEmpty条件
                    notEmpty_.wait(lock);
                }

                if(!isPoolRunning_)
                {
                    threads_.erase(threadId);
                    curThreadSize_--;
                    idleThreadSize_--;

                    std::cout << "threadid:" << std::this_thread::get_id() << " exit" << std::endl;
                    exitCond_.notify_all();
                    return;
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
            task->exec();
        }
        idleThreadSize_++;
        lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
    }

    threads_.erase(threadId);
    curThreadSize_--;
    idleThreadSize_--;

    std::cout << "threadid:" << std::this_thread::get_id() << " exit" << std::endl;
    exitCond_.notify_all();
    return;
}

// 检查Pool的运行状态
bool ThreadPool::checkRunninngState() const
{
    return isPoolRunning_;
}

///////////////// 线程方法实现
int Thread::generatedId_ = 0;

// 线程构造函数
Thread::Thread(ThreadFunc func)
    :func_(func)
    , threadId_(generatedId_++)
{}

// 线程析构
Thread::~Thread()
{}

// 启动线程
void Thread::start()
{
    // 创建一个线程来执行一个线程函数
    std::thread t(func_, threadId_);

    // 设置分离线程
    t.detach();
}

// 获取线程ID
int Thread::getId() const
{
    return threadId_;
}

///////////////// Task方法实现
Task::Task()
    :result_(nullptr)
{}

void Task::exec()
{
    if(result_ != nullptr)
    {
        result_->setVal(run());
    }
}

void Task::setResult(Result* res)
{
    result_ = res;
}

///////////////// Result方法实现
// Result构造函数
Result::Result(std::shared_ptr<Task> task, bool isValid)
    :task_(task)
    ,isValid_(isValid)
{
    task_->setResult(this);
}

Any Result::get()
{
    if(!isValid_)
    {
        return "";
    }

    sem_.wait(); // task任务如果没有执行完，这里会阻塞用户线程
    return std::move(any_);
}

// setVal方法。获取任务执行完的返回值
void Result::setVal(Any any)
{
    // 存储task的返回值
    this->any_ = std::move(any);

    // 已经获取任务的返回值，增加信号量
    sem_.post();
}