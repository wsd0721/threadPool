#include "../include/threadpool.h"

#include<iostream>
#include<chrono>
#include<thread>

class MyTask : public Task
{
public:
    MyTask(int begin, int end)
        :begin_(begin)
        , end_(end)
    {}

    Any run()
    {
        std::cout << "tid:" << std::this_thread::get_id() << "begin" << std::endl;
        //std::this_thread::sleep_for(std::chrono::seconds(5));
        long long sum = 0;
        for (int i = begin_; i <= end_; i++)
        {
            sum += i;
        }
        std::cout << "tid:" << std::this_thread::get_id() << "end" << std::endl;

        return sum;
    }

private:
    int begin_;
    int end_;
};

int main(){
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.start(4);

    Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 10000));
    Result res2 = pool.submitTask(std::make_shared<MyTask>(10001, 20000));
    Result res3 = pool.submitTask(std::make_shared<MyTask>(20001, 30000));
    // Master - Slave线程模型
    // Master线程用来分解任务，然后给各个Slave线程分配任务
    // 等待各个Slave线程执行完任务，返回结果
    // Master线程合并任务结果，输出
    long long sum = res1.get().cast_<long long>() + res2.get().cast_<long long>() + res3.get().cast_<long long>();
    std::cout << sum << std::endl;
    /*pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());*/

    getchar();
    return 0;
}