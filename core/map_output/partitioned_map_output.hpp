#pragma once

#include "core/map_output/abstract_map_output.hpp"
#include "core/index/abstract_key_to_part_mapper.hpp"

#include "core/map_output/map_output_stream.hpp"

#include <memory>
#include <vector>
#include <algorithm>

#include "glog/logging.h"

namespace xyz {

/*
 * Not thread-safe
 */
template<typename KeyT, typename MsgT>
class Output: public AbstractMapOutput {
 public:
  using OutputKeyT = KeyT;
  using OutputMsgT = MsgT;
  Output(std::shared_ptr<AbstractKeyToPartMapper> mapper)
      :key_to_part_mapper_(mapper), buffer_(mapper->GetNumPart()), 
       buffer_pointers_(mapper->GetNumPart()) {
    for (int i = 0; i < buffer_.size(); ++ i) {
      buffer_[i] = std::make_shared<MapOutputStream<KeyT, MsgT>>();
      buffer_pointers_[i] = static_cast<MapOutputStream<KeyT, MsgT>*>(buffer_[i].get());
    }
    typed_mapper_ = static_cast<TypedKeyToPartMapper<KeyT>*>(key_to_part_mapper_.get());
  }
  virtual ~Output() {}

  virtual int GetBufferSize() {
    return buffer_.size();
  }
  virtual std::shared_ptr<AbstractMapOutputStream> Get(int i) {
    CHECK_LT(i, buffer_.size());
    return buffer_[i];
  }

  using CombineFuncT = std::function<void(MsgT*, const MsgT&)>;

  void SetCombineFunc(CombineFuncT combine_func) {
    for (auto* buffer : buffer_pointers_) {
      buffer->SetCombineFunc(combine_func);
    }
  }

  void Add(KeyT key, MsgT msg) {
    DCHECK(typed_mapper_);
    auto part_id = typed_mapper_->Get(key);
    DCHECK_LT(part_id, key_to_part_mapper_->GetNumPart());
    buffer_pointers_[part_id]->Add(std::make_pair(std::move(key), std::move(msg)));
  }

  void Add(std::pair<KeyT, MsgT> msg) {
    DCHECK(typed_mapper_);
    auto part_id = typed_mapper_->Get(msg.first);
    DCHECK_LT(part_id, key_to_part_mapper_->GetNumPart());
    buffer_pointers_[part_id]->Add(std::move(msg));
  }

  void Add(std::vector<std::pair<KeyT, MsgT>> msgs) {
    for (auto& msg : msgs) {
      Add(std::move(msg));
    }
  }

  // TODO: can this be removed as we call combine for each mapoutputstream?
  virtual void Combine() override {
    for (auto* buffer : buffer_pointers_) {
      buffer->Combine();
    }
  }

  // Now we push into SArrayBinStream one by one.
  // May consider to use SArray as the underlying storage for SArrayMapOuput, so
  // that no need to serialize at all when the KeyT and MsgT are both trivially
  // copyable.
  virtual std::vector<SArrayBinStream> Serialize() override {
    std::vector<SArrayBinStream> rets;
    rets.reserve(buffer_.size());
    for (auto& buffer : buffer_) {
      rets.push_back(buffer->Serialize());
    }
    return rets;
  }

  const std::vector<std::pair<KeyT, MsgT>>& GetBuffer(int part_id) const {
    CHECK_LE(part_id, buffer_pointers_.size());
    return buffer_pointers_[part_id]->GetBuffer();
  }

  // For test use only.
  std::vector<std::vector<std::pair<KeyT, MsgT>>> GetBuffer() { 
    std::vector<std::vector<std::pair<KeyT, MsgT>>> ret;
    for (auto* buffer: buffer_pointers_) {
      ret.push_back(buffer->GetBuffer());
    }
    return ret; 
  }
 private:
  // std::vector<std::vector<std::pair<KeyT, MsgT>>> buffer_;
  std::vector<std::shared_ptr<AbstractMapOutputStream>> buffer_;
  // use buffer_pointers_ with cautions
  std::vector<MapOutputStream<KeyT, MsgT>*> buffer_pointers_;  

  std::shared_ptr<AbstractKeyToPartMapper> key_to_part_mapper_;
  TypedKeyToPartMapper<KeyT>* typed_mapper_ = nullptr;
};

}  // namespace

