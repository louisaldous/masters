#include "JobState.h"
#include "ThreadPool.h"

#include <iostream>
#include <cassert>
#include <thread>
#include <cstring>

using namespace threadlib;

JobState::JobState(ThreadPool *threadpool) : m_noConflicts(true), m_threadpool(threadpool){}

bool JobState::noConflicts(){
  std::scoped_lock lock(m_mutex);
  return m_noConflicts;
}

void JobState::doesLoadConflict(void *addr, bool updateStatus){ 
  const Timestamp &t = m_threadpool->getTimestampForCurrentThread();
  std::scoped_lock load(m_mutex);

  if(m_addrMap.find(addr) == m_addrMap.end()) return;

  // read by t1 to line written by t2 = conflict
  auto WAR = m_addrMap[addr].m_writes.upper_bound(&t);
  bool conflict = WAR != m_addrMap[addr].m_writes.end();

  //if(conflict) std::cout << "READ CONFLICT DETECTED\n";
  if(conflict && updateStatus) m_noConflicts = false; 
}

void JobState::doesStoreConflict(void *addr, bool updateStatus){
  const Timestamp &t = m_threadpool->getTimestampForCurrentThread();
  std::scoped_lock lock(m_mutex);

  if(m_addrMap.find(addr) == m_addrMap.end()) return;
  
  // read by t2 and then written by t1 = conflict
  auto RAW = m_addrMap[addr].m_reads.upper_bound(&t);
  bool conflict = RAW != m_addrMap[addr].m_reads.end();

  // write by t1 to a line written by t2
  if(!conflict){
    auto WAW = m_addrMap[addr].m_writes.upper_bound(&t);
    conflict = WAW != m_addrMap[addr].m_writes.end();
  }
  
  if(conflict) {
    //std::cout << "STORE CONFLICT DETECTED: " << addr << "\n";
  }

  if(conflict && updateStatus) {
    m_noConflicts = false;
  }
}

void JobState::addRead(void *addr){
  const Timestamp &t = m_threadpool->getTimestampForCurrentThread();
  std::scoped_lock lock(m_mutex);
  m_addrMap[addr].m_reads.insert(&t);
}

void JobState::addEntry(void *addr, size_t size){
  void **copyTo = nullptr;
  const Timestamp &t = m_threadpool->getTimestampForCurrentThread(); 
  {
  std::scoped_lock lock(m_mutex);

  bool newEntry = m_addrMap.find(addr) == m_addrMap.end();
  if(!newEntry) newEntry = m_addrMap[addr].m_writes.empty();

  if(m_noConflicts) {
    m_addrMap[addr].m_writes.insert(&t);
  }

  if(!newEntry || 
      (!m_noConflicts && m_rollback.find(addr) != m_rollback.end())) return;

  m_rollback[addr] = new VersionEntry(size, nullptr);
  copyTo = &m_rollback[addr]->m_addr;
  }

  // This is thread-safe with rollback() as all tasks must finish
  // before we rollback. As this is part of the task's call stack
  // we can guarantee that this is done before rollback()
  
  void *rollbackAddr = malloc((size_t)size);
  assert(copyTo && "copyTo is nullptr");

  *copyTo = rollbackAddr;
  std::memcpy(rollbackAddr, addr, size); 
}

void JobState::rollback(){
  std::scoped_lock lock(m_mutex);
  std::cout << "rolling back job\n";
  for(auto it: m_rollback){
    void *addr = it.first;
    VersionEntry *entry = it.second;

    std::memcpy(addr, entry->m_addr, entry->m_size);
  }
}

void JobState::printHistory(){
  std::cout << "Write map size: " << m_addrMap.size() << "\nEntries:\n";
  for(auto it : m_addrMap){
    std::cout << "\t" << it.first << ": Reads: " << it.second.m_reads.size() 
                                  << " Writes: " << it.second.m_writes.size() 
                                  << "\n";
  } 
}

void JobState::printRollback(){
  std::cout << "Rollback size: " << m_rollback.size() << "\nEntries:\n";
  for(auto it : m_rollback){
    std::cout << "\t" << it.first << ": " << it.second->m_size << "\n";
  }
}

