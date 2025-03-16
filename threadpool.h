//#pragma once //windows 中用来防止头文件重复包含的
#ifndef  THREADPOOL_H
#define THREADPOOL_H
#include <iostream>
#include <vector>
#include <queue>
#include<memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include<thread>
#include <future>
#include <chrono>

const int TASK_MAX_THRESHHOLD = 2;
const int Thread_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 10;//单位：秒
//线程池支持两种模式fix和cache
//用class可以避免枚举类型不同，但枚举项相同的冲突
enum class PoolMode
{
	MODE_FIXED,//固定数量的线程
	MODE_CACHE,//线程数量可动态增长
};

//线程类型需要抽象
class Thread
{
public:
	//使用using关键字来定义一个类型别名。
	// 具体来说，它定义了一个名为ThreadFunc的类型别名，这个别名代表了std::function<void()>类型。
	//std::function<void()>：这是C++标准库中的一个通用函数包装器，
	// 它可以存储、复制和调用任何可以接受(无参数)参数类型为int并返回void的函数对象
	using ThreadFunc = std::function<void(int)>;
	//线程构造
	Thread(ThreadFunc func)
		:func_(func)//线程函数接收的是绑定器bind中绑定的threadFunc函数
		, threadId_(generateId_++)
	{}
	// 线程析构
	~Thread()=default;
	//启动线程
	void start()
	{
		//创建一个线程来执行线程函数
	//创建了一个新的线程 t，并且它会立即开始执行传递给它的函数 func_。
		std::thread t(func_, threadId_);
		//这行代码将新创建的线程 t 分离（detach）出来。一旦线程被分离，它就会独立执行，而与原来的 std::thread 对象不再有任何关联
		t.detach();
	}

	//获取线程id
	int getId() const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;//保存线程id
};

