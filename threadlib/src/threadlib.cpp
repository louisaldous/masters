#include "JobState.h"
#include "ThreadPool.h"

#include <cassert>
#include <cstdlib>
#include <mutex>

#include <iostream>

#define THREADS 4 

using namespace threadlib;

static ThreadPool *g_globalThreadPool = nullptr;
static std::mutex m_initThreadPool;
static std::vector<void *> g_allocs;

extern "C" bool __enqueue_task(FunctionPtr func, FunctionPtr sequential, FunctionPtr continued, void* args, void* newScope, int64_t start, int64_t step, int64_t end){ 
  m_initThreadPool.lock();
  if(!g_globalThreadPool) g_globalThreadPool = new ThreadPool(THREADS);
  m_initThreadPool.unlock();

  bool success = true;

  g_globalThreadPool->addTask(func, args, newScope, start, step, end, sequential, continued);
  
  if(g_globalThreadPool->isMainThread()){   
    success = g_globalThreadPool->wait();  
    g_globalThreadPool->clear();  
  }

  return success;
}

extern "C" void __check_load_conflict(void *addr){
  assert(g_globalThreadPool && "globalThreadPool is nullptr!");
  Job *job = g_globalThreadPool->getJobInProgress();
  assert(job && "job pointer returned null!");

  job->getState()->doesLoadConflict(addr);
  job->getState()->addRead(addr);
}

extern "C" void __check_write_conflict(void *addr, int64_t size){
  assert(g_globalThreadPool && "globalThreadPool is nullptr!");
  Job *job = g_globalThreadPool->getJobInProgress();
  assert(job && "job pointer returned null!");

  job->getState()->doesStoreConflict(addr);
  job->getState()->addEntry(addr, (size_t)size);
}

extern "C" void* __malloc(int64_t size, int64_t num){
  std::scoped_lock lock(m_initThreadPool);
  void *addr = malloc((size_t) (size * num));
  g_allocs.push_back(addr);
  return addr;
}
