#include "core/plan/runner.hpp"

using namespace xyz;

struct ObjT {
  using KeyT = std::string;
  using ValT = int;
  ObjT() = default;
  ObjT(KeyT key) : a(key), b(0) {}
  KeyT Key() const { return a; }
  KeyT a;
  int b;
  friend SArrayBinStream &operator<<(xyz::SArrayBinStream &stream,
                                     const ObjT &obj) {
    stream << obj.a << obj.b;
    return stream;
  }
  friend SArrayBinStream &operator>>(xyz::SArrayBinStream &stream, ObjT &obj) {
    stream >> obj.a >> obj.b;
    return stream;
  }
};

int main(int argc, char **argv) {
  Runner::Init(argc, argv);
  auto c1 = Context::distribute(
      std::vector<std::string>{"b", "a", "n", "a", "n", "a"}, 1);

  // std::string s;
  // auto c3 = Context::load(s, [](const std::string) { return ObjT(); });

  auto c2 = Context::placeholder<ObjT>(10);

  // mapupdate
  Context::mapupdate(c1, c2, [](std::string word,
                              Output<std::string, int> *o) { o->Add(word, 1); },
                   [](ObjT *obj, int m) {
                     obj->b += m;
                     LOG(INFO) << "update result: " << obj->a << " " << obj->b;
                   });

  // mappartupdate
  Context::mappartupdate(
      c1, c2,
      [](TypedPartition<std::string> *p, Output<std::string, int> *o) {
        // std::vector<std::pair<std::string, int>> kvs;
        for (auto &elem : *p) {
          o->Add(elem, 1);
        }
        // return kvs;
      },
      [](ObjT *obj, int m) {
        obj->b += m;
        LOG(INFO) << "update result: " << obj->a << " " << obj->b;
      });

  // mappartwithupdate
  // map 1, with 2, update 3
  Context::mappartwithupdate(
      c1, c2, c2,
      [](TypedPartition<std::string> *p, TypedCache<ObjT> *typed_cache,
         Output<std::string, int> *o) {

        // Get
        /*
        std::vector<std::string> keys{"a", "b", "c", "n"};
        auto ret = typed_cache->Get(keys);
        CHECK_EQ(ret.size(), keys.size());
        std::stringstream ss;
        for (int i = 0; i < keys.size(); ++ i) {
          ss << keys[i] << ":" << ret[i].b << ", ";
        }
        LOG(INFO) << ss.str();
        */

        // GetPartition
        for (int part_id = 0; part_id < 10; ++part_id) {
          auto part = typed_cache->GetPartition(part_id);
          auto *with_p = static_cast<TypedPartition<ObjT> *>(part.get());
          LOG(INFO) << "partition: " << part_id
                    << ", size: " << with_p->GetSize();
          for (auto &elem : *with_p) {
            LOG(INFO) << "elem: " << elem.a << " " << elem.b;
          }
          typed_cache->ReleasePart(part_id);
        }

        // std::vector<std::pair<std::string, int>> kvs;
        for (auto &elem : *p) {
          // kvs.push_back({elem, 1});
          o->Add(elem, 1);
        }
        // return kvs;
      },
      [](ObjT *obj, int m) {
        obj->b += m;
        LOG(INFO) << "update result: " << obj->a << " " << obj->b;
      })
      ->SetIter(10)
      ->SetStaleness(0)
      ->SetCheckpointInterval(5);

  Context::count(c1);

  // Context::write(c2, "", [](const ObjT& obj, std::stringstream& ss) {
  //   ss << obj.a << " " << obj.b << "\n";
  // });

  // Context::checkpoint(c1, "/tmp/tmp/checkpoint");
  // Context::loadcheckpoint(c1, "/tmp/tmp/checkpoint");

  Runner::Run();
  // Runner::PrintDag();
}