//静态成员变量需要在类外初始化
int Thread::generateId_ = 0;
//线程池类型需要抽象出来
class ThreadPool
{
public:
	//ThreadPool();//线程池构造
	ThreadPool()
		:initThreadSize_(0)
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(Thread_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}
	~ThreadPool()//线程池析构
	{
		isPoolRunning_ = false;

		//等待线程池里所有的线程返回 ，池中的线程有两种状态：阻塞和指针执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}
	//设置线程池的工作模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
		{
			return;
		}
		poolMode_ = mode;
	}
	//设置初始的线程数量,可以直接并入到start()
	//void setInitThreadSize(int size);
	//设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
		{
			return;
		}
		taskQueMaxThreshHold_ = threshhold;
	}
	//设置cache模式下，线程池的线程上限阈值
	void setThreadSizeThreshHold(int threshhold)
	{
		if (checkRunningState())
		{
			return;
		}
		if (poolMode_ == PoolMode::MODE_CACHE)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}
	//给线程池提交任务
	//使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
	//Result submitTask(std::shared_ptr<Task> sp);
	template<typename Func,typename...Args>
	auto submitTask(Func&& func, Args&&...args) -> std::future<decltype(func(args...))>
	{
		//打包任务，放入任务队列
		using RType = decltype(func(args...));//返回值类型
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
		);
		std::future<RType> reslut = task->get_future();

		//获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
		{
			//表示notFull_等待1s，条件仍然没有满足
			std::cerr << "task queue is full,submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(*task)();
			return task->get_future();//错误
		}

		//如果有空余，把任务放入任务队列中
		//taskQue_.emplace(sp);
		taskQue_.emplace([task]() {(*task)(); });
		taskSize_++;

		//因为新放了任务，任务队列不空，notEmpty_上进行通知,可以分配线程执行任务
		notEmpty_.notify_all();

		//cache模式，任务处理比较紧急，场景：小而快的任务
		//需要根据任务数量和空闲线程数量，判断是否需要创建新的线程出来
		if (poolMode_ == PoolMode::MODE_CACHE
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ <= threadSizeThreshHold_)
		{
			std::cout << ">>>>create new thread!" << std::endl;
			//创建新线程
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));

			threads_[threadId]->start();//启动线程

			curThreadSize_++;//修改当前线程数量
			idleThreadSize_++;//修改空闲线程数量
		}

		//返回任务的Result对象
		return  reslut;//一、task->getResult(),result套在task里，在这里不可以
		//二、Result(task),task隶属于Result里
	}
	//开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//设置线程池的运行状态
		isPoolRunning_ = true;
		//记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;
		//创建线程对象
		for (int i = 0; i < initThreadSize_; i++)
		{
			//创建thread线程对象的时候，把线程函数threadFunc给到thread线程对象
			//threads_.emplace_back(new Thread());
			//threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
			std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			//threads_.emplace_back(ptr);//这一行会报错，
			// 是因为unique_ptr的特点是只允许一个指针指向这块内存,不允许进行普通的拷贝构造和赋值
			//threads_.emplace_back(std::move(ptr));
		}

		//启动所有线程,std::vector<Thread*> threads_;
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();//需要执行线程函数
			idleThreadSize_++;//记录初始空闲线程数量
		}
	}

	ThreadPool(const ThreadPool&) = delete;//删除了ThreadPool类的复制构造函数。
	//删除了ThreadPool类的复制赋值操作符
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	//定义线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();
		for (;;)
		{
			Task task;
			{
				//先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << " tid:" << std::this_thread::get_id()
					<< "尝试获取任务。。。" << std::endl;

				//在cache模式下有可能已经创建了很多线程，但是空闲时间超过60s，
				// 应该把多余线程回收掉(超过initThreadSize_数量的线程要被回收掉)
				//当前时间-上一次线程执行的时间>60s
				//锁+双重判断
				while (taskQue_.size() == 0)
				{

					//线程池要结束，回收线程资源
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
						exitCond_.notify_all();
						return;
					}
					//每秒返回一次  怎么区分：超时返回，还是有任务待执行返回
					if (poolMode_ == PoolMode::MODE_CACHE)
					{
						//条件变量超时返回了
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								//开始回收当前线程
								//记录线程变量的相关变量的值的修改
								//把线程对象从线程列表容器中删除
								//threadid=>thread对象=>删除
								threads_.erase(threadid);
								curThreadSize_--;
								idleThreadSize_--;
								std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
								return;

							}
						}
					}
					else
					{
						//等待notEmpty_
						//notEmpty_.wait(lock, [&]()->bool {return taskQue_.size() > 0; });
						notEmpty_.wait(lock);
					}
					
				}



				idleThreadSize_--;

				std::cout << " tid:" << std::this_thread::get_id()
					<< "获取任务成功。。。" << std::endl;
				//从任务队列中取出一个任务
				task = taskQue_.front();//从队列头取出任务
				taskQue_.pop();//将取出的任务从任务队列中删除
				taskSize_--;

				//取出一个任务，如果队列中还有任务，通知其他线程执行任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				//取出一个任务，进行通知,队列不满,用户线程可以提交任务
				notFull_.notify_all();
			}//取出一个任务就该把锁释放掉


			 //当前线程负责执行这个任务
			if (task != nullptr)
			{
				//task->run();//1.执行任务。2.把任务的返回值通过setVal方法给到Result
				//task->exec();
				task();//执行function<void()>
			}
			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now();//更新线程执行完任务的时间

		}
	}
	//检查pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}
private:
	//std::vector<std::unique_ptr<Thread>> threads_;//线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//线程列表
	int initThreadSize_;//初始线程数量
	std::atomic_int curThreadSize_;//记录当前线程池中线程的总数量
	std::atomic_int idleThreadSize_;//记录空闲线程的数量
	int threadSizeThreshHold_;//线程数量上限阈值

	//Task任务就是函数对象submitTask的函数对象
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;//任务队列，智能指针，为了延长对象的的生命周期
	//在多线程环境中，如果多个线程需要读取或修改 taskSize_ 的值，使用 std::atomic_int 可以保证这些操作是原子的，从而避免潜在的数据竞争和不确定的行为。
	std::atomic_int taskSize_;//任务的数量
	int taskQueMaxThreshHold_;//任务队列中任务数量上限阈值

	//用户线程和线程池内部线程都会对任务队列产生影响，所以需要线程互斥来保证任务队列的线程安全
	std::mutex taskQueMtx_;
	std::condition_variable notFull_;//表示任务队列不满，即用户线程可以往队列中添加任务
	std::condition_variable notEmpty_;//表示任务队列不空，即可以从任务队列中取出任务执行
	std::condition_variable exitCond_;//等待线程资源全部回收

	PoolMode poolMode_;//当前线程池的工作模式
	std::atomic_bool isPoolRunning_;//表示当前线程池的启动状态

};

#endif // ! THREADPOOL_H

