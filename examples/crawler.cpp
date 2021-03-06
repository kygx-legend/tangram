#include "base/color.hpp"
#include "boost/tokenizer.hpp"
#include "core/plan/runner.hpp"

DEFINE_string(url, "", "The url to fetch");

DEFINE_string(python_script_path, "", "");

using namespace xyz;

struct UrlElem {
  using KeyT = std::string;
  UrlElem() = default;
  UrlElem(KeyT k) : url(k), status(Status::ToFetch) {}

  KeyT Key() const { return url; }
  enum class Status : char { ToFetch, Done };

  KeyT url;
  Status status;

  friend SArrayBinStream &operator<<(xyz::SArrayBinStream &stream,
                                     const UrlElem &url_elem) {
    stream << url_elem.url << url_elem.status;
    return stream;
  }
  friend SArrayBinStream &operator>>(xyz::SArrayBinStream &stream,
                                     UrlElem &url_elem) {
    stream >> url_elem.url >> url_elem.status;
    return stream;
  }
  std::string DebugString() const {
    std::stringstream ss;
    ss << "url: " << url;
    ss << ", status: " << (status == Status::ToFetch ? "ToFetch" : "Done");
    return ss.str();
  }
};

int main(int argc, char **argv) {
  Runner::Init(argc, argv);
  std::vector<UrlElem> seeds;
  boost::char_separator<char> sep(",");
  boost::tokenizer<boost::char_separator<char>> tok(FLAGS_url, sep);
  boost::tokenizer<boost::char_separator<char>>::iterator it = tok.begin();
  while (it != tok.end()) {
    seeds.push_back(UrlElem(*(it++)));
  }
  auto url_table =
      Context::distribute_by_key(seeds, 400, "distribute the seed");

  Context::mapupdate(
      url_table, url_table,
      [](const UrlElem &url_elem, Output<std::string, UrlElem::Status> *o) {
        if (url_elem.status == UrlElem::Status::ToFetch) {
          std::string cmd =
              "python " + FLAGS_python_script_path + " " + url_elem.url;
          std::array<char, 128> buffer;
          std::string result;
          LOG(INFO) << "downloading: " << url_elem.url;
          std::shared_ptr<FILE> pipe(popen(cmd.c_str(), "r"), pclose);
          LOG(INFO) << "downloaded: " << url_elem.url;
          if (!pipe)
            throw std::runtime_error("popen() failed!");
          while (!feof(pipe.get())) {
            if (fgets(buffer.data(), 128, pipe.get()) != nullptr)
              result += buffer.data();
          }
          std::stringstream ss(result);
          std::istream_iterator<std::string> begin(ss);
          std::istream_iterator<std::string> end;
          std::vector<std::string> urls(begin, end);
          // TODO: download the page and extract url
          o->Add(url_elem.url, UrlElem::Status::Done);
          LOG(INFO) << "new urls size: " << urls.size();
          for (auto fetched_url : urls) {
            // LOG(INFO) << RED(fetched_url);
            o->Add(fetched_url, UrlElem::Status::ToFetch);
          }
        }
      },
      [](UrlElem *url_elem, UrlElem::Status s) {
        if (s == UrlElem::Status::Done) {
          url_elem->status = UrlElem::Status::Done;
        }
      })
      ->SetIter(100)
      ->SetName("crawler main logic")
      ->SetStaleness(2);
  Context::foreach (
      url_table,
      [](const UrlElem &url_elem) { LOG(INFO) << RED(url_elem.DebugString()); },
      "print all status");
  Context::count(url_table);

  Runner::Run();
  // Runner::PrintDag();
}
