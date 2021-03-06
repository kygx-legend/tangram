#include "core/worker/plan_controller.hpp"
#include "core/scheduler/control.hpp"
#include "glog/logging.h"
#include "base/color.hpp"

#include <limits>
#include <stdlib.h>
//core/scheduler/control_manager.cpp; core/worker/plan_controller.cpp
#define BGCP false
// #define CPULIMIT

// for migrate update like pr, enable MIGRATE_JOIN and use stop_updateing_partitions_
// for migrate map like kmeans, disable stop_updateing_partitions_
// #define MIGRATE_JOIN 

namespace xyz {

PlanController::PlanController(Controller* controller)
  : controller_(controller) {
  fetch_executor_ = std::make_shared<Executor>(controller_->engine_elem_.num_update_threads);
  map_executor_ = std::make_shared<Executor>(controller_->engine_elem_.num_local_threads);
  local_map_mode_ = true;
}

PlanController::~PlanController() {
  ShowJoinTrackerSize();
  LOG(INFO) << RED("~PlanController: " + std::to_string(plan_id_));
}
 
void PlanController::Setup(SpecWrapper spec) {
  type_ = spec.type;
  CHECK(type_ == SpecWrapper::Type::kMapJoin
       || type_ == SpecWrapper::Type::kMapWithJoin);
  if (type_ == SpecWrapper::Type::kMapWithJoin) {
    auto* p = static_cast<MapWithJoinSpec*>(spec.spec.get());
    fetch_collection_id_ = p->with_collection_id;
  }
  auto* p = static_cast<MapJoinSpec*>(spec.spec.get());
  map_collection_id_ = p->map_collection_id;
  update_collection_id_ = p->update_collection_id;
  checkpoint_interval_ = p->checkpoint_interval;
  checkpoint_path_ = p->checkpoint_path;
  if (checkpoint_interval_ != 0) {
    CHECK(checkpoint_path_.size());
  }
  plan_id_ = spec.id;
  num_upstream_part_ = controller_->engine_elem_.collection_map->GetNumParts(map_collection_id_);
  num_update_part_ = controller_->engine_elem_.collection_map->GetNumParts(update_collection_id_);
  num_local_update_part_ = controller_->engine_elem_.partition_manager->GetNumLocalParts(update_collection_id_);
  num_local_map_part_ = controller_->engine_elem_.partition_manager->GetNumLocalParts(map_collection_id_);
  min_version_ = 0;
  staleness_ = p->staleness;
  expected_num_iter_ = p->num_iter;
  CHECK_NE(expected_num_iter_, 0);
  map_versions_.clear();
  update_versions_.clear();
  update_tracker_.clear();
  pending_updates_.clear();
  waiting_updates_.clear();
  int combine_timeout = p->combine_timeout;
  delayed_combiner_ = std::make_shared<DelayedCombiner>(this, combine_timeout);

  auto parts = controller_->engine_elem_.partition_manager->Get(map_collection_id_);
  for (auto& part : parts) {
    map_versions_[part->id] = 0;
  }
  parts = controller_->engine_elem_.partition_manager->Get(update_collection_id_);
  for (auto& part : parts) {
    update_versions_[part->id] = 0;
  }

  SArrayBinStream reply_bin;
  ControllerMsg ctrl;
  ctrl.flag = ControllerMsg::Flag::kSetup;
  ctrl.node_id = controller_->engine_elem_.node.id;
  ctrl.plan_id = plan_id_;
  ctrl.version = -1;
  reply_bin << ctrl;
  controller_->SendMsgToScheduler(reply_bin);
}

void PlanController::StartPlan() {
  TryRunSomeMaps();
}


void PlanController::UpdateVersion(SArrayBinStream bin) {
  int new_version;
  bin >> new_version;
  
#ifdef CPULIMIT 
  std::thread cpu_limit([this, new_version](){
    if (controller_->engine_elem_.node.id == 1 &&
        new_version == 1) {
      LOG(INFO) << RED("[PlanController::UpdateVersion] start to run cpulimit on node 1");
      // system("/data/opt/tmp/xuan/cpulimit/src/cpulimit -l 100 -e PageRankWith");
      system("/data/opt/tmp/xuan/cpulimit/src/cpulimit -l 1600 -e KmeansRowExample");
      //system("/data/yuzhen/utils/runAll.sh \"pkill cpulimit\"");
    }
  });
  cpu_limit.detach();
#endif

  CHECK_LT(new_version, expected_num_iter_);
  CHECK_EQ(new_version, min_version_+1);
  min_version_ = new_version;
  for (auto& part_version : update_tracker_) {
    part_version.second.erase(new_version-1);  // erase old update_tracker content
  }
  TryRunSomeMaps();
}

void PlanController::FinishMap(SArrayBinStream bin) {
  int part_id;
  bool update_version;
  bin >> part_id >> update_version;
  running_maps_.erase(part_id);

  {
    // the migrate partition is running map
    // when finished running map and stop_updateing is erasable
    std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
    if (stop_updateing_partitions_.find(part_id) != stop_updateing_partitions_.end() &&
        stop_updateing_partitions_[part_id]) {
      stop_updateing_partitions_.erase(part_id);
    }
  }

#ifdef MIGRATE_JOIN
  if (!update_version) {
    return;
  }
#endif

  if (map_versions_.find(part_id) == map_versions_.end()) {
    LOG(INFO) << "FinishMap for part not local";
    return;
  }
  int last_version = map_versions_[part_id];
  map_versions_[part_id] += 1;
  ReportFinishPart(ControllerMsg::Flag::kMap, part_id, map_versions_[part_id]);

  if (map_collection_id_ == update_collection_id_) {
    if (!pending_updates_[part_id][last_version].empty()) {
      while (!pending_updates_[part_id][last_version].empty()) {
        waiting_updates_[part_id].push_back(pending_updates_[part_id][last_version].front());
        pending_updates_[part_id][last_version].pop_front();
      }
      pending_updates_[part_id].erase(last_version);
    }
    // try run waiting updates if map_collection_id_ == update_collection_id_
    TryRunWaitingJoins(part_id);
  }
  // select other maps
  TryRunSomeMaps();
}

void PlanController::TryRunSomeMaps() {
  if (min_version_ == expected_num_iter_) {
    return;
  }

  for (auto kv : map_versions_) {
    int part_id = kv.first;
    if (IsMapRunnable(part_id)) {
      int version = kv.second;
      // 0. get partition
      CHECK(controller_->engine_elem_.partition_manager->Has(map_collection_id_, part_id));
      auto p = controller_->engine_elem_.partition_manager->Get(map_collection_id_, part_id);
      RunMap(part_id, version, p);
    }
  }
}

bool PlanController::IsMapRunnable(int part_id) {
  // 1. check whether there is running map for this part
  if (running_maps_.find(part_id) != running_maps_.end()) {
    return false;
  }
  // 2. if mid == jid, check whether there is running update for this part
  if (map_collection_id_ == update_collection_id_
          && running_updates_.find(part_id) != running_updates_.end()) {
    return false;
  }
  // 3. if mid == jid, check whether this part is migrating
  {
    std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
    if (map_collection_id_ == update_collection_id_
            && stop_updateing_partitions_.find(part_id) != stop_updateing_partitions_.end()) {
      return false;
    }
  }
  // 4. check version
  int version = map_versions_[part_id];
  if (version == expected_num_iter_) {  // no need to run anymore
    return false;
  }
  if (version <= min_version_ + staleness_) {
    return true;
  } else {
    return false;
  }
}

bool PlanController::TryRunWaitingJoins(int part_id) {
  // if some fetches are in the waiting_updates_, 
  // update_collection_id_ == fetch_collection_id_
#ifdef MIGRATE_JOIN
  {
    std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
    if (stop_updateing_partitions_.find(part_id) != stop_updateing_partitions_.end()) {
      // if this part is migrating, do not update or fetch
      return false;
    }
  }
#endif
  
  if (running_updates_.find(part_id) != running_updates_.end()) {
    // someone is updateing this part
    return false;
  }

  if (fetch_collection_id_ == update_collection_id_ 
          && running_fetches_[part_id] > 0) {
    // someone is fetching this part
    return false;
  }

  if (map_collection_id_ == update_collection_id_ 
          && running_maps_.find(part_id) != running_maps_.end()) {
    // someone is mapping this part
    return false;
  }

  // see whether there is any waiting updates
  auto& updates = waiting_updates_[part_id];
  if (!updates.empty()) {
    VersionedJoinMeta meta = updates.front();
    updates.pop_front();
    if (!meta.meta.is_fetch) {
      RunJoin(meta);
    } else {
      RunFetchRequest(meta);
    }
    return true;
  }
  return false;
}

void PlanController::FinishJoin(SArrayBinStream bin) {
  int part_id, version;
  std::vector<int> upstream_part_ids;
  bin >> part_id >> version >> upstream_part_ids;
  // LOG(INFO) << "FinishJoin: partid, version: " << part_id << " " << version;
  running_updates_.erase(part_id);

  for (auto upstream_part_id : upstream_part_ids) {
    update_tracker_[part_id][version].insert(upstream_part_id);
  }
  if (update_tracker_[part_id][version].size() == num_upstream_part_) {
    CalcJoinTrackerSize();
    update_tracker_[part_id][version].clear(); //
    update_versions_[part_id] += 1;

    bool runcp = TryCheckpoint(part_id);
    if (runcp) {
      // if runcp, whether BGCP or not, ReportFinishPart after checkpoint
      return;
    } else {
      ReportFinishPart(ControllerMsg::Flag::kJoin, part_id, update_versions_[part_id]);
    }
  }
  TryRunWaitingJoins(part_id);
}

bool PlanController::TryCheckpoint(int part_id) {
  /*
  if (update_versions_[part_id] == expected_num_iter_) {
    // TODO: ignore the last one
    return false;
  }
  */
  if (checkpoint_interval_ != 0 && update_versions_[part_id] % checkpoint_interval_ == 0) {
    int checkpoint_iter = update_versions_[part_id] / checkpoint_interval_;
    std::string dest_url = checkpoint_path_ + 
        "/cp-" + std::to_string(checkpoint_iter);
    dest_url = GetCheckpointUrl(dest_url, update_collection_id_, part_id);

    CHECK(running_updates_.find(part_id) == running_updates_.end());
    running_updates_.insert({part_id, -1});
    // TODO: is it ok to use the fetch_executor? can it be block due to other operations?
    fetch_executor_->Add([this, part_id, dest_url]() {
      CHECK(controller_->engine_elem_.partition_manager->Has(update_collection_id_, part_id));
      auto part = controller_->engine_elem_.partition_manager->Get(update_collection_id_, part_id);

      SArrayBinStream bin;
      part->ToBin(bin);
	  if (!BGCP) { //No Backgroud CP
        auto writer = controller_->io_wrapper_->GetWriter();
        bool rc = writer->Write(dest_url, bin.GetPtr(), bin.Size());
        CHECK_EQ(rc, 0);
        LOG(INFO) << "write checkpoint to " << dest_url;
   	  } else {
	    std::thread cp_write([this, bin, dest_url, part_id]() {
	      boost::shared_lock<boost::shared_mutex> lk(controller_->erase_mu_);
		  if (controller_->erased[plan_id_]) {
		    LOG(INFO) << BLUE("Plan " + std::to_string(plan_id_) + " has been erased, stop cp write");
		    return;
		  }
          auto writer = controller_->io_wrapper_->GetWriter();
          bool rc = writer->Write(dest_url, bin.GetPtr(), bin.Size());
          CHECK_EQ(rc, 0);
          LOG(INFO) << "BGCP: writing checkpoint to " << dest_url << ", node: " << controller_->engine_elem_.node.id;
  	      ReportFinishPart(ControllerMsg::Flag::kFinishCP, part_id, update_versions_[part_id]);
		});
		cp_write.detach();
	  }
	  // Send finish checkpoint
      Message msg;
      msg.meta.sender = 0;
      msg.meta.recver = 0;
      msg.meta.flag = Flag::kOthers;
      SArrayBinStream ctrl_bin, plan_bin, reply_bin;
      ctrl_bin << ControllerFlag::kFinishCheckpoint;
      plan_bin << plan_id_; 
      reply_bin << part_id;

      msg.AddData(ctrl_bin.ToSArray());
      msg.AddData(plan_bin.ToSArray());
      msg.AddData(reply_bin.ToSArray());
      controller_->GetWorkQueue()->Push(msg);
    });
    return true;
  } else {
    return false;
  }
}

void PlanController::FinishCheckpoint(SArrayBinStream bin) {
  int part_id;
  bin >> part_id;
  LOG(INFO) << "finish checkpoint: " << part_id;
  CHECK(running_updates_.find(part_id) != running_updates_.end());
  running_updates_.erase(part_id);
  ReportFinishPart(ControllerMsg::Flag::kJoin, part_id, update_versions_[part_id]);
  TryRunWaitingJoins(part_id);
  TryRunSomeMaps();
}

void PlanController::ReportFinishPart(ControllerMsg::Flag flag, 
        int part_id, int version) {
  SArrayBinStream bin;
  ControllerMsg ctrl;
  ctrl.flag = flag;
  ctrl.version = version;
  ctrl.node_id = controller_->engine_elem_.node.id;
  ctrl.plan_id = plan_id_;
  ctrl.part_id = part_id;
  bin << ctrl;
  controller_->SendMsgToScheduler(bin);
}

void PlanController::RunMap(int part_id, int version, 
        std::shared_ptr<AbstractPartition> p) {
  CHECK(running_maps_.find(part_id) == running_maps_.end());
  running_maps_.insert(part_id);
  
  std::function<void(int, int, Controller*)> AbortMap = [](int plan_id_, int part_id, Controller* controller_){
    //map a partition to be migrated
    //avoid a part of ignore messages
    //need to send a finish map message
    LOG(INFO) << "[RunMap] map task on a migrating partition: " << part_id <<" submitted, stop map and send finish map msg";
    Message msg;
    msg.meta.sender = 0;
    msg.meta.recver = 0;
    msg.meta.flag = Flag::kOthers;
    SArrayBinStream ctrl_bin, plan_bin, bin;
    ctrl_bin << ControllerFlag::kFinishMap;
    bool update_version = false;
    plan_bin << plan_id_;
    bin << part_id << update_version;
    msg.AddData(ctrl_bin.ToSArray());
    msg.AddData(plan_bin.ToSArray());
    msg.AddData(bin.ToSArray());
    controller_->GetWorkQueue()->Push(msg);
  };

  map_executor_->Add([this, part_id, version, p, AbortMap]() {
    // auto start = std::chrono::system_clock::now();
    {
      std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
      if (stop_updateing_partitions_.find(part_id) != stop_updateing_partitions_.end()) {
        AbortMap(plan_id_, part_id, controller_);
        return;
      }
    }
    // 1. map
    std::shared_ptr<AbstractMapOutput> map_output;
    if (type_ == SpecWrapper::Type::kMapJoin) {
      auto& map = controller_->engine_elem_.function_store->GetMap(GetRealId(plan_id_));
      map_output = map(p); 
    } else if (type_ == SpecWrapper::Type::kMapWithJoin){
      auto& mapwith = controller_->engine_elem_.function_store->GetMapWith(GetRealId(plan_id_));
      map_output = mapwith(plan_id_, version, p, controller_->engine_elem_.fetcher); 
    } else {
      CHECK(false);
    }
  
    {
      std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
      if (stop_updateing_partitions_.find(part_id) != stop_updateing_partitions_.end()) {
        AbortMap(plan_id_, part_id, controller_);
        return;
      }
    }
    // 2. send to delayed_combiner
    // auto start2 = std::chrono::system_clock::now();
    CHECK(delayed_combiner_);
    delayed_combiner_->AddMapOutput(part_id, version, map_output);
    Message msg;
    msg.meta.sender = 0;
    msg.meta.recver = 0;
    msg.meta.flag = Flag::kOthers;
    SArrayBinStream ctrl_bin, plan_bin, bin;
    ctrl_bin << ControllerFlag::kFinishMap;
    bool update_version = true;
    plan_bin << plan_id_;
    bin << part_id << update_version;
    msg.AddData(ctrl_bin.ToSArray());
    msg.AddData(plan_bin.ToSArray());
    msg.AddData(bin.ToSArray());
    controller_->GetWorkQueue()->Push(msg);
    
    /*
    auto end = std::chrono::system_clock::now();
    {
      std::unique_lock<std::mutex> lk(time_mu_);
      map_time_[part_id] = std::make_tuple(start, start2, end);
    }
    */
  });
}

void PlanController::ReceiveJoin(Message msg) {
  CHECK_EQ(msg.data.size(), 4);
  SArrayBinStream ctrl2_bin, bin;
  ctrl2_bin.FromSArray(msg.data[2]);
  bin.FromSArray(msg.data[3]);
  VersionedShuffleMeta meta;
  ctrl2_bin >> meta;
  // LOG(INFO) << "ReceiveJoin: " << meta.DebugString();

  VersionedJoinMeta update_meta;
  update_meta.meta = meta;
  update_meta.bin = bin;

  if (update_versions_.find(meta.part_id) == update_versions_.end()) {
    // if receive something that is not belong to here
    buffered_requests_[meta.part_id].push_back(update_meta);
    //LOG(INFO) << "part to migrate: " << meta.part_id <<  ", buffered requsts size: " << buffered_requests_[meta.part_id].size();
    return;
  }

  // if already updateed, omit it.
  // still need to check again in RunJoin as this upstream_part_id may be in waiting updates.
  if (IsJoinedBefore(meta)) {
    return;
  }

  if (map_collection_id_ == update_collection_id_) {
    if (meta.version >= map_versions_[meta.part_id]) {
      pending_updates_[meta.part_id][meta.version].push_back(update_meta);
      return;
    }
  }

  waiting_updates_[meta.part_id].push_back(update_meta);
  TryRunWaitingJoins(meta.part_id);
}

// check whether this upstream_part_id is updateed already
bool PlanController::IsJoinedBefore(const VersionedShuffleMeta& meta) {
  if (meta.upstream_part_id == -1) {
    CHECK_GT(meta.ext_upstream_part_ids.size(), 0);
    bool result = update_tracker_[meta.part_id][meta.version].find(meta.ext_upstream_part_ids.at(0))
      != update_tracker_[meta.part_id][meta.version].end();
    for (int i = 1; i < meta.ext_upstream_part_ids.size(); i++) {
      CHECK_EQ(result, 
          update_tracker_[meta.part_id][meta.version].find(meta.ext_upstream_part_ids.at(i))
          != update_tracker_[meta.part_id][meta.version].end());
    }
    if (result) {
      LOG(INFO) << "[PlanController::IsJoinedBefore] (ext_upstream_part_ids)ignore update, already updateed: " << meta.DebugString();
    } 
    return result;
  }

  if (update_tracker_[meta.part_id][meta.version].find(meta.upstream_part_id)
      != update_tracker_[meta.part_id][meta.version].end()) {
    LOG(INFO) << "[PlanController::IsJoinedBefore] ignore update, already updateed: " << meta.DebugString();
    return true;
  } else {
    return false;
  }
}

void PlanController::RunJoin(VersionedJoinMeta meta) {
  // LOG(INFO) << meta.meta.DebugString();
  if (IsJoinedBefore(meta.meta)) {
    // Need to TryRunWaitingJoins
    TryRunWaitingJoins(meta.meta.part_id);
    return;
  }
  // LOG(INFO) << "RunJoin: " << meta.meta.DebugString();
  CHECK(running_updates_.find(meta.meta.part_id) == running_updates_.end());
  running_updates_.insert({meta.meta.part_id, meta.meta.upstream_part_id});
  // use the fetch_executor to avoid the case:
  // map wait for fetch, fetch wait for update, update is in running_updates_
  // but it cannot run because map does not finish and occupy the threadpool
  fetch_executor_->Add([this, meta]() {
    // auto start = std::chrono::system_clock::now();

    if (local_map_mode_ && meta.meta.local_mode) {
      std::tuple<int, std::vector<int>, int> k;
      if (meta.meta.ext_upstream_part_ids.empty()) {
        k = std::make_tuple(meta.meta.part_id, std::vector<int>{meta.meta.upstream_part_id}, meta.meta.version);
      } else {
        k = std::make_tuple(meta.meta.part_id, meta.meta.ext_upstream_part_ids, meta.meta.version);
      }
      auto stream = stream_store_.Get(k);
      stream_store_.Remove(k);
      auto& update_func = controller_->engine_elem_.function_store->GetJoin2(GetRealId(plan_id_));
      CHECK(controller_->engine_elem_.partition_manager->Has(update_collection_id_, meta.meta.part_id));
      auto p = controller_->engine_elem_.partition_manager->Get(update_collection_id_, meta.meta.part_id);
      update_func(p, stream);
    } else {
      auto& update_func = controller_->engine_elem_.function_store->GetJoin(GetRealId(plan_id_));
      CHECK(controller_->engine_elem_.partition_manager->Has(update_collection_id_, meta.meta.part_id));
      auto p = controller_->engine_elem_.partition_manager->Get(update_collection_id_, meta.meta.part_id);
      update_func(p, meta.bin);
    }

    Message msg;
    msg.meta.sender = 0;
    msg.meta.recver = 0;
    msg.meta.flag = Flag::kOthers;
    SArrayBinStream ctrl_bin, plan_bin, bin;
    ctrl_bin << ControllerFlag::kFinishJoin;
    plan_bin << plan_id_; 
    bin << meta.meta.part_id;
    bin << meta.meta.version; 
    if (meta.meta.ext_upstream_part_ids.empty()) {
      bin << std::vector<int>{meta.meta.upstream_part_id};  // still need to make it vector
    } else {
      bin << meta.meta.ext_upstream_part_ids;
    }

    msg.AddData(ctrl_bin.ToSArray());
    msg.AddData(plan_bin.ToSArray());
    msg.AddData(bin.ToSArray());
    controller_->GetWorkQueue()->Push(msg);
    
    /*
    auto end = std::chrono::system_clock::now();
    {
      std::unique_lock<std::mutex> lk(time_mu_);
      update_time_[meta.meta.part_id][meta.meta.upstream_part_id] = std::make_pair(start, end);
    }
    */
  });
}

void PlanController::ReceiveFetchRequest(Message msg) {
  CHECK_EQ(msg.data.size(), 3);
  SArrayBinStream ctrl2_bin, bin;
  ctrl2_bin.FromSArray(msg.data[1]);
  bin.FromSArray(msg.data[2]);
  FetchMeta received_fetch_meta;
  ctrl2_bin >> received_fetch_meta;
  CHECK(received_fetch_meta.plan_id == plan_id_);
  CHECK(controller_->engine_elem_.partition_manager->Has(received_fetch_meta.collection_id, received_fetch_meta.partition_id)) 
      << "cid: " << received_fetch_meta.collection_id << ", pid: " << received_fetch_meta.partition_id;

  VersionedJoinMeta fetch_meta;
  fetch_meta.meta.plan_id = plan_id_;
  fetch_meta.meta.collection_id = received_fetch_meta.collection_id;
  CHECK(fetch_meta.meta.collection_id == fetch_collection_id_);
  fetch_meta.meta.part_id = received_fetch_meta.partition_id;
  fetch_meta.meta.upstream_part_id = received_fetch_meta.upstream_part_id;
  fetch_meta.meta.version = received_fetch_meta.version;  // version -1 means fetch objs, others means fetch part
  fetch_meta.meta.is_fetch = true;
  fetch_meta.meta.local_mode = received_fetch_meta.local_mode;
  fetch_meta.bin = bin;
  fetch_meta.meta.sender = msg.meta.sender;
  CHECK_EQ(msg.meta.recver, controller_->Qid());
  fetch_meta.meta.recver = msg.meta.recver;
  
  CHECK(fetch_meta.meta.is_fetch == true);

  bool update_fetch = (fetch_meta.meta.collection_id == update_collection_id_);

  if (!update_fetch) {
    RunFetchRequest(fetch_meta);
    return;
  }

  // otherise update_fetch
  CHECK(update_fetch);

  if (update_versions_.find(fetch_meta.meta.part_id) == update_versions_.end()) {
    // if receive something that is not belong to here
    buffered_requests_[fetch_meta.meta.part_id].push_back(fetch_meta);
    return;
  }

#ifdef MIGRATE_JOIN
  {
    std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
    if (stop_updateing_partitions_.find(fetch_meta.meta.part_id) != stop_updateing_partitions_.end()) {
      // if migrating this part
      waiting_updates_[fetch_meta.meta.part_id].push_back(fetch_meta);
      return;
    }
  }
#endif

  if (running_updates_.find(fetch_meta.meta.part_id) != running_updates_.end()) {
    // if this part is updateing
    waiting_updates_[fetch_meta.meta.part_id].push_back(fetch_meta);
  } else {
    RunFetchRequest(fetch_meta);
  }
}

void PlanController::RunFetchRequest(VersionedJoinMeta fetch_meta) {
  running_fetches_[fetch_meta.meta.part_id] += 1;

  // identify the version
  int version = -1;
  if (fetch_collection_id_ == update_collection_id_) {
    version = update_versions_[fetch_meta.meta.part_id];
  } else {
    version = std::numeric_limits<int>::max();
  }

  bool local_fetch = (GetNodeId(fetch_meta.meta.sender) == GetNodeId(fetch_meta.meta.recver));
  // LOG(INFO) << "run fetch: " << local_fetch << " " << fetch_meta.meta.version;
  if (fetch_meta.meta.local_mode && fetch_meta.meta.version != -1 && local_fetch) {  // for local fetch part
    // grant access to the fetcher
    // the fetcher should send kFinishFetch explicitly to release the fetch
    Message reply_msg;
    reply_msg.meta.sender = fetch_meta.meta.recver;
    reply_msg.meta.recver = fetch_meta.meta.sender;
    reply_msg.meta.flag = Flag::kOthers;
    SArrayBinStream ctrl_reply_bin, ctrl2_reply_bin;
    ctrl_reply_bin << FetcherFlag::kFetchPartReplyLocal;
    FetchMeta meta;
    meta.plan_id = plan_id_;
    meta.upstream_part_id = fetch_meta.meta.upstream_part_id;
    meta.collection_id = fetch_meta.meta.collection_id;
    meta.partition_id = fetch_meta.meta.part_id;
    meta.version = version;
    ctrl2_reply_bin << meta;
    reply_msg.AddData(ctrl_reply_bin.ToSArray());
    reply_msg.AddData(ctrl2_reply_bin.ToSArray());
    controller_->engine_elem_.sender->Send(std::move(reply_msg));
    // fetcher should release the fetch
  } else {
    fetch_executor_->Add([this, fetch_meta, version] {
      Fetch(fetch_meta, version);
    });
  }

}

void PlanController::Fetch(VersionedJoinMeta fetch_meta, int version) {
  CHECK(controller_->engine_elem_.partition_manager->Has(fetch_meta.meta.collection_id, fetch_meta.meta.part_id)) << fetch_meta.meta.collection_id << " " <<  fetch_meta.meta.part_id;
  auto part = controller_->engine_elem_.partition_manager->Get(fetch_meta.meta.collection_id, fetch_meta.meta.part_id);
  SArrayBinStream reply_bin;
  if (fetch_meta.meta.version == -1) {  // fetch objs
    auto& func = controller_->engine_elem_.function_store->GetGetter(fetch_meta.meta.collection_id);
    reply_bin = func(fetch_meta.bin, part);
  } else {  // fetch part
    part->ToBin(reply_bin);
  }
  // reply, send to fetcher
  Message reply_msg;
  reply_msg.meta.sender = fetch_meta.meta.recver;
  reply_msg.meta.recver = fetch_meta.meta.sender;
  reply_msg.meta.flag = Flag::kOthers;
  SArrayBinStream ctrl_reply_bin, ctrl2_reply_bin;
  if (fetch_meta.meta.version == -1) {  // fetch objs
    ctrl_reply_bin << FetcherFlag::kFetchObjsReply;
  } else {  // fetch part
    ctrl_reply_bin << FetcherFlag::kFetchPartReplyRemote;
  }
  FetchMeta meta;
  meta.plan_id = plan_id_;
  meta.upstream_part_id = fetch_meta.meta.upstream_part_id;
  meta.collection_id = fetch_meta.meta.collection_id;
  meta.partition_id = fetch_meta.meta.part_id;
  meta.version = version;
  ctrl2_reply_bin << meta;
  reply_msg.AddData(ctrl_reply_bin.ToSArray());
  reply_msg.AddData(ctrl2_reply_bin.ToSArray());
  reply_msg.AddData(reply_bin.ToSArray());
  controller_->engine_elem_.sender->Send(std::move(reply_msg));
  
  // send to controller
  Message msg;
  msg.meta.sender = 0;
  msg.meta.recver = 0;
  msg.meta.flag = Flag::kOthers;
  SArrayBinStream ctrl_bin, plan_bin, bin;
  ctrl_bin << ControllerFlag::kFinishFetch;
  plan_bin << plan_id_;
  bin << fetch_meta.meta.part_id << fetch_meta.meta.upstream_part_id;
  msg.AddData(ctrl_bin.ToSArray());
  msg.AddData(plan_bin.ToSArray());
  msg.AddData(bin.ToSArray());
  controller_->GetWorkQueue()->Push(msg);
}

void PlanController::FinishFetch(SArrayBinStream bin) {
  int part_id, upstream_part_id;
  bin >> part_id >> upstream_part_id;
  // LOG(INFO) << "FinishFetch: " << part_id << " " << upstream_part_id;
  running_fetches_[part_id] -= 1;
  TryRunWaitingJoins(part_id);
}

void PlanController::MigratePartition(Message msg) {
  CHECK_GE(msg.data.size(), 3);
  SArrayBinStream ctrl2_bin, bin;
  ctrl2_bin.FromSArray(msg.data[2]);
  MigrateMeta migrate_meta;
  ctrl2_bin >> migrate_meta;
  if (migrate_meta.flag == MigrateMeta::MigrateFlag::kStartMigrate) {
    // update collection_map
    CollectionView collection_view;
    ctrl2_bin >> collection_view;
    {
      std::lock_guard<std::mutex> lk(migrate_mu_);
      controller_->engine_elem_.collection_map->Insert(collection_view);
      // send msg to from_id
      MigratePartitionStartMigrate(migrate_meta);
    }
  } else if (migrate_meta.flag == MigrateMeta::MigrateFlag::kFlushAll) {
    MigratePartitionReceiveFlushAll(migrate_meta);
  } else if (migrate_meta.flag == MigrateMeta::MigrateFlag::kDest){
    MigratePartitionDest(msg);
  } else if (migrate_meta.flag == MigrateMeta::MigrateFlag::kStartMigrateMapOnly){
    MigratePartitionStartMigrateMapOnly(migrate_meta);
  } else if (migrate_meta.flag == MigrateMeta::MigrateFlag::kReceiveMapOnly){
    MigratePartitionReceiveMapOnly(msg);
  } else {
    CHECK(false);
  }
}

void PlanController::MigratePartitionStartMigrate(MigrateMeta migrate_meta) {
  migrate_meta.flag = MigrateMeta::MigrateFlag::kFlushAll;
  Message flush_msg;
  flush_msg.meta.sender = controller_->engine_elem_.node.id;
  flush_msg.meta.recver = GetControllerActorQid(migrate_meta.from_id);
  flush_msg.meta.flag = Flag::kOthers;
  SArrayBinStream ctrl_bin, plan_bin, ctrl2_bin;
  ctrl_bin << ControllerFlag::kMigratePartition;
  plan_bin << plan_id_;
  ctrl2_bin << migrate_meta;
  flush_msg.AddData(ctrl_bin.ToSArray());
  flush_msg.AddData(plan_bin.ToSArray());
  flush_msg.AddData(ctrl2_bin.ToSArray());
  controller_->engine_elem_.sender->Send(std::move(flush_msg));
  LOG(INFO) << "[Migrate] Send Flush signal on node: " << controller_->engine_elem_.node.id 
      << ": " << migrate_meta.DebugString();

  if (migrate_meta.from_id == controller_->engine_elem_.node.id) {
    // stop the update to the partition.
    CHECK_EQ(migrate_meta.collection_id, update_collection_id_) << "the migrate collection must be the update collection";
    // TODO: now the migrate partition must be update_collection
    CHECK(update_versions_.find(migrate_meta.partition_id) != update_versions_.end());
    {
      std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
      CHECK(stop_updateing_partitions_.find(migrate_meta.partition_id) == stop_updateing_partitions_.end());
      // insert the migrate partition into stop_updateing_partitions_
      stop_updateing_partitions_[migrate_meta.partition_id] = false;
    }
  }
}

void PlanController::MigratePartitionReceiveFlushAll(MigrateMeta migrate_meta) {
  CHECK_EQ(migrate_meta.from_id, controller_->engine_elem_.node.id) << "only w_a receives FlushAll";
  if (flush_all_count_.find(migrate_meta.partition_id) == flush_all_count_.end()){
    flush_all_count_[migrate_meta.partition_id] = 0;
  };
  flush_all_count_[migrate_meta.partition_id] += 1;
  LOG(INFO) << "[Migrate] Received one Flush signal";
  if (flush_all_count_[migrate_meta.partition_id] == migrate_meta.num_nodes) {
    if (running_updates_.find(migrate_meta.partition_id) != running_updates_.end()) {
      // if there is a update/fetch task for this part
      // push a msg to the msg queue to run this function again
      LOG(INFO) << "there is a update/fetch for part, try to migrate partition and msgs later " << migrate_meta.partition_id;
      flush_all_count_[migrate_meta.partition_id] -= 1;
      CHECK(migrate_meta.flag == MigrateMeta::MigrateFlag::kFlushAll);
      Message flush_msg;
      flush_msg.meta.sender = controller_->engine_elem_.node.id;
      flush_msg.meta.recver = controller_->engine_elem_.node.id;
      flush_msg.meta.flag = Flag::kOthers;
      SArrayBinStream ctrl_bin, plan_bin, ctrl2_bin;
      ctrl_bin << ControllerFlag::kMigratePartition;
      plan_bin << plan_id_;
      ctrl2_bin << migrate_meta;
      flush_msg.AddData(ctrl_bin.ToSArray());
      flush_msg.AddData(plan_bin.ToSArray());
      flush_msg.AddData(ctrl2_bin.ToSArray());
      controller_->GetWorkQueue()->Push(flush_msg);
      return;
    }
    flush_all_count_[migrate_meta.partition_id] = 0;

    // flush the buffered data structure to to_id
    {
      std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
      CHECK(stop_updateing_partitions_.find(migrate_meta.partition_id) != 
          stop_updateing_partitions_.end());
      // stop_updateing is erasable from now on
      stop_updateing_partitions_[migrate_meta.partition_id] = true;
      if (running_maps_.find(migrate_meta.partition_id) != running_maps_.end()) {
        // migrate a partition in running_maps
        // delay, leave it to RunMap to erase
        // RunMap will call FinishMap to erase it whether aborted or not
      } else {
        stop_updateing_partitions_.erase(migrate_meta.partition_id);
      }
    }
    LOG(INFO) << "[Migrate] Received all Flush signal for partition "
      << migrate_meta.partition_id <<", send everything to dest";
    // now I don't care about the complexity
    // set the flag
    migrate_meta.flag = MigrateMeta::MigrateFlag::kDest;
    // construct the MigrateData
    MigrateData data;
    if (map_collection_id_ == update_collection_id_) {
      data.map_version = map_versions_[migrate_meta.partition_id];
      map_versions_.erase(migrate_meta.partition_id);
      num_local_map_part_ -= 1;
      // TryUpdateMapVersion();
    }
    data.update_version = update_versions_[migrate_meta.partition_id];
    data.pending_updates = std::move(pending_updates_[migrate_meta.partition_id]);
    data.waiting_updates = std::move(waiting_updates_[migrate_meta.partition_id]);

    auto serialize_from_stream_store = [this](VersionedJoinMeta& meta) {
      std::tuple<int, std::vector<int>, int> k;
      if (meta.meta.ext_upstream_part_ids.empty()) {
        k = std::make_tuple(meta.meta.part_id, std::vector<int>{meta.meta.upstream_part_id}, meta.meta.version);
      } else {
        k = std::make_tuple(meta.meta.part_id, meta.meta.ext_upstream_part_ids, meta.meta.version);
      }
      auto stream = stream_store_.Get(k);
      stream_store_.Remove(k);
      auto bin = stream->Serialize();
      meta.bin = bin;
      meta.meta.local_mode = false;
    };
    for (auto& version_updates : data.pending_updates) {
      for (auto& update_meta : version_updates.second) {
        if (local_map_mode_ && update_meta.meta.local_mode) {
          serialize_from_stream_store(update_meta);
        }
      } 
    }
    for (auto& update_meta : data.waiting_updates) {
      if (local_map_mode_ && update_meta.meta.local_mode) {
        serialize_from_stream_store(update_meta);
      }
    } 
    data.update_tracker = std::move(update_tracker_[migrate_meta.partition_id]);
    update_versions_.erase(migrate_meta.partition_id);
    num_local_update_part_ -= 1;
    pending_updates_.erase(migrate_meta.partition_id);
    waiting_updates_.erase(migrate_meta.partition_id);
    update_tracker_.erase(migrate_meta.partition_id);
    Message msg;
    msg.meta.sender = controller_->engine_elem_.node.id;
    msg.meta.recver = GetControllerActorQid(migrate_meta.to_id);
    msg.meta.flag = Flag::kOthers;
    SArrayBinStream ctrl_bin, plan_bin, ctrl2_bin, bin1, bin2;
    ctrl_bin << ControllerFlag::kMigratePartition;
    plan_bin << plan_id_;
    ctrl2_bin << migrate_meta;
    bin1 << data;

    // construct the partition bin
    CHECK(controller_->engine_elem_.partition_manager->Has(
      migrate_meta.collection_id, migrate_meta.partition_id)) << migrate_meta.collection_id << " " <<  migrate_meta.partition_id;
    auto part = controller_->engine_elem_.partition_manager->Get(
      migrate_meta.collection_id, migrate_meta.partition_id);
    part->ToBin(bin2);  // serialize
    controller_->engine_elem_.partition_manager->Remove(migrate_meta.collection_id, migrate_meta.partition_id);

    msg.AddData(ctrl_bin.ToSArray());
    msg.AddData(plan_bin.ToSArray());
    msg.AddData(ctrl2_bin.ToSArray());
    msg.AddData(bin1.ToSArray());
    msg.AddData(bin2.ToSArray());
    controller_->engine_elem_.sender->Send(std::move(msg));
  }
}

void PlanController::MigratePartitionDest(Message msg) {
  CHECK_EQ(msg.data.size(), 5);
  SArrayBinStream ctrl2_bin, bin1, bin2;
  ctrl2_bin.FromSArray(msg.data[2]);
  bin1.FromSArray(msg.data[3]);
  bin2.FromSArray(msg.data[4]);
  MigrateMeta migrate_meta;
  MigrateData migrate_data;
  ctrl2_bin >> migrate_meta;
  bin1 >> migrate_data;
  CHECK_EQ(migrate_meta.to_id, controller_->engine_elem_.node.id) << "only w_b receive this";

  {
    if (fetch_collection_id_ != -1 &&
        map_collection_id_ == update_collection_id_ &&
        map_collection_id_ != fetch_collection_id_ &&
        (load_finished_.find(migrate_meta.partition_id) == load_finished_.end() || !load_finished_[migrate_meta.partition_id])
       ) { // only for mapwith and co-allocated
      // if load cp has not finished for this part
      // push a msg to the msg queue to run this function again
      std::thread migrate_thread([this, migrate_meta, bin1, bin2](){
        LOG(INFO) << "load cp has not finished for part: " << migrate_meta.partition_id <<", try to run later";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Message msg;
        msg.meta.sender = controller_->engine_elem_.node.id;
        msg.meta.recver = controller_->engine_elem_.node.id;
        msg.meta.flag = Flag::kOthers;
        SArrayBinStream ctrl_bin, plan_bin, ctrl2_bin;
        ctrl_bin << ControllerFlag::kMigratePartition;
        plan_bin << plan_id_;
        ctrl2_bin << migrate_meta;
        CHECK(migrate_meta.flag == MigrateMeta::MigrateFlag::kDest);
        msg.AddData(ctrl_bin.ToSArray());
        msg.AddData(plan_bin.ToSArray());
        msg.AddData(ctrl2_bin.ToSArray());
        msg.AddData(bin1.ToSArray());
        msg.AddData(bin2.ToSArray());
        controller_->GetWorkQueue()->Push(msg);
      });
      migrate_thread.detach();
      return;
    }
    load_finished_[migrate_meta.partition_id] = false;
  }

  LOG(INFO) << "[MigrateDone] " << migrate_data.DebugString();
  if (map_collection_id_ == update_collection_id_) {
    CHECK(map_versions_.find(migrate_meta.partition_id) == map_versions_.end());
    map_versions_[migrate_meta.partition_id] = migrate_data.map_version;
    num_local_map_part_ += 1;
    // TryUpdateMapVersion();
  } else {
    migrate_data.map_version = -1;
  }
  CHECK(update_versions_.find(migrate_meta.partition_id) == update_versions_.end());
  CHECK(pending_updates_.find(migrate_meta.partition_id) == pending_updates_.end());
  CHECK(waiting_updates_.find(migrate_meta.partition_id) == waiting_updates_.end());
  CHECK(update_tracker_.find(migrate_meta.partition_id) == update_tracker_.end());
  update_versions_[migrate_meta.partition_id] = migrate_data.update_version;
  pending_updates_[migrate_meta.partition_id] = std::move(migrate_data.pending_updates);
  waiting_updates_[migrate_meta.partition_id] = std::move(migrate_data.waiting_updates);
  update_tracker_[migrate_meta.partition_id] = std::move(migrate_data.update_tracker);
  num_local_update_part_ += 1;

  // handle buffered request
  LOG(INFO) << "[MigrateDone] part to migrate: " << migrate_meta.partition_id 
    <<", buffered_requests_ size: " << buffered_requests_[migrate_meta.partition_id].size();
  for (auto& request: buffered_requests_[migrate_meta.partition_id]) {
    CHECK_EQ(request.meta.part_id, migrate_meta.partition_id);
    if (map_collection_id_ == update_collection_id_
        && request.meta.version >= map_versions_[request.meta.part_id]) {
      pending_updates_[request.meta.part_id][request.meta.version].push_back(request);
    } else {
      waiting_updates_[request.meta.part_id].push_back(request);
    }
  }
  buffered_requests_[migrate_meta.partition_id].clear();

  auto& func = controller_->engine_elem_.function_store
      ->GetCreatePart(migrate_meta.collection_id);
  auto p = func();
  p->FromBin(bin2);  // now I serialize in the controller thread
  LOG(INFO) << "[MigrateDone] receive migrate partition in dest: " << migrate_meta.DebugString();
  CHECK(!controller_->engine_elem_.partition_manager->Has(migrate_meta.collection_id, migrate_meta.partition_id));
  controller_->engine_elem_.partition_manager->Insert(migrate_meta.collection_id, migrate_meta.partition_id, std::move(p));

  if (map_collection_id_ == update_collection_id_) {
    TryRunSomeMaps();
  }
  TryRunWaitingJoins(migrate_meta.partition_id);

  SArrayBinStream bin;
  ControllerMsg ctrl;
  ctrl.flag = ControllerMsg::Flag::kFinishMigrate;
  ctrl.version = -1;
  ctrl.node_id = -1;
  ctrl.plan_id = plan_id_;
  ctrl.part_id = migrate_meta.partition_id;
  bin << ctrl;
  controller_->SendMsgToScheduler(bin);
}

void PlanController::FinishLoadWith(SArrayBinStream bin) {
  std::tuple<int, int, int> submeta;
  bin >> submeta;
  {
    load_finished_[std::get<2>(submeta)] = true;
    LOG(INFO) << "[PlanController::FinishLoadWith] from_id: " << 
      std::get<0>(submeta) << " to_id: " << std::get<1>(submeta)
      << " part_id: " << std::get<2>(submeta);
  }
}

void PlanController::MigratePartitionStartMigrateMapOnly(MigrateMeta migrate_meta) {
  LOG(INFO) << "[Migrate] migrating start: " << migrate_meta.DebugString();
  // std::thread th([this, migrate_meta]() mutable {
  CHECK_EQ(migrate_meta.collection_id, map_collection_id_) << "only consider map_collection first";
  CHECK_NE(migrate_meta.collection_id, update_collection_id_) << "only consider map_collection first";
  CHECK(map_versions_.find(migrate_meta.partition_id) != map_versions_.end());

  // stop the running map
  {
    std::lock_guard<std::mutex> lk(stop_updateing_partitions_mu_);
    CHECK(stop_updateing_partitions_.find(migrate_meta.partition_id) == stop_updateing_partitions_.end());
    // insert the migrate partition into stop_updateing_partitions_
    stop_updateing_partitions_[migrate_meta.partition_id] = true;
  }
  
  // version
  int map_version = map_versions_[migrate_meta.partition_id];
  SArrayBinStream bin1;
  bin1 << map_version;

  // partition
  CHECK(controller_->engine_elem_.partition_manager->Has(
    migrate_meta.collection_id, migrate_meta.partition_id)) << migrate_meta.collection_id << " " <<  migrate_meta.partition_id;
  auto part = controller_->engine_elem_.partition_manager->Get(
    migrate_meta.collection_id, migrate_meta.partition_id);
  SArrayBinStream bin2;
  part->ToBin(bin2);  // serialize

  // reset the flag
  migrate_meta.flag = MigrateMeta::MigrateFlag::kReceiveMapOnly;

  // send
  Message msg;
  msg.meta.sender = controller_->engine_elem_.node.id;
  msg.meta.recver = GetControllerActorQid(migrate_meta.to_id);
  msg.meta.flag = Flag::kOthers;
  SArrayBinStream ctrl_bin, plan_bin, ctrl2_bin;
  ctrl_bin << ControllerFlag::kMigratePartition;
  plan_bin << plan_id_;
  ctrl2_bin << migrate_meta;
  msg.AddData(ctrl_bin.ToSArray());
  msg.AddData(plan_bin.ToSArray());
  msg.AddData(ctrl2_bin.ToSArray());
  msg.AddData(bin1.ToSArray());
  msg.AddData(bin2.ToSArray());
  controller_->engine_elem_.sender->Send(std::move(msg));
  LOG(INFO) << "[Migrate] migrating: " << migrate_meta.DebugString();
  // });
  // th.detach();
}

void PlanController::ReassignMap(SArrayBinStream bin) {
  // to_id, part_id, version_id
  std::vector<std::tuple<int,int,int>> reassignments;
  bin >> reassignments;
  for (auto t : reassignments) {
    // LOG(INFO) << "A: " << std::get<0>(t) << ", " << std::get<1>(t)
    //   << ", " << std::get<2>(t);
    int to_id = std::get<0>(t);
    if (to_id == controller_->engine_elem_.node.id) {
      int part_id = std::get<1>(t);
      int version = std::get<2>(t);
      CHECK(controller_->engine_elem_.partition_manager->Has(map_collection_id_, part_id));
      CHECK(map_versions_.find(part_id) == map_versions_.end());
      map_versions_[part_id] = version;
      num_local_map_part_ += 1;
    }
  }
  CHECK_EQ(num_local_map_part_, controller_->engine_elem_.partition_manager->GetNumLocalParts(map_collection_id_));
  TryRunSomeMaps();
}

void PlanController::MigratePartitionReceiveMapOnly(Message msg) {
  LOG(INFO) << "MigratePartitionReceiveMapOnly start";
  CHECK_EQ(msg.data.size(), 5);
  SArrayBinStream ctrl2_bin, bin1, bin2;
  ctrl2_bin.FromSArray(msg.data[2]);
  bin1.FromSArray(msg.data[3]);
  bin2.FromSArray(msg.data[4]);
  MigrateMeta migrate_meta;
  ctrl2_bin >> migrate_meta;

  // version
  int map_version;
  bin1 >> map_version;

  auto& func = controller_->engine_elem_.function_store
      ->GetCreatePart(migrate_meta.collection_id);
  auto p = func();
  p->id = migrate_meta.partition_id;
  p->FromBin(bin2);  // now I serialize in the controller thread

  LOG(INFO) << "MigratePartitionReceiveMapOnly";
  RunMap(migrate_meta.partition_id, map_version, p);
}

void PlanController::DisplayTime() {
  std::lock_guard<std::mutex> lk(time_mu_);
  double avg_map_time = 0;
  double avg_map_stime = 0;
  double avg_update_time = 0;
  std::chrono::duration<double> duration;
  for (auto& x : map_time_) {
    duration = std::get<1>(x.second) - std::get<0>(x.second);
    avg_map_time += duration.count();
    duration = std::get<2>(x.second) - std::get<1>(x.second);
    avg_map_stime += duration.count();
  }
  for (auto& x : update_time_) {
    for (auto& y : x.second) {
      duration = y.second.second - y.second.first;
      avg_update_time += duration.count();
    }
  }
  avg_map_time /= num_local_map_part_;
  avg_map_stime /= num_local_map_part_;
  avg_update_time = avg_update_time / num_local_update_part_;// num_upstream_part_;
  // nan if num is zero
  LOG(INFO) << "avg map time: " << avg_map_time << " ,avg map serialization time: " << avg_map_stime << " ,avg update time: " << avg_update_time;
} 

void PlanController::CalcJoinTrackerSize() {
  int size = 0;
  for (const auto& part: update_tracker_) {
    for (const auto& v: part.second) {
      size += v.second.size();
    }
  }
  update_tracker_size_.push_back(size);
}

void PlanController::ShowJoinTrackerSize() {
  std::stringstream ss;
  int mini = INT_MAX;
  int maxi = INT_MIN;
  int sum = 0;
  for (auto s : update_tracker_size_) {
    ss << s << " ";
    if (s > maxi) maxi = s;
    if (s < mini) mini = s;
    sum += s;
  }
  float avg = update_tracker_size_.empty() ? -1:sum*1./update_tracker_size_.size();
  LOG(INFO) << "trackersize: min: " << mini << " max: " << maxi << " avg: " << avg << " all: " << ss.str();
}

}  // namespace

