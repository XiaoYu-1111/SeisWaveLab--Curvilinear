#pragma once
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
namespace SeisWave {
// One persistent worker. Only the caller consumes completed values; no UI/OpenGL
// operation is ever dispatched to this thread. Cancellation happens between steps.
template<class Result>class BackgroundRunner {
    std::mutex mutex_;
    std::condition_variable wake_,completed_;
    std::function<Result(const std::atomic_bool&)> task_;
    std::optional<Result> result_;
    std::exception_ptr exception_;
    std::atomic_bool cancel_{};
    bool busy_{},ready_{},shutdown_{};
    std::thread thread_;
    std::optional<Result> consume(std::unique_lock<std::mutex>&) {
        if(!ready_)return {};
        ready_=busy_=false;
        if(exception_){auto exception=std::exchange(exception_,{});std::rethrow_exception(exception);}
        auto value=std::move(result_);result_.reset();return value;
    }
public:
    BackgroundRunner():thread_([this]{
        for(;;) {
            std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return shutdown_||bool(task_);});
            if(shutdown_)return;
            auto task=std::move(task_);task_={};lock.unlock();
            std::optional<Result> value;std::exception_ptr exception;
            try{value=task(cancel_);}catch(...){exception=std::current_exception();}
            lock.lock();result_=std::move(value);exception_=exception;ready_=true;completed_.notify_all();
        }
    }){}
    ~BackgroundRunner(){
        cancel();{std::unique_lock lock(mutex_);completed_.wait(lock,[&]{return !busy_||ready_;});shutdown_=true;wake_.notify_all();}
        thread_.join();
    }
    BackgroundRunner(const BackgroundRunner&)=delete;
    bool busy(){std::lock_guard lock(mutex_);return busy_;}
    bool submit(std::function<Result(const std::atomic_bool&)> task) {
        std::lock_guard lock(mutex_);if(busy_)return false;
        cancel_=false;busy_=true;task_=std::move(task);wake_.notify_one();return true;
    }
    void cancel(){cancel_=true;}
    std::optional<Result> poll(){std::unique_lock lock(mutex_);return consume(lock);}
    std::optional<Result> wait(){cancel();std::unique_lock lock(mutex_);completed_.wait(lock,[&]{return !busy_||ready_;});return consume(lock);}
};
}
