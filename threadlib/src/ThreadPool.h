#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <atomic>
#include <thread>
#include <mutex>
#include <future>

#include <vector>
#include <queue>
#include <map>
#include <set>


namespace threadlib {

using FunctionPtr = void(*)(int64_t, void*);
using Timestamp = std::vector<int64_t>;

class JobState;
class Job;
class ThreadPool;

class Task {
public:
  Task(int64_t indvar, void *args, Task *parent, Job *job)
    :  m_job(job), m_indvar(indvar), m_args(args), m_newScope(nullptr){
    if(!parent) {
      m_timestamp = std::vector<int64_t>();
      m_timestamp.push_back(indvar);
      return;
    }

    m_timestamp = parent->m_timestamp;
    m_timestamp.push_back(indvar);
  }

  int64_t getIndVar();
  void *getArgs();
  void *getNewScope();
  void setNewScope(void *scope);

  const std::vector<int64_t> &getTimestamp();
  
  bool operator>(Task const& right) const;
  
  void exec();  

  friend class ThreadPool;

protected:
  Job *m_job;

  int64_t m_indvar; 
  void *m_args;
  void *m_newScope;

  std::vector<int64_t> m_timestamp; 
};

template<class T>
class PtrComparison {
public:
  bool operator()(T *lhs, T *rhs){
    return *lhs > *rhs; 
  }
};

class Job {
public:
  Job(ThreadPool *threadpool, 
      JobState *state,
      FunctionPtr func, 
      FunctionPtr sequential = nullptr, 
      FunctionPtr continued = nullptr, 
      Job *parent = nullptr);
  
  JobState *getState();
  ThreadPool *getThreadPool();

  void addTask(Task* newTask, Task* parentTask);
  Task *popTask();

  bool operator>(Job const& right) const {
    return m_priority > right.m_priority; 
  }

  friend class Task;
  friend class ThreadPool;

protected: 
  ThreadPool *m_threadpool;
   
  FunctionPtr m_func;
  FunctionPtr m_sequentialBody;
  FunctionPtr m_nextFunc;

  Job *m_parent;
  JobState *m_state;
  
  uint32_t m_priority;
  
  std::set<Task *> m_parentTasks;
  
  std::priority_queue<Task *, std::vector<Task *>, PtrComparison<Task>> m_taskQueue;
  
  std::vector<std::thread::id> m_waitingThreads;

  std::promise<void> m_taskFinished;
  std::shared_future<void> m_future;
  std::mutex m_mutex;
  std::mutex m_waitingMutex;

  static std::atomic<uint32_t> s_counter; 
};

class ThreadPool {
public:
  ThreadPool(uint32_t numThreads);
  ~ThreadPool();

  void addTask(FunctionPtr func, void *args, void *newScope, int64_t start, int64_t step, int64_t end, FunctionPtr seqBody, FunctionPtr restOfFunc);

  void finishJob(Job *job);

  uint32_t getSize();
  Job *getJobInProgress();
  bool isMainThread(std::thread::id tid = std::this_thread::get_id());
  const Timestamp &getTimestampForCurrentThread();

  void setPromise(bool value);

  bool wait();
  void clear();
protected:
  Job *getJob(FunctionPtr func);
  Task *getTaskForThread(std::thread::id tid);

  Job *createJob(FunctionPtr func, FunctionPtr sequential, FunctionPtr continued, Job* parent = nullptr, JobState* state = nullptr);
  Task *createTask(int64_t indvar, void *args, Task *parent, Job *job);
  JobState *createJobState();

  void makeReady();
  void dequeueTask();

protected:
  uint32_t m_size;
  bool m_ready;

  std::promise<bool> m_promise;

  std::vector<std::thread> m_threads;
  std::vector<Task *> m_tasks;
  std::vector<Job *> m_jobs;
  std::vector<JobState *> m_states;
  std::vector<void *> m_allocs;

  std::set<std::thread::id> m_threadIdSet;

  std::priority_queue<Job *, std::vector<Job *>, PtrComparison<Job>> m_activeJobs;
  std::map<FunctionPtr, Job *> m_jobMap;
  std::map<std::thread::id, Task *> m_taskMap;
  std::map<Job *, std::vector<Job *>> m_childJobs;

  std::mutex m_taskMutex;
  std::mutex m_jobMutex;
  std::mutex m_isReady;
};
}

#endif
