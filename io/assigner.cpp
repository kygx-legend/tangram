#include "io/assigner.hpp"

#include "core/queue_node_map.hpp"
#include "core/scheduler/control.hpp"
#include "base/color.hpp"

#include "base/get_ip.hpp"

#include <numeric>

namespace xyz {

bool Assigner::FinishBlock(FinishedBlock block) {
  CHECK(assigned_blocks_.find(block.block_id) != assigned_blocks_.end());
  auto url = assigned_blocks_[block.block_id].first;
  auto offset = assigned_blocks_[block.block_id].second;
  assigned_blocks_.erase(block.block_id);
  StoredBlock b{url, offset, block.node_id};
  finished_blocks_.insert({block.block_id, b});
  num_finished_ += 1;

  if (num_finished_ == expected_num_finished_) {
    LOG(INFO) << "Assigner: locality information: <has_locality, no_locality>: " 
      << locality_count_.first << ", " << locality_count_.second;
    return true;
  } else {
    std::pair<std::string, int> slave{block.hostname, block.node_id};
    Assign(block.collection_id, slave);
    return false;
  }
}

bool Assigner::Done() { return num_finished_ == expected_num_finished_; }

void Assigner::InitBlocks(std::string url) {
  CHECK_NOTNULL(browser_);
  auto blocks = browser_->Browse(url);
  for (auto& block : blocks) {
    std::pair<std::string, size_t> p{block.filename, block.offset};
    block.hostname = GetIP(block.hostname);
    locality_map_[block.hostname].insert(p);
    blocks_[p].push_back(block.hostname);
  }
  init_ = true;
}

int Assigner::GetNumBlocks() {
  CHECK(init_);
  return blocks_.size();
}

// return num of blocks
int Assigner::Load(int collection_id, std::string url,
                   std::vector<std::pair<std::string, int>> slaves,
                   std::vector<int> num_local_threads,
                   bool is_load_meta, bool is_whole_file) {
  is_load_meta_ = is_load_meta;
  is_whole_file_ = is_whole_file;
  CHECK_EQ(slaves.size(), num_local_threads.size());
  InitBlocks(url);
  num_finished_ = 0;
  num_assigned_ = 0;
  expected_num_finished_ = blocks_.size();
  // Need to ensure that there is no message coming before Load().

  LOG(INFO) << "Loading: " << url
            << ", as collection: " << collection_id
            << ", assigning " << RED(std::to_string(blocks_.size()))
            << " parititions to " << RED(std::to_string(slaves.size())) << " slaves";
  // TODO: use more scheduling sophisticatic algorithm
  /*
  // this algo didn't work well when locality are considered and 
  // the data blocks in machines are skew, as one machine with lots
  // of blocks will have read more blocks than others which will incur
  // skew computation latter.
  int index = 0;
  int num_total_slots = std::accumulate(num_local_threads.begin(), num_local_threads.end(), 0);
  int to_assign = std::min(int(blocks_.size()), num_total_slots);
  while (to_assign > 0) {
    if (num_local_threads[index] > 0) {
      bool suc = Assign(collection_id, slaves[index]);
      CHECK_EQ(suc, true);
      num_local_threads[index] -= 1;
      to_assign --;
    }
    index += 1;
    index %= num_local_threads.size();
  }
  */

  // this algo only considers balance machine power.
  int index = 0;
  int to_assign = int(blocks_.size());
  while (to_assign) {
    bool suc = Assign(collection_id, slaves[index]);
    CHECK_EQ(suc, true);
    to_assign --;
    index += 1;
    index %= num_local_threads.size();
  }
  return expected_num_finished_;
}

bool Assigner::Assign(int collection_id, std::pair<std::string, int> slave) {
  if (blocks_.empty()) {
    return false;
  }

  std::pair<std::string, size_t> block;
  // find block according to locality if any
  if (locality_map_.find(slave.first) != locality_map_.end() &&
      !locality_map_[slave.first].empty()) {
    block = *locality_map_[slave.first].begin();
    // LOG(INFO) << "has locality";
    locality_count_.first += 1;
  } else {
    block = blocks_.begin()->first;
    // LOG(INFO) << "no locality";
    locality_count_.second += 1;
  }

  // send
  SArrayBinStream ctrl_bin, bin;
  ScheduleFlag flag = ScheduleFlag::kLoadBlock;
  ctrl_bin << flag;
  AssignedBlock assigned_block;
  assigned_block.url = block.first;
  assigned_block.offset = block.second;
  assigned_block.id = block_id_++;
  assigned_block.collection_id = collection_id;
  assigned_block.is_load_meta = is_load_meta_;
  assigned_block.is_whole_file = is_whole_file_;
  bin << assigned_block;
  Message msg;
  msg.meta.sender = 0;
  msg.meta.recver = GetWorkerQid(slave.second);
  msg.meta.flag = Flag::kOthers;
  msg.AddData(ctrl_bin.ToSArray());
  msg.AddData(bin.ToSArray());
  sender_->Send(std::move(msg));
  VLOG(1) << "Assigining block: " << assigned_block.DebugString();

  // remove blocks info
  auto b = blocks_.find(block);
  CHECK(b != blocks_.end());
  auto locs = b->second;
  CHECK_GT(locs.size(), 0);
  for (auto loc : locs) {
    locality_map_[loc].erase(block);
  }
  blocks_.erase(block);
  num_assigned_ += 1;
  assigned_blocks_.insert({assigned_block.id, block});
  return true;
}

std::string Assigner::DebugStringLocalityMap() {
  std::stringstream ss;
  for (auto &kv : locality_map_) {
    ss << "hostname: " << kv.first;
    ss << "\n{";
    for (auto p : kv.second) {
      ss << "<" << p.first << ", " << p.second << "> ";
    }
    ss << "}\n";
  }
  return ss.str();
}

std::string Assigner::DebugStringBlocks() {
  std::stringstream ss;
  for (auto &kv : blocks_) {
    ss << "block: <" << kv.first.first << ", " << kv.first.second << ">: ";
    for (auto &h : kv.second) {
      ss << h << ", ";
    }
    ss << "\n";
  }
  return ss.str();
}

std::string Assigner::DebugStringFinishedBlocks() {
  std::stringstream ss;
  ss << "finished block:\n";
  for (auto &kv : finished_blocks_) {
    ss << "block id: " << kv.first;
    ss << " " << kv.second.DebugString();
    ss << "\n";
  }
  return ss.str();
}

} // namespace xyz
