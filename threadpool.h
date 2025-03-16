//#pragma once //windows ��������ֹͷ�ļ��ظ�������
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
const int THREAD_MAX_IDLE_TIME = 10;//��λ����
//�̳߳�֧������ģʽfix��cache
//��class���Ա���ö�����Ͳ�ͬ����ö������ͬ�ĳ�ͻ
enum class PoolMode
{
	MODE_FIXED,//�̶��������߳�
	MODE_CACHE,//�߳������ɶ�̬����
};

//�߳�������Ҫ����
class Thread
{
public:
	//ʹ��using�ؼ���������һ�����ͱ�����
	// ������˵����������һ����ΪThreadFunc�����ͱ������������������std::function<void()>���͡�
	//std::function<void()>������C++��׼���е�һ��ͨ�ú�����װ����
	// �����Դ洢�����ƺ͵����κο��Խ���(�޲���)��������Ϊint������void�ĺ�������
	using ThreadFunc = std::function<void(int)>;
	//�̹߳���
	Thread(ThreadFunc func)
		:func_(func)//�̺߳������յ��ǰ���bind�а󶨵�threadFunc����
		, threadId_(generateId_++)
	{}
	// �߳�����
	~Thread()=default;
	//�����߳�
	void start()
	{
		//����һ���߳���ִ���̺߳���
	//������һ���µ��߳� t����������������ʼִ�д��ݸ����ĺ��� func_��
		std::thread t(func_, threadId_);
		//���д��뽫�´������߳� t ���루detach��������һ���̱߳����룬���ͻ����ִ�У�����ԭ���� std::thread ���������κι���
		t.detach();
	}

	//��ȡ�߳�id
	int getId() const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;//�����߳�id
};

