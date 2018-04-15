#include "base/color.hpp"
#include "core/plan/runner.hpp"
#include "boost/tokenizer.hpp"

#include <string>
#include <cmath>

DEFINE_string(url, "", "The url for hdfs file");
DEFINE_int32(num_data, -1, "The number of data in the dataset");
DEFINE_int32(num_params, -1, "The number of parameters in the dataset");
DEFINE_int32(num_data_parts, -1, "The number of partitions for dataset");
DEFINE_int32(num_param_per_part, -1, "The number of parameters per partition");
DEFINE_int32(batch_size, 1, "Batch size of SGD");
DEFINE_double(alpha, 0.1, "The learning rate of the model");
DEFINE_int32(num_iter, 1, "The number of iterations");
DEFINE_int32(staleness, 0, "Staleness for the SSP");
DEFINE_bool(is_sparse, false, "Is the dataset sparse or not");
DEFINE_bool(is_sgd, false, "Full gradient descent or mini-batch SGD");
DEFINE_string(combine_type, "kDirectCombine", "kShuffleCombine, kDirectCombine, kNoCombine, timeout");
DEFINE_int32(max_lines_per_part, -1, "max lines per part, for debug");

using namespace xyz;

struct Point {
  Point() = default;
  // <Fea, Val>
  std::vector<std::pair<int, float>> x;
  // Label
  int y;

  friend SArrayBinStream &operator<<(xyz::SArrayBinStream &stream,
                                     const Point &point) {
    stream << point.y << point.x;
    return stream;
  }
  friend SArrayBinStream &operator>>(xyz::SArrayBinStream &stream,
                                     Point &point) {
    stream >> point.y >> point.x;
    return stream;
  }
};

struct IndexedPoints {
  using KeyT = int;
  IndexedPoints() = default;
  IndexedPoints(KeyT _key) : key(_key) {}
  KeyT Key() const { return key; }
  KeyT key;
  std::vector<Point> points;

  friend SArrayBinStream &operator<<(xyz::SArrayBinStream &stream,
                                     const IndexedPoints &indexed_points) {
    stream << indexed_points.key << indexed_points.points;
    return stream;
  }
  friend SArrayBinStream &operator>>(xyz::SArrayBinStream &stream,
                                     IndexedPoints &indexed_points) {
    stream >> indexed_points.key >> indexed_points.points;
    return stream;
  }
};

struct Param {
  using KeyT = int;
  Param() = default;
  Param(KeyT _fea) : fea(_fea) {}
  KeyT Key() const { return fea; }
  KeyT fea;
  float val = 0;

  friend SArrayBinStream &operator<<(xyz::SArrayBinStream &stream,
                                     const Param &param) {
    stream << param.fea << param.val;
    return stream;
  }
  friend SArrayBinStream &operator>>(xyz::SArrayBinStream &stream,
                                     Param &param) {
    stream >> param.fea >> param.val;
    return stream;
  }
};

static auto* load_data() {
  return Context::load(FLAGS_url, [](std::string s) {
	  Point point;
    /*
    boost::char_separator<char> sep(" \t");
    boost::tokenizer<boost::char_separator<char>> tok(s, sep);
    boost::tokenizer<boost::char_separator<char>>::iterator it = tok.begin();

    point.y = std::stoi(*it);
    it ++;
    for (; it != tok.end(); ++it) {
      std::vector<std::string> fea_val;
      std::istringstream ss2(*it);
      std::string token;
      while (std::getline(ss2, token, ':')) {
        fea_val.push_back(token);
      }
      CHECK_EQ(fea_val.size(), 2);
      int fea = std::stoi(fea_val[0]);
      float val = std::stof(fea_val[1]);
      point.x.push_back(std::make_pair(fea, val));
    }
    */
	  char* pos;
	  char* tok = strtok_r(&s[0], " \t:", &pos);
	  int i = -1;
	  int idx;
	  float val;
	  while (tok != NULL) {
	    if (i == 0) {
	        idx = std::atoi(tok) - 1;
	        i = 1;
	    } else if (i == 1) {
	        val = std::atof(tok);
	        point.x.push_back(std::make_pair(idx, val));
	        i = 0;
	    } else {
	        point.y = std::atof(tok);
	        i = 0;
	    }
	    // Next key/value pair
	    tok = strtok_r(NULL, " \t:", &pos);
	  }

    return point;
  }, FLAGS_max_lines_per_part)->SetName("dataset");
}
