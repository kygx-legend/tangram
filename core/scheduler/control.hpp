#pragma once

#include <sstream>
#include <string>

namespace xyz {

enum class ScheduleFlag : char {
  kRegisterProgram,
  kFinishBlock,
  kLoadBlock,
  kDummy,
  kExit,
  kDistribute,
  kFinishDistribute,
  kCheckpoint,
  kFinishCheckpoint,
  kLoadCheckpoint,
  kFinishLoadCheckpoint,
  kWritePartition,
  kFinishWritePartition,
  kControl,
  kFinishPlan,
  kUpdateCollection,
  kUpdateCollectionReply,
  kRecovery
};

static const char *ScheduleFlagName[] = {"kRegisterProgram",
                                         "kFinishBlock",
                                         "kLoadBlock",
                                         "kDummy",
                                         "kExit",
                                         "kDistribute",
                                         "kFinishDistribute",
                                         "kCheckpoint",
                                         "kFinishCheckpoint",
                                         "kLoadCheckpoint",
                                         "kFinishLoadCheckpoint",
                                         "kWritePartition",
                                         "kFinishWritePartition",
                                         "kControl",
                                         "kFinishPlan",
                                         "kUpdateCollection",
                                         "kUpdateCollectionReply",
                                         "kRecovery"
};

enum class FetcherFlag : char{
  kFetchObjsReply,
  kFetchPartRequest,
  kFetchPartReplyLocal,
  kFetchPartReplyRemote,
};

// currently workerinfo only has one field.
struct WorkerInfo {
  int num_local_threads;
};

/*
 * The message sent between the control_manager and the plan_controller.
 */
struct ControllerMsg {
  enum class Flag : char {
    kSetup, kMap, kJoin, kFinish
  };
  static constexpr const char* FlagName[] = {
    "kSetup", "kMap", "kJoin", "kFinish"
  };
  Flag flag;
  int version;
  int node_id;
  int plan_id;
  std::string DebugString() const {
    std::stringstream ss;
    ss << "flag: " << FlagName[static_cast<int>(flag)];
    ss << ", version: " << version;
    ss << ", node_id: " << node_id;
    ss << ", plan_id: " << plan_id;
    return ss.str();
  }
};

enum class ControllerFlag : char {
  kSetup,
  kStart,
  kFinishMap,
  kFinishJoin,
  kUpdateVersion,
  kReceiveJoin,
  kFetchRequest,
  kFinishFetch,
  kFinishCheckpoint,
  kRequestPartition,
  kReceivePartition,
  kMigratePartition,
};

struct FetchMeta {
  int plan_id; 
  int upstream_part_id;
  int collection_id;
  int partition_id; 
  int version;
  bool local_mode;
  std::string DebugString() const {
    std::stringstream ss;
    ss << "plan_id: " << plan_id;
    ss << ", upstream_part_id: " << upstream_part_id;
    ss << ", collection_id: " << collection_id;
    ss << ", partition_id: " << partition_id;
    ss << ", version: " << version;
    ss << ", local_mode: " << (local_mode ? "true":"false");
    return ss.str();
  }
};

struct MigrateMeta {
  int plan_id;
  int collection_id;
  int partition_id;
  int from_id;
  int to_id;  // the node id
  int current_map_version;
  std::string DebugString() const {
    std::stringstream ss;
    ss << "plan_id: " << plan_id;
    ss << ", collection_id: " << collection_id;
    ss << ", partition_id: " << partition_id;
    ss << ", from_id: " << from_id;
    ss << ", to_id: " << to_id;
    ss << ", current_map_version: " << current_map_version;
    return ss.str();
  }
};


struct MigrateMeta2 {
  enum class MigrateFlag {
    kStartMigrate,
    kFlushAll,
    kDest
  };
  static constexpr const char* FlagName[] = {
    "kStartMigrate", 
    "kFlushAll",
    "kDest"
  };
  MigrateFlag flag;
  int plan_id;
  int collection_id;
  int partition_id;
  int from_id;
  int to_id;  // the node id
  std::string DebugString() const {
    std::stringstream ss;
    ss << "flag: " << FlagName[static_cast<int>(flag)];
    ss << ", plan_id: " << plan_id;
    ss << ", collection_id: " << collection_id;
    ss << ", partition_id: " << partition_id;
    ss << ", from_id: " << from_id;
    ss << ", to_id: " << to_id;
    return ss.str();
  }
};

} // namespace xyz
