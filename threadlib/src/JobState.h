#ifndef JOBSTATE_H
#define JOBSTATE_H

#include <map>
#include <set>
#include <vector>
#include <mutex>

namespace threadlib { 
class ThreadPool;
using Timestamp = std::vector<int64_t>;

struct AddrHistory {
  std::set<const Timestamp *, std::greater<const Timestamp *>> m_writes;
  std::set<const Timestamp *, std::greater<const Timestamp *>> m_reads;
};

struct VersionEntry {
  size_t m_size;
  void *m_addr;

  VersionEntry(size_t size, void *addr) : m_size(size), m_addr(addr){}

  ~VersionEntry(){
    free(m_addr);
  }
};

class JobState {
public:
  JobState(ThreadPool *threadpool);

  ~JobState(){
    //for(auto it : m_addrMap){
    //  for(auto *entry : *it.second){
    //    delete entry;
    //  }
    //  delete it.second;
    //}

    for(auto it: m_rollback){
      delete it.second;
    }
  }

  bool noConflicts();

  void doesLoadConflict(void *addr, bool updateStatus = true);
  void doesStoreConflict(void *addr, bool updateStatus = true);

  void addRead(void *addr);
  void addEntry(void *addr, size_t size);

  void rollback();

  void printHistory();
  void printRollback();

public:
  bool m_noConflicts;
  std::mutex m_mutex;

protected:
  ThreadPool *m_threadpool;

  //std::map<void *, const Timestamp *> m_writeMap;
  std::map<void *, AddrHistory> m_addrMap;
  std::map<void *, VersionEntry *> m_rollback; 

  // TODO: CHANGE TO MAP
  //std::unordered_map<void *, std::list<VersionEntry *> *> m_addrMap;
};
}

#endif
