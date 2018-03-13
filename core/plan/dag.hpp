#pragma once

#include "glog/logging.h"

#include <map>
#include <set>
#include <vector>
#include <sstream>

namespace xyz {

struct Col {
  int id;  // the collection id
  std::vector<int> read_queue;  // the plan id that read the collection
  std::vector<int> write_queue;

  Col() = default;
  Col(int _id): id(_id) {}
  int GetLastWrite() const {
    return write_queue.empty() ? -1 : write_queue.back();
  }
  void AppendRead(int r) {
    read_queue.push_back(r);
  }
  void AppendWrite(int w) {
    write_queue.push_back(w);
  }
};

struct DagNode {
  int id;  // the plan id
  std::vector<int> out;
  std::vector<int> in;

  DagNode() = default;
  DagNode(int _id): id(_id) {}
  void AddOut(int i) {
    out.push_back(i);
  }
  void AddIn(int i) {
    in.push_back(i);
  }
  std::string DebugString() const;
};

class Dag {
 public:
  void AddDagNode(int id, std::vector<int> read, std::vector<int> write);

  std::string DebugString() const;
  friend class DagVistor;
 private:
  void AddEdge(int src, int dst);
  DagNode* GetOrCreateDagNode(int id);
  Col* GetOrCreateCol(int id);

 private:
  std::map<int, DagNode> nodes_;
  std::map<int, Col> cols_;
};

class DagVistor {
 public:
  DagVistor(const Dag& dag);
  void Finish(int id);
  std::vector<int> GetFront();
  int GetNumDagNodes() {
    return nodes_.size();
  }
 private:
  std::map<int, DagNode> nodes_;
  std::map<int, int> indegrees_;
  std::map<int, Col> cols_;
};

} // namespace xyz

