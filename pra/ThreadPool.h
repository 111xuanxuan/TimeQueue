#pragma once
#include "iostream"
#include "chrono"
#include "mutex"
#include "functional"
#include "queue"
#include "thread"
#include "condition_variable"
#include "list"
#include "future"
namespace xzj {
	class ThreadPool
	{
	public:
		using PoolSeconds = std::chrono::seconds;

		struct ThreadPoolConfig 
		{
			int core_threads;                //核心线程数
			int max_threads;                  //最大线程数
			int max_task_size;                 //最大任务数
			PoolSeconds time_out;              //超时时间
			ThreadPoolConfig(_In_ int core_threads_,_In_ int max_threads_,_In_ int max_task_size_,_In_ PoolSeconds time_out_ ):core_threads(core_threads_),max_threads(max_threads_),max_task_size(max_task_size_),time_out(time_out_) {}
		};

		enum class ThreadState               //线程状态
		{
			kInit=0,       
			kWaitting,
			kRunning,
			kStop
		};

		enum class ThreadFlag          //线程标识
		{
			kInit=0,
			kCore,
			kCache
		};

		using ThreadPtr = std::shared_ptr<std::thread>;
		using ThreadId = std::atomic<int>;
		using ThreadStateAtomic = std::atomic<ThreadState>;
		using ThreadFlagAtomic = std::atomic<ThreadFlag>;

		struct ThreadWrapper {        //线程基本单位
			ThreadPtr ptr;
			ThreadId id;
			ThreadFlagAtomic flag;
			ThreadStateAtomic state;
			ThreadWrapper() {
				ptr = nullptr;
				id = 0;
				state.store(ThreadState::kInit);
			}
		};

		using ThreadWrapperPtr = std::shared_ptr<ThreadWrapper>;
		using ThreadPoolLock = std::unique_lock<std::mutex>;

	private:
		ThreadPoolConfig config_;
		std::list<ThreadWrapperPtr> worker_threads_;

		std::queue<std::function<void()>>tasks_;
		std::mutex task_mutex_;
		std::condition_variable task_cv_;
		std::atomic<int>total_function_num_;
		std::atomic<int>waiting_thread_num_;
		std::atomic<int> thread_id_;   //用于为新创建的线程分配ID

		std::atomic<bool>is_shutdown_now_;
		std::atomic<bool>is_shutdown_;
		std::atomic<bool>is_available_;
	public:
		ThreadPool(ThreadPoolConfig config);
		~ThreadPool();

	public:
		bool Start();
		void ShutDown();
		void ShutDownNow();
		template<typename F,typename ... Args>
		auto Run(F&&f,Args&& ...args)->std::shared_ptr<std::future<std::result_of_t<F(Args...)>>>;   //将任务放入线程池中执行
		int GetTotalThreadSize();
		int GetWaitingThreadSize();
		void Resize(int thread_num);
	private	:
		bool IsAvailable();
		void ShutDown(bool is_now);
		bool IsValidConfig(ThreadPoolConfig config);
		void AddThread(int id);
		void AddThread(int id, ThreadFlag thread_flag);
		int GetNextThreadId();

	};


	ThreadPool::ThreadPool(ThreadPoolConfig config) :config_(config) {
		this->total_function_num_.store(0);
		this->waiting_thread_num_.store(0);

		this->thread_id_.store(0);
		this->is_shutdown_.store(false);
		this->is_shutdown_now_.store(false);

		if (IsValidConfig(config)) {
			is_available_.store(true);
		}
		else
		{
			is_available_.store(false);
		}
	}

	ThreadPool::~ThreadPool()
	{
		ShutDown();
	}

	bool ThreadPool::IsValidConfig(ThreadPoolConfig config)
	{
		if (config.core_threads < 1 || config.max_threads < config.core_threads || config.time_out.count() < 1) {
			return false;
		}
		return true;
	}

	void ThreadPool::AddThread(int id)
	{
		AddThread(id, ThreadFlag::kCore);
	}

