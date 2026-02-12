#include<future>
#include<iostream>
#include"threadpool.h"
#include<unistd.h>

int sum1(int a, int b){
    sleep(3);
    return a + b;
}

int main(){
    ThreadPool pool;
    pool.start(4);

    std::future<int> r1 = pool.submitTask(sum1, 10, 20);
    pool.submitTask(sum1, 10, 20);
    pool.submitTask(sum1, 10, 20);
    pool.submitTask(sum1, 10, 20);
    pool.submitTask(sum1, 10, 20);
    pool.submitTask(sum1, 10, 20);
    std::future<int> r2 = pool.submitTask(sum1, 10, 20);

    std::cout << r1.get() << std::endl;
    std::cout << r2.get() << std::endl;
    return 0;
}