// threadPoolFinalVersion.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <functional>
#include <thread>
#include <future>
#include "threadpool.h"
/*
如何让线程池提交任务更方便
1.pool.submitTask(),函数用可变参模板编程
2.为了接收返回值创造了Result相关类型，比较复杂
C++11线程库 thread  packaged_task(function函数对象)  async(比thread强大，可以直接获取返回值).
使用future 来代替Result，节省线程池代码
*/
using namespace std;
int sum1(int a, int b)
{
    this_thread::sleep_for(chrono::seconds(2));
    return a + b;
}
int sum2(int a, int b,int c)
{
    this_thread::sleep_for(chrono::seconds(2));
    return a + b+c;
}
int main()
{
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHE);
    pool.start(2);

    future<int> r1=pool.submitTask(sum1, 1, 2);
    future<int> r2 = pool.submitTask(sum2, 1, 2,3);
    future<int> r3 = pool.submitTask([](int b, int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++)
            sum += i;
        return sum;
        },1,100);
    future<int> r4 = pool.submitTask([](int b, int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++)
            sum += i;
        return sum;
        }, 1, 100);
    future<int> r5 = pool.submitTask([](int b, int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++)
            sum += i;
        return sum;
        }, 1, 100);

    cout << r1.get() << endl;
    cout << r2.get() << endl;
    cout << r3.get() << endl;
    cout << r4.get() << endl;
    cout << r5.get() << endl;
   /* packaged_task<int(int, int)> task(sum1);
    future<int> res = task.get_future();
    thread t(std::move(task), 10, 20);
    t.join();//或者t.detach()
    cout << res.get() << endl;
    std::cout << "Hello World!\n";*/
    return 0;
}