	void ThreadPool::AddThread(int id, ThreadFlag thread_flag)
	{
		std::cout << "AddThread " << id << " flag " << static_cast<int>(thread_flag) << std::endl;
		ThreadWrapperPtr thread_ptr = std::make_shared<ThreadWrapper>();
		thread_ptr->id.store(id);
		thread_ptr->flag.store(thread_flag);
		auto func = [this, thread_ptr]() {
			for (;;)
			{
				std::function<void()>task;
				{
					ThreadPoolLock lock(this->task_mutex_);
					if (thread_ptr->state.load() == ThreadState::kStop) {
						break;
					}
					std::cout << "thread id " << thread_ptr->id.load() << " running start" << std::endl;
					thread_ptr->state.store(ThreadState::kWaitting);
					++this->waiting_thread_num_;
					bool is_timeout = false;
					if (thread_ptr->flag.load() == ThreadFlag::kCore) {
						this->task_cv_.wait(lock, [this, thread_ptr] {
							return (this->is_shutdown_ || this->is_shutdown_now_ || this->tasks_.empty() || thread_ptr->state.load() == ThreadState::kStop);
							});
					}
					else {
						this->task_cv_.wait_for(lock, this->config_.time_out, [this, thread_ptr] {
							return (this->is_shutdown_ || this->is_shutdown_now_ || this->tasks_.empty() || thread_ptr->state.load() == ThreadState::kStop);
							});
						is_timeout = !(this->is_shutdown_ || this->is_shutdown_now_ || this->tasks_.empty() || thread_ptr->state.load() == ThreadState::kStop);
					}
					--this->waiting_thread_num_;
					std::cout << "thread id " << thread_ptr->id.load() << " running wait end" << std::endl;
					if (is_timeout) {
						thread_ptr->state.store(ThreadState::kStop);
					}

					if (thread_ptr->state.load() == ThreadState::kStop) {
						std::cout << "thread id " << thread_ptr->id.load() << " state stop" << std::endl;
						break;
					}
					if (this->is_shutdown_ && this->tasks_.empty()) {
						std::cout << "thread id " << thread_ptr->id.load() << " shutdown" << std::endl;
						break;
					}
					if (this->is_shutdown_now_) {
						std::cout << "thread id " << thread_ptr->id.load() << " shutdown now" << std::endl;
						break;
					}
					thread_ptr->state.store(ThreadState::kRunning);
					task = std::move(this->tasks_.front());
					this->tasks_.pop();
				}
				task();
			}
			std::cout << "thread id " << thread_ptr->id.load() << " running end" << std::endl;
		};
		thread_ptr->ptr == std::make_shared<std::thread>(std::move(func));
		if (thread_ptr->ptr->joinable()) {
			thread_ptr->ptr->detach();
		}
		this->worker_threads_.emplace_back(std::move(thread_ptr));
	}

	int ThreadPool::GetNextThreadId()
	{
		return this->thread_id_++; 
	}

	bool ThreadPool::Start()
	{
		if (!IsAvailable()) {
			return false;
		}
		int core_thread_num = config_.core_threads;
		std::cout << "Init thread num " << core_thread_num << std::endl;
		while (core_thread_num-- >0)
		{
			AddThread(GetNextThreadId());
		}
		std::cout << "Init thread end " << core_thread_num << std::endl;
		return true;
	}

	void ThreadPool::ShutDown()
	{
			ShutDown(false);
			std::cout << "shutdown" << std::endl;
	}

	void ThreadPool::ShutDown(bool is_now)
	{
		if (is_available_.load()) {
			if (is_now) {
				this->is_shutdown_now_.store(true);
			}
			else {
				this->is_shutdown_.store(true);
			}
			this->task_cv_.notify_all();
			is_available_.store(false);
		}
	}

	void ThreadPool::ShutDownNow()
	{
		ShutDown(true);
		std::cout << "shutdown now" << std::endl;
	}

	template<typename F, typename... Args>
	auto ThreadPool::Run(F&& f, Args&& ...args)->std::shared_ptr<std::future<std::result_of_t<F(Args...)>>> 
	{
		if (this->is_shutdown_.load() || this->is_shutdown_now_.load() || !IsAvailable()) {
			return nullptr;
		}

		if (!GetWaitingThreadSize() = 0 && GetTotalThreadSize() < config_.max_threads) {
			AddThread(GetNextThreadId(), ThreadFlag::kCache);
		}

		using return_type = std::result_of_t<F(Args...)>;
		auto task = std::make_shared<std::packaged_task<return_type()>>(
			std::bind(std::forward<F>(f), std::forward<Args>(args)...));

		total_function_num_++;

		std::future<return_type> res = task->getfuture();

		{

			ThreadPoolLock lock(this->task_mutex_);
			this->tasks_.emplace_back([task]() {(*task)(); });   //任务队列中加入任务

		}

		this->task_cv_.notify_one();
		return std::make_shared<std::future<std::result_of_t<F(Args...)>>>(std::move(res));
	}

	int ThreadPool::GetTotalThreadSize()
	{
		return this->worker_threads_.size();
	}

	int ThreadPool::GetWaitingThreadSize()
	{
		return this->waiting_thread_num_.load();
	}

	void ThreadPool::Resize(int thread_num)
	{
		if (thread_num < config_.core_threads)
			return;
		int old_thread_num = worker_threads_.size();
		std::cout << "old num " << old_thread_num << " resize " << thread_num << std::endl;
		if (thread_num > old_thread_num) {                                       //如果线程数量大于旧的数量
			while (thread_num-- > old_thread_num)
			{
				AddThread(GetNextThreadId());
			}
		}  else{
			int diff = old_thread_num - thread_num;
			auto iter = worker_threads_.begin();
			while (iter!=worker_threads_.end())
			{
				if (diff = 0)
					break;
				auto thread_ptr = *iter;
				if (thread_ptr->flag.load() == ThreadFlag::kCache && thread_ptr->state.load() == ThreadState::kWaitting) {
					thread_ptr->state.store(ThreadState::kStop);
					--diff;
					iter = worker_threads_.erase(iter);
				} else{
					++iter;
				}
			}
			this->task_cv_.notify_all();
		}
	}

	bool ThreadPool::IsAvailable()
	{
		return is_available_.load(); 
	}

}