//��̬��Ա������Ҫ�������ʼ��
int Thread::generateId_ = 0;
//�̳߳�������Ҫ�������
class ThreadPool
{
public:
	//ThreadPool();//�̳߳ع���
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
	~ThreadPool()//�̳߳�����
	{
		isPoolRunning_ = false;

		//�ȴ��̳߳������е��̷߳��� �����е��߳�������״̬��������ָ��ִ��������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}
	//�����̳߳صĹ���ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
		{
			return;
		}
		poolMode_ = mode;
	}
	//���ó�ʼ���߳�����,����ֱ�Ӳ��뵽start()
	//void setInitThreadSize(int size);
	//����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
		{
			return;
		}
		taskQueMaxThreshHold_ = threshhold;
	}
	//����cacheģʽ�£��̳߳ص��߳�������ֵ
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
	//���̳߳��ύ����
	//ʹ�ÿɱ��ģ���̣���submitTask���Խ������������������������Ĳ���
	//Result submitTask(std::shared_ptr<Task> sp);
	template<typename Func,typename...Args>
	auto submitTask(Func&& func, Args&&...args) -> std::future<decltype(func(args...))>
	{
		//������񣬷����������
		using RType = decltype(func(args...));//����ֵ����
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
		);
		std::future<RType> reslut = task->get_future();

		//��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//�û��ύ�����������������1s�������ж��ύ����ʧ�ܣ�����
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
		{
			//��ʾnotFull_�ȴ�1s��������Ȼû������
			std::cerr << "task queue is full,submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(*task)();
			return task->get_future();//����
		}

		//����п��࣬������������������
		//taskQue_.emplace(sp);
		taskQue_.emplace([task]() {(*task)(); });
		taskSize_++;

		//��Ϊ�·�������������в��գ�notEmpty_�Ͻ���֪ͨ,���Է����߳�ִ������
		notEmpty_.notify_all();

		//cacheģʽ��������ȽϽ�����������С���������
		//��Ҫ�������������Ϳ����߳��������ж��Ƿ���Ҫ�����µ��̳߳���
		if (poolMode_ == PoolMode::MODE_CACHE
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ <= threadSizeThreshHold_)
		{
			std::cout << ">>>>create new thread!" << std::endl;
			//�������߳�
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));

			threads_[threadId]->start();//�����߳�

			curThreadSize_++;//�޸ĵ�ǰ�߳�����
			idleThreadSize_++;//�޸Ŀ����߳�����
		}

		//���������Result����
		return  reslut;//һ��task->getResult(),result����task������ﲻ����
		//����Result(task),task������Result��
	}
	//�����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//�����̳߳ص�����״̬
		isPoolRunning_ = true;
		//��¼��ʼ�̸߳���
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;
		//�����̶߳���
		for (int i = 0; i < initThreadSize_; i++)
		{
			//����thread�̶߳����ʱ�򣬰��̺߳���threadFunc����thread�̶߳���
			//threads_.emplace_back(new Thread());
			//threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
			std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			//threads_.emplace_back(ptr);//��һ�лᱨ��
			// ����Ϊunique_ptr���ص���ֻ����һ��ָ��ָ������ڴ�,�����������ͨ�Ŀ�������͸�ֵ
			//threads_.emplace_back(std::move(ptr));
		}

		//���������߳�,std::vector<Thread*> threads_;
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();//��Ҫִ���̺߳���
			idleThreadSize_++;//��¼��ʼ�����߳�����
		}
	}

	ThreadPool(const ThreadPool&) = delete;//ɾ����ThreadPool��ĸ��ƹ��캯����
	//ɾ����ThreadPool��ĸ��Ƹ�ֵ������
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	//�����̺߳���
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();
		for (;;)
		{
			Task task;
			{
				//�Ȼ�ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << " tid:" << std::this_thread::get_id()
					<< "���Ի�ȡ���񡣡���" << std::endl;

				//��cacheģʽ���п����Ѿ������˺ܶ��̣߳����ǿ���ʱ�䳬��60s��
				// Ӧ�ðѶ����̻߳��յ�(����initThreadSize_�������߳�Ҫ�����յ�)
				//��ǰʱ��-��һ���߳�ִ�е�ʱ��>60s
				//��+˫���ж�
				while (taskQue_.size() == 0)
				{

					//�̳߳�Ҫ�����������߳���Դ
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
						exitCond_.notify_all();
						return;
					}
					//ÿ�뷵��һ��  ��ô���֣���ʱ���أ������������ִ�з���
					if (poolMode_ == PoolMode::MODE_CACHE)
					{
						//����������ʱ������
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								//��ʼ���յ�ǰ�߳�
								//��¼�̱߳�������ر�����ֵ���޸�
								//���̶߳�����߳��б�������ɾ��
								//threadid=>thread����=>ɾ��
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
						//�ȴ�notEmpty_
						//notEmpty_.wait(lock, [&]()->bool {return taskQue_.size() > 0; });
						notEmpty_.wait(lock);
					}
					
				}



				idleThreadSize_--;

				std::cout << " tid:" << std::this_thread::get_id()
					<< "��ȡ����ɹ�������" << std::endl;
				//�����������ȡ��һ������
				task = taskQue_.front();//�Ӷ���ͷȡ������
				taskQue_.pop();//��ȡ������������������ɾ��
				taskSize_--;

				//ȡ��һ��������������л�������֪ͨ�����߳�ִ������
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				//ȡ��һ�����񣬽���֪ͨ,���в���,�û��߳̿����ύ����
				notFull_.notify_all();
			}//ȡ��һ������͸ð����ͷŵ�


			 //��ǰ�̸߳���ִ���������
			if (task != nullptr)
			{
				//task->run();//1.ִ������2.������ķ���ֵͨ��setVal��������Result
				//task->exec();
				task();//ִ��function<void()>
			}
			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now();//�����߳�ִ���������ʱ��

		}
	}
	//���pool������״̬
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}
private:
	//std::vector<std::unique_ptr<Thread>> threads_;//�߳��б�
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//�߳��б�
	int initThreadSize_;//��ʼ�߳�����
	std::atomic_int curThreadSize_;//��¼��ǰ�̳߳����̵߳�������
	std::atomic_int idleThreadSize_;//��¼�����̵߳�����
	int threadSizeThreshHold_;//�߳�����������ֵ

	//Task������Ǻ�������submitTask�ĺ�������
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;//������У�����ָ�룬Ϊ���ӳ�����ĵ���������
	//�ڶ��̻߳����У��������߳���Ҫ��ȡ���޸� taskSize_ ��ֵ��ʹ�� std::atomic_int ���Ա�֤��Щ������ԭ�ӵģ��Ӷ�����Ǳ�ڵ����ݾ����Ͳ�ȷ������Ϊ��
	std::atomic_int taskSize_;//���������
	int taskQueMaxThreshHold_;//�����������������������ֵ

	//�û��̺߳��̳߳��ڲ��̶߳����������в���Ӱ�죬������Ҫ�̻߳�������֤������е��̰߳�ȫ
	std::mutex taskQueMtx_;
	std::condition_variable notFull_;//��ʾ������в��������û��߳̿������������������
	std::condition_variable notEmpty_;//��ʾ������в��գ������Դ����������ȡ������ִ��
	std::condition_variable exitCond_;//�ȴ��߳���Դȫ������

	PoolMode poolMode_;//��ǰ�̳߳صĹ���ģʽ
	std::atomic_bool isPoolRunning_;//��ʾ��ǰ�̳߳ص�����״̬

};

#endif // ! THREADPOOL_H

