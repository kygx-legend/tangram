#include "core/plan/context.hpp"

namespace xyz {

Store<CollectionBase> Context::collections_;
Store<PlanBase> Context::plans_;
Dag Context::dag_;

} // namespace xyz

