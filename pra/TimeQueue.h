#pragma once
#include "iostream"
#include "chrono"
#include "mutex"
#include "functional"
#include "queue"
#include "thread"
#include "condition_variable"
#include "ThreadPool.h"
class TimeQueue
{
public:
	struct  Internals
	{
		std::chrono::time_point<std::chrono::high_resolution_clock> time_point_;
		std::function<void()> func_;
		bool operator < (Internals& b)const {
			return time_point_ < b.time_point_;
		}
		int repeated_id;
	};
	enum class RepeatedIdState
	{
		kInit = 0,
		kRuning,
		kStop
	};

private:
	std::priority_queue<Internals>queue_;
	bool running_ = false;                 //运行状态
	std::mutex mutex_;                    //互斥量
	std::condition_variable cond_;       //条件变量

	xzj::ThreadPool thread_pool_;     //线程池
	std::atomic<int>repeated_func_id;
	xzj::ThreadSafeMap<int, RepeatedIdState> repeated_id_state_map_;

public:
	TimeQueue() :running_(true), thread_pool_(xzj::ThreadPool::ThreadPoolConfig(4, 4, 40, std::chrono::seconds(4))) {
		repeated_func_id.store(0);
	}
	~TimeQueue();

public:
	bool Run() {
		bool ret = thread_pool_.Start();
		if (!ret) {
			return false;
		}
		std::thread([this]() {RunLocal(); }).detach();
		return true;
	}

	void Stop() {
		running_ = false;
		cond_.notify_all();
	}

	template<typename F, typename... Args>
	void AddFuncAtTimePoint(const std::chrono::time_point<std::chrono::high_resolution_clock>& time_point, F&& f, Args&& ...args) {   //某个时间点执行任务
		Internals s;
		s.time_point_ = time_point;
		s.func_ = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
		std::unique_lock<std::mutex> lock(mutex_);
		queue_.push(s);
		cond_.notify_all();
	}

	template<typename R, typename P, typename F, typename ...Args>
	void AddFuncAfterDuration(const std::chrono::duration<R, P>& time, F&& f, Args&& ...args) {                       //某段时间执行任务
		Internals s;
		s.time_point_ = std::chrono::high_resolution_clock::now() + time;
		s.func_ = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
		std::unique_lock<std::mutex> lock(mutex_);
		queue_.push(s);
		cond_.notify_all();
	}

	template<typename R, typename P, typename F, typename ...Args>
	void AddRepeatedFunc(int repeat_num, const std::chrono::duration<R, P>& time, F&& f, Args&& ...args) {                       //
		int id = GetNextRepeatedFuncId();
		repeated_id_state_map_.Emplace(id, RepeatedIdState::kRuning);
		auto tem_func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
		AddRepeatedFunc(repeat_num - 1, time, id, std::move(tem_func));
		return id;
	}

	int GetNextRepeatedFuncId() {
		return repeated_func_id++;
	}

	void CancleRepeatedFuncId(int func_id) {
		repeated_id_state_map_.EraseKey(func_id);
	}
private:
	void RunLocal() {
		while (running_)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (queue_.empty()) {                        //任务队列为空
				cond_.wait(lock);                        //阻塞
				continue;
			}
			auto s = queue_.top();
			auto diff = s.time_point_ - std::chrono::high_resolution_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() > 0) {
				cond_.wait_for(lock, diff);
				continue;
			}
			else {
				queue_.pop();
				lock.unlock();
				thread_pool_.Run(std::move(s.func_));
			}
		}
	}

	template<typename R, typename P, typename F>
	void AddRepeatedFuncLocal(int repeat_num, const std::chrono::duration<R, P>& time, int id, F&& f) {
		if (!this->repeated_id_state_map_.IsKeyExist(id)) {
			return;
		}
		Internals s;
		s.time_point_ = std::chrono::high_resolution_clock::now() + time;
		auto tem_func = std::move(f);
		s.repeated_id = id;
		s.func_ = [this, &tem_func, repeat_num, time, id]() {
			tem_func();
			if (!this->repeated_id_state_map_.IsKeyExist(id) || repeat_num == 0) {
				return;
			}
			AddRepeatedFuncLocal(repeat_num - 1, time, id, std::move(tem_func));
		};
		std::unique_lock<std::mutex> lock(mutex_);
		queue_.push(s);
		lock.unlock();
		cond_.notify_all();
	}
};

