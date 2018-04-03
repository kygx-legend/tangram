#include "core/plan/runner.hpp"
#include "base/color.hpp"

using namespace xyz;

struct ObjT {
  using KeyT = int;
  using ValT = int;
  ObjT() = default;
  ObjT(KeyT key) : a(key), b(0) {}
  KeyT Key() const { return a; }
  KeyT a;
  int b;
  friend SArrayBinStream& operator<<(xyz::SArrayBinStream& stream, const ObjT& obj) {
    stream << obj.a << obj.b;
    return stream;
  }
  friend SArrayBinStream& operator>>(xyz::SArrayBinStream& stream, ObjT& obj) {
    stream >> obj.a >> obj.b;
    return stream;
  }
};


void mj() {
  std::vector<int> seed;
  const int num_map_part = 2;
  const int num_join_part = num_map_part;
  for (int i = 0; i < num_map_part; ++ i) {
    seed.push_back(i);
  }
  auto c1 = Context::distribute(seed, num_map_part);
  auto c2 = Context::placeholder<ObjT>(num_join_part);

  Context::checkpoint(c1, "/tmp/tmp/jasper/c0");
  Context::checkpoint(c2, "/tmp/tmp/jasper/c1");

  // mapjoin
  Context::mapjoin(c1, c2, 
    [](int id) {
      LOG(INFO) << GREEN("id: "+std::to_string(id)+", sleep for: " +std::to_string(50000) + " ms");
      std::this_thread::sleep_for(std::chrono::milliseconds(50000));
      return std::pair<int, int>(id, 1);
    },
    [](ObjT* obj, int m) {
      obj->b += m;
      LOG(INFO) << "join result: " << obj->a << " " << obj->b;
    });
}

void mpj() {
  std::vector<int> seed;
  const int num_map_part = 2;
  const int num_join_part = num_map_part;
  for (int i = 0; i < num_map_part; ++ i) {
    seed.push_back(i);
  }
  auto c0 = Context::distribute(seed, num_map_part);
  auto c1 = Context::placeholder<ObjT>(num_join_part);

  Context::checkpoint(c0, "/tmp/tmp/jasper/c0");
  Context::checkpoint(c1, "/tmp/tmp/jasper/c1");

   // mappartjoin
   Context::mappartjoin(c0, c1, 
    [](TypedPartition<int>* p, AbstractMapProgressTracker* t) {
      LOG(INFO) << GREEN("Sleep for: " +std::to_string(50000) + " ms");
      std::this_thread::sleep_for(std::chrono::milliseconds(50000));
      std::vector<std::pair<int, int>> kvs;
      for (auto& elem : *p) {
        kvs.push_back({elem, 1});
      }
      return kvs;
    },
    [](ObjT* obj, int m) {
      obj->b += m;
      LOG(INFO) << "join result: " << obj->a << " " << obj->b;
    });
} 
 

int main(int argc, char** argv) {
  Runner::Init(argc, argv);

  mj();
  // mpj();

  Runner::Run();
  // Runner::PrintDag();
}