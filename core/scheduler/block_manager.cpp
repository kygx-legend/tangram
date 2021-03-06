#include "core/scheduler/block_manager.hpp"

namespace xyz {

BlockManager::BlockManager(std::shared_ptr<SchedulerElem> elem,
        std::shared_ptr<CollectionManager> collection_manager,
        std::function<std::shared_ptr<Assigner>()> builder)
    : elem_(elem), collection_manager_(collection_manager), 
    builder_(builder) {}

void BlockManager::Load(SpecWrapper spec_wrapper) {
  CHECK(spec_wrapper.type == SpecWrapper::Type::kLoad);
  int plan_id = spec_wrapper.id;
  auto* spec = static_cast<LoadSpec*>(spec_wrapper.spec.get());
  std::vector<std::pair<std::string, int>> assigned_nodes;
  std::vector<int> num_local_threads;
  for (auto& kv: elem_->nodes) {
    assigned_nodes.push_back({kv.second.node.hostname, kv.second.node.id});
    num_local_threads.push_back(kv.second.num_local_threads);
  }
  CHECK(builder_);
  assigners_[spec->collection_id] = builder_();
  auto& assigner = assigners_[spec->collection_id];
  CHECK(assigner);
  int num_blocks =
      assigner->Load(spec->collection_id, spec->url, assigned_nodes, num_local_threads,
          spec->is_load_meta, spec->is_whole_file);
  cid_pid_[spec->collection_id] = plan_id;
}

void BlockManager::FinishBlock(SArrayBinStream bin) {
  FinishedBlock block;
  bin >> block;
  LOG(INFO) << "[Scheduler] FinishBlock: " << block.DebugString();
  auto& assigner = assigners_[block.collection_id];
  bool done = assigner->FinishBlock(block);
  if (done) {
    auto blocks = assigner->GetFinishedBlocks();
    stored_blocks_[block.collection_id] = blocks;
    // construct the collection view
    std::vector<int> part_to_node(blocks.size());
    for (int i = 0; i < part_to_node.size(); ++i) {
      CHECK(blocks.find(i) != blocks.end()) << "unknown block id " << i;
      part_to_node[i] = blocks[i].node_id;
    }
    CollectionView cv;
    cv.collection_id = block.collection_id;
    cv.mapper = SimplePartToNodeMapper(part_to_node);
    cv.num_partition = cv.mapper.GetNumParts();
    // LOG(INFO) << "!!!!\n" << cv.DebugString();
    elem_->collection_map->Insert(cv);

    // trigger update collection
    int collection_id = block.collection_id;
    int plan_id = cid_pid_[block.collection_id];
    collection_manager_->Update(collection_id, [this, plan_id]() {
      SArrayBinStream reply_bin;
      reply_bin << plan_id;
      ToScheduler(elem_, ScheduleFlag::kFinishPlan, reply_bin);
    });
  }
}


} // namespace xyz
