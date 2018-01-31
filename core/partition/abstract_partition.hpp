#pragma once

#include <memory>

namespace xyz {

class AbstractPartition {
 public:
  virtual ~AbstractPartition() {}
};

template <typename ObjT>
class TypedPartition : public AbstractPartition {
 public:
  // Add obj into partition
  void Add(ObjT obj) {
    TypedAdd(std::move(obj));
  }
  virtual void TypedAdd(ObjT obj) = 0;

  virtual size_t GetSize() const = 0;

  virtual void Sort() = 0;
  /*
   * Subclasses need to implement Iterator and implement CreateIterator() function
   * to support range-based for loop.
   */
  struct Iterator {
    virtual ObjT& Deref() = 0; 
    virtual void SubAdvance() = 0;
    virtual bool SubUnequal(const std::unique_ptr<Iterator>& other) = 0;
  };
  struct IterWrapper {
    std::unique_ptr<Iterator> iter;
    ObjT& operator*() {
      return iter->Deref();
    }
    IterWrapper& operator++() {
      iter->SubAdvance();
      return *this;
    }
    bool operator!=(const IterWrapper& iw) const {
      return iter->SubUnequal(iw.iter);
    }
  };
  IterWrapper begin() {
    return CreateIterator(true);
  }
  IterWrapper end() {
    return CreateIterator(false);
  }

  virtual IterWrapper CreateIterator(bool) = 0;

};

}  // namespace