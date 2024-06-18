#include "ThreadPool.h"
#include "JobState.h"

#include <cassert>
#include <functional>


#include <iostream>


using namespace threadlib;

std::atomic<uint32_t> Job::s_counter = 0; 

int64_t Task::getIndVar(){
  return m_indvar;
}

void *Task::getArgs(){
  return m_args;
}

void *Task::getNewScope(){
  return m_newScope;
}

void Task::setNewScope(void *scope){
  m_newScope = scope;
}

const std::vector<int64_t> &Task::getTimestamp(){
  return m_timestamp;
}

bool Task::operator>(Task const& right) const {
  return m_timestamp > right.m_timestamp;
}

void Task::exec(){
  assert(m_job && "parent is null!");
  
  m_job->m_func(m_indvar, m_args);
}

Job::Job(ThreadPool *threadpool,  
    JobState *state,
    FunctionPtr func,
    FunctionPtr sequential,
    FunctionPtr continued,
    Job *parent) 
: m_threadpool(threadpool), 
  m_func(func), 
  m_sequentialBody(sequential), 
  m_nextFunc(continued), 
  m_parent(parent), 
  m_state(state),
  m_priority(s_counter++),
  m_future(m_taskFinished.get_future()){}

JobState* Job::getState(){
  return m_state;
}

ThreadPool *Job::getThreadPool(){
  return m_threadpool;
}

Task *Job::popTask(){
  m_mutex.lock();

  if(m_taskQueue.empty() || !m_state->noConflicts()){
    m_waitingThreads.push_back(std::this_thread::get_id());
    uint32_t size = m_waitingThreads.size();

    if(size == m_threadpool->getSize()){
      m_threadpool->finishJob(this);
      m_mutex.unlock();
      m_taskFinished.set_value(); 
    } else {
      m_mutex.unlock();
      m_future.wait();
    }

    return nullptr;
  }

  Task *task = m_taskQueue.top();
  m_taskQueue.pop();
  
  m_mutex.unlock();
  return task;
}

void Job::addTask(Task *newTask, Task *taskParent){
  std::scoped_lock lock(m_mutex);
  if(!m_state->noConflicts()) return;
  
  if(taskParent) m_parentTasks.insert(taskParent);
  m_taskQueue.push(newTask);
}

void ThreadPool::finishJob(Job *job){
  if(!job->m_state->noConflicts()){
    job->m_state->rollback();
  }

  if(job->m_nextFunc || job->m_sequentialBody){
    assert(job->m_parent && "no parent scope to exec from");
    assert(job->m_nextFunc && "m_nextFunc is null");
    assert(job->m_sequentialBody && "m_sequentialBody is null");

    FunctionPtr nextFunc = job->m_state->noConflicts() ? job->m_nextFunc : job->m_sequentialBody;
    Job *nextJob = createJob(nextFunc, nullptr, nullptr, job, job->m_parent->m_state);  
    
    for(Task *task : job->m_parentTasks){
      void *scope = job->m_state->noConflicts() ? task->getNewScope() : task->getArgs();

      Task *newTask = createTask(task->getIndVar(), scope, task, nextJob);
      nextJob->addTask(newTask, nullptr);
    }
  }

  if(job != m_activeJobs.top()){
    std::cout << "FINISHED JOB IS NOT AT FRONT OF THE QUEUE\n";
    std::cout << "finished job: " << job->m_priority << " job at front: " << m_activeJobs.top()->m_priority << "\n";
  } else {
    m_activeJobs.pop();
    //std::cout << "POPPED FINISHED JOB\n";
    if(m_childJobs.find(job) != m_childJobs.end() && job->m_state->noConflicts()){
      //std::cout << "Adding children to q\n";
      for(Job *child : m_childJobs[job]){
        m_activeJobs.push(child);
      }
      m_childJobs.erase(job);
    }
  }

  if(m_activeJobs.empty()) setPromise(job->m_state->noConflicts());
}

ThreadPool::ThreadPool(uint32_t numThreads) : m_size(numThreads), m_ready(false) {}

ThreadPool::~ThreadPool(){
  clear();
}

