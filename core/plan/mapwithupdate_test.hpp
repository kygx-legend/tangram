#include "gtest/gtest.h"
#include "glog/logging.h"

#include "core/partition/seq_partition.hpp"
#include "core/plan/mapwithupdate.hpp"

namespace xyz {
namespace {

struct ObjT {
  using KeyT = int;
  using ValT = int;
  ObjT() = default;
  ObjT(KeyT _a):a(_a) {}
  KeyT Key() const { return a; }
  int a;
};

class TestMapWithJoin: public testing::Test {};

TEST_F(TestMapWithJoin, Create) {
  int plan_id = 0;
  Collection<ObjT> c1{1};
  Collection<ObjT> c2{2};
  Collection<ObjT> c3{4};
  auto plan = GetMapWithJoin<int>(plan_id, &c1, &c2, &c3);
  plan.mapwith = [](const ObjT& obj, TypedCache<ObjT>* cache, Output<typename ObjT::KeyT, int>* o) {
    ObjT cache_obj = cache->Get(2);
    int ret = obj.Key() + cache_obj.a;
    o->Add(ret, 1);
  };
}

}  // namespace
}  // namespace xyz

