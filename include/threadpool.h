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

// Any类型的手动实现
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any &) = delete;
    Any &operator=(const Any &) = delete;
    Any(Any &&) = default;
    Any &operator=(Any &&) = default;

    // 构造函数允许Any接收任意类型的数据
    template <typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data))
    {}

    // 提取Any的data数据
    template<typename T>
    T cast_()
    {
        Derive<T> *pd = dynamic_cast<Derive<T>*>(base_.get());
        if(pd == nullptr)
        {
            throw std::runtime_error("type is incompatiable");
        }
        return pd->data_;
    }

private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    // 派生类类型
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {}
        T data_;
    };
private:
    // 定义一个基类指针
    std::unique_ptr<Base> base_;
};

// 实现一个信号量类
class Semaphore
{
public:
    Semaphore(int limit = 0) 
        :resLimit_(limit)
    {}
    ~Semaphore() = default;

    // 获取一个信号量
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_.wait(lock, [&]() -> bool
                   { return resLimit_ > 0; });
        resLimit_--;
    }

    //增加一个信号量
    void post()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }

private:
    int resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

class Task;
// 实现接收提交到线程池的Task任务执行完成后的返回值类型
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    // setVal方法。获取任务执行完的返回值
    void setVal(Any any);

    // get方法
    Any get();

private:
    Any any_; // 存储任务的返回值
    Semaphore sem_; // 线程通信的信号量
    std::shared_ptr<Task> task_; // 指向对应获取返回值的任务对象
    std::atomic_bool isValid_;
};

// 任务抽象基类
class Task
{
public:
    Task();

    // 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    virtual Any run() = 0;

    void exec();

    void setResult(Result *res);

private:
    Result *result_; // 不能用shared_ptr，会导致循环引用
};

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
    using ThreadFunc = std::function<void()>;

    // 线程构造函数
    Thread(ThreadFunc func);

    // 线程析构
    ~Thread();

    // 启动线程,函数逻辑为监视任务队列，有任务就消费，需要访问线程池的锁，所以需要在线程池中定义具体函数
    void start();

private:
    ThreadFunc func_;
};

/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
public:
    void run() { // 线程代码... }
};

pool.submitTask(std::make_shared<MyTask>());
*/
// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool();

    // 线程池析构
    ~ThreadPool();
    
    // 开启线程池，同时指定线程数量，避免再写一个接口
    void start(int initThreadSize = 4);
    
    // 设置线程池的工作模式
    void setMode(PoolMode mode);

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshhold);

    // 给线程池提交任务，如果1秒没有等待空余，允许提交失败，返回结果即可
    Result submitTask(std::shared_ptr<Task> sp);

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
private:
    // 定义线程函数
    void threadFunc();

    // 检查Pool的运行状态
    bool checkRunninngState() const;

private:
    std::vector<std::unique_ptr<Thread>> threads_; // 线程列表，裸指针不安全，使用智能指针
    size_t initThreadSize_;          // 初始线程数量

    std::queue<std::shared_ptr<Task>> taskQue_; // 任务队列，需要多态所以选择指针；需要生命周期够长，所以选择智能指针
    std::atomic_uint taskSize_;     // 任务数量，被用户和线程池同时读写，需要线程安全
    size_t taskQueThreshHold_;      // 任务数量上限阈值

    std::mutex taskQueMtx_;         // 保证任务队列线程安全
    std::condition_variable notFull_;  // 表示队列不满
    std::condition_variable notEmpty_; // 表示队列不空

    PoolMode poolMode_; // 当前线程池的工作模式

    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};

#endif