void ThreadPool::addTask(FunctionPtr func, 
    void* args, 
    void* newScope,
    int64_t start, 
    int64_t step, int64_t end,
    FunctionPtr sequential, 
    FunctionPtr continued){

  std::thread::id tid = std::this_thread::get_id();  
  Job *job = nullptr;
  {
  std::scoped_lock jobLock(m_jobMutex);
  job = getJob(func); 

  if(!job) {
    Job *parent = nullptr;
    {
    std::scoped_lock taskLock(m_taskMutex);
    Task *t = getTaskForThread(tid);    
    if(t) parent = t->m_job;
    }
    job = createJob(func, sequential, continued, parent); 
  }

  }
  assert(job && "Job is null!");

  //std::cout << "For loop stats: start:" << start << " step:" << step << " final:" << end << "\n";  
   
  for(int64_t i = start; i < end; i += step) {
    Task *taskParent;
    if(isMainThread(tid)){
      taskParent = nullptr;
    } else {
      std::scoped_lock lock(m_taskMutex);
      taskParent = getTaskForThread(tid);
      taskParent->setNewScope(newScope);
    }
    Task *newTask = nullptr;
    {
    std::scoped_lock lock(m_taskMutex);
    newTask = createTask(i, args, taskParent, job);
    }
    job->addTask(newTask, taskParent);
  }
  makeReady();
}

const Timestamp &ThreadPool::getTimestampForCurrentThread(){
  Task *task = nullptr;
  {
  std::scoped_lock lock(m_taskMutex);
  task = getTaskForThread(std::this_thread::get_id());
  }
  assert(task && "task is null!");

  const Timestamp &t = task->getTimestamp();
  return t;
}

uint32_t ThreadPool::getSize(){
  return m_size;
}

Job *ThreadPool::getJob(FunctionPtr func){
  const auto &it = m_jobMap.find(func); 
  return it == m_jobMap.end() ? nullptr : it->second; 
}

Job *ThreadPool::getJobInProgress(){
  return m_activeJobs.top();
}

Job *ThreadPool::createJob(
    FunctionPtr func, 
    FunctionPtr sequential, 
    FunctionPtr continued, 
    Job *parent,
    JobState *state){
  assert((m_jobMap.find(func) == m_jobMap.end()) && "Job for function already exists!");
  
  //std::cout << "creating new job\n";
  if(!state) state = createJobState();
  Job *job = new Job(this, state, func, sequential, continued, parent);
  m_jobMap[func] = job;

  if(parent) m_childJobs[parent].push_back(job);
  else m_activeJobs.push(job);
  m_jobs.push_back(job);
  return job;
}

Task *ThreadPool::createTask(int64_t indvar, void *args, Task *parent, Job *job){
  Task *task = new Task(indvar, args, parent, job);
  m_tasks.push_back(task);
  return task;
}

JobState *ThreadPool::createJobState(){
  JobState *state = new JobState(this);
  m_states.push_back(state);
  return state;
}

bool ThreadPool::isMainThread(std::thread::id tid){
  std::scoped_lock lock(m_isReady);
  return m_threadIdSet.find(tid) == m_threadIdSet.end();
}

Task *ThreadPool::getTaskForThread(std::thread::id tid){
  if(isMainThread(tid)) return nullptr;

  const auto &it = m_taskMap.find(tid);  
  return it == m_taskMap.end() ? nullptr : it->second; 
}

void ThreadPool::setPromise(bool value){
  m_promise.set_value(value);
}

bool ThreadPool::wait(){
  std::future<bool> success = m_promise.get_future();
  success.wait();
  return success.get();
}

void ThreadPool::makeReady(){
  if(m_ready) return;
  std::scoped_lock lock(m_isReady);
  m_threads.reserve(m_size);
  for(uint32_t i = 0; i < m_size; i++){
    m_threads.push_back(std::thread(&ThreadPool::dequeueTask, this));
    m_threadIdSet.insert(m_threads[i].get_id());
  }
  m_ready = true;
}

void ThreadPool::dequeueTask(){
  while(true){
    Job *job = nullptr;
    Task *task = nullptr;

    {
    std::scoped_lock lock(m_isReady);
    if(!m_ready) continue;
    }

    {
    std::scoped_lock lock(m_jobMutex);
    if(m_activeJobs.empty()) break;
    job = m_activeJobs.top();
    }

    assert(job && "null job for dequeue");
    task = job->popTask(); 
    if(!task) {
      continue;
    }

    {
    std::scoped_lock lock(m_taskMutex);
    m_taskMap[std::this_thread::get_id()] = task;
    }

    task->exec();
  }
}

void ThreadPool::clear(){
  assert(m_activeJobs.empty() && "tried to clear threadpool with active jobs!");
  for(std::thread &t : m_threads){
    t.join();
  }

  m_ready = false;
  m_promise = std::promise<bool>(); 
  
  m_jobMap.clear();
  m_taskMap.clear();
  m_threadIdSet.clear();
  m_threads.clear();

  for(Task *task : m_tasks) delete task;
  for(JobState *state : m_states) delete state;
  for(Job *job : m_jobs) delete job;

  m_tasks.clear();
  m_states.clear();
  m_jobs.clear();
}

