#pragma once

#include <cstdlib>
#include <vector>
#include <numeric>
#include <set>

#include "core/index/abstract_part_to_node_mapper.hpp"

namespace xyz {

class SimplePartToNodeMapper : public AbstractPartToNodeMapper {
 public:
  SimplePartToNodeMapper() = default;

  /*
   * Build map from consecutive nodes from 0 to num_nodes - 1.
   */
  void BuildRandomMap(int num_parts, int num_nodes) {
    std::vector<int> nodes;
    nodes.reserve(num_nodes);
    std::iota(nodes.begin(), nodes.end(), 0);
    BuildRandomMapFromNodeList(num_parts, nodes);
  }
  void BuildRandomMapFromNodeList(int num_parts, std::vector<int> nodes) {
    int num_nodes = nodes.size();
    v_.clear();
    v_.reserve(num_parts);
    for (int i = 0; i < num_parts; ++ i) {
      v_[i] = nodes[rand() % num_nodes];
    }
  }

  void UpdateMap(std::vector<int> nodes) {
    CHECK_GT(nodes.size(), 0);
    std::set<int> s(nodes.begin(), nodes.end());
    for (int i = 0; i < v_.size(); ++ i) {
      if (s.find(v_[i]) != s.end()) {
        v_[i] = nodes[rand() % nodes.size()];
      }
    }
  }

  virtual int Get(int part_id) {
    CHECK_LT(part_id, v_.size());
    return v_[part_id];
  }

  virtual void FromBin(SArrayBinStream& bin) override {
    bin >> v_;
  }
  virtual void ToBin(SArrayBinStream& bin) const override {
    bin << v_;
  }

  friend SArrayBinStream& operator<<(xyz::SArrayBinStream& stream, const SimplePartToNodeMapper& m) {
    m.ToBin(stream);
  	return stream;
  }
  friend SArrayBinStream& operator>>(xyz::SArrayBinStream& stream, SimplePartToNodeMapper& m) {
    m.FromBin(stream);
  	return stream;
  }
 private:
  std::vector<int> v_;
};

}  // namespace xyz
