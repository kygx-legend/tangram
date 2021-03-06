#include "base/color.hpp"
#include "boost/tokenizer.hpp"
#include "core/plan/runner.hpp"

#include "core/partition/file_partition.hpp"

#include <cmath>
#include <regex>
#include <string>

DEFINE_int32(num_of_docs, 1, "# number of docs");
DEFINE_int32(num_doc_partition, 10, "");
DEFINE_int32(num_term_partition, 10, "");

DEFINE_string(url, "", "The url for hdfs file");

using namespace xyz;

class Document {
public:
  using KeyT = std::string;
  Document() = default;
  explicit Document(const KeyT &t) : title(t) {}
  KeyT title;
  std::vector<float> tf;
  std::vector<float> tf_idf;
  std::vector<std::string> words;
  int total_words = 0;
  const KeyT &id() const { return title; }
  KeyT Key() const { return title; }
  friend SArrayBinStream &operator<<(SArrayBinStream &stream,
                                     const Document &doc) {
    stream << doc.title << doc.tf << doc.tf_idf << doc.words << doc.total_words;
    return stream;
  }
  friend SArrayBinStream &operator>>(SArrayBinStream &stream, Document &doc) {
    stream >> doc.title >> doc.tf >> doc.tf_idf >> doc.words >> doc.total_words;
    return stream;
  }
};

class Term {
public:
  using KeyT = std::string;
  Term() = default;
  explicit Term(const KeyT &term) : termid(term) {}
  KeyT termid;
  int idf;
  std::vector<std::string> titles;
  std::vector<int> indexs;
  const KeyT &id() const { return termid; }
  KeyT Key() const { return termid; }
  friend SArrayBinStream &operator<<(SArrayBinStream &stream, const Term &t) {
    stream << t.termid << t.idf << t.titles << t.indexs;
    return stream;
  }
  friend SArrayBinStream &operator>>(SArrayBinStream &stream, Term &t) {
    stream >> t.termid >> t.idf >> t.titles >> t.indexs;
    return stream;
  }
};

struct Msg {
  int index;
  std::string title;

  friend SArrayBinStream &operator<<(xyz::SArrayBinStream &stream,
                                     const Msg &m) {
    stream << m.index << m.title;
    return stream;
  }
  friend SArrayBinStream &operator>>(xyz::SArrayBinStream &stream, Msg &m) {
    stream >> m.index >> m.title;
    return stream;
  }
};

int main(int argc, char **argv) {
  Runner::Init(argc, argv);

  // load and generate two collections
  auto parse_doc = [](std::string content) {
    // parse extract title
    std::regex rgx(".*?title=\"(.*?)\".*?");
    std::smatch match;
    std::string title;
    if (std::regex_search(content, match, rgx)) {
      // LOG(INFO) << match[1];
      title = match[1];
    } else {
      CHECK(false) << "cannot match title";
    }

    Document doc(title);
    std::vector<int> count;
    if (content.size() > 0) {
      boost::char_separator<char> sep(" \t\n.,()\'\":;!?<>");
      boost::tokenizer<boost::char_separator<char>> tok(content, sep);
      for (auto &w : tok) {
        doc.words.push_back(w);
        std::transform(doc.words.back().begin(), doc.words.back().end(),
                       doc.words.back().begin(), ::tolower);
      }
      doc.total_words = doc.words.size();
      std::sort(doc.words.begin(), doc.words.end());
      int n = 0;
      for (int i = 0, j = 0; i < doc.words.size(); i = j) {
        for (j = i + 1; j < doc.words.size(); j++) {
          if (doc.words.at(i).compare(doc.words.at(j)) != 0)
            break;
        }
        count.push_back(j - i);
        doc.words[n++] = doc.words[i];
      }
      doc.words.resize(n);
      doc.words.shrink_to_fit();
      doc.tf.resize(doc.words.size());
      doc.tf_idf.resize(doc.words.size());
      for (int i = 0; i < doc.words.size(); i++) {
        doc.tf.at(i) = static_cast<float>(count.at(i)) / doc.total_words;
      }
    }

    return doc;
  };

  auto file_meta = Context::load_wholefiles_meta(FLAGS_url);
  auto indexed_docs = Context::placeholder<Document>(FLAGS_num_doc_partition);

  Context::mappartupdate(
      file_meta, indexed_docs,
      [parse_doc](TypedPartition<std::string> *p,
                  Output<std::string, Document> *o) {
        auto *bp = dynamic_cast<FilePartition *>(p);
        CHECK_NOTNULL(bp);
        std::string file_str = bp->GetFileString();

        // find each doc
        std::string::size_type pos = 0;
        std::string::size_type prev = 0;
        std::string doc_str;
        while ((pos = file_str.find('\n', prev)) != std::string::npos) {
          std::string line = file_str.substr(prev, pos - prev);
          if (line.compare("</doc>") == 0) {
            Document doc = parse_doc(std::move(doc_str));
            o->Add(doc.id(), std::move(doc));
            doc_str.clear();
          } else {
            doc_str += " " + line;
          }
          prev = pos + 1;
        }
      },
      [](Document *p_doc, Document doc) { *p_doc = std::move(doc); })
      ->SetName("build indexed_docs from loaded_docs");

  auto terms = Context::placeholder<Term>(FLAGS_num_term_partition);
  Context::sort_each_partition(indexed_docs);

  Context::mappartupdate(
      indexed_docs, terms,
      [](TypedPartition<Document> *p, Output<std::string, Msg> *o) {
        for (auto &doc : *p) {
          for (int i = 0; i < doc.words.size(); i++) {
            Msg msg;
            msg.index = i;
            msg.title = doc.id();
            o->Add(doc.words[i], msg);
          }
        }
      },
      [](Term *term, Msg msg) {
        term->titles.push_back(msg.title);
        term->indexs.push_back(msg.index);
        term->idf += 1;

      })
      ->SetName("Out all the doc");

  // Context::count(terms);

  Context::mappartupdate(
      terms, indexed_docs,
      [](TypedPartition<Term> *p, Output<std::string, std::pair<int, int>> *o) {
        for (auto &term : *p) {
          for (int i = 0; i < term.indexs.size(); i++) {
            o->Add(term.titles[i], std::make_pair(term.indexs[i], term.idf));
          }
        }
      },
      [](Document *doc, std::pair<int, int> m) {
        doc->tf_idf[m.first] = std::log(FLAGS_num_of_docs / float(m.second));
      })
      ->SetName("Send idf back to doc");
  Runner::Run();
}
