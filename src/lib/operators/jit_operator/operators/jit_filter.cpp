#include "jit_filter.hpp"

namespace opossum {

JitFilter::JitFilter(const JitTupleValue& condition) : _condition{condition} {
  DebugAssert(condition.data_type() == DataType::Bool, "Filter condition must be a boolean");
}

std::string JitFilter::description() const { return "[Filter] on x" + std::to_string(_condition.tuple_index()); }

void JitFilter::_consume(JitRuntimeContext& context) const {
//  const auto condition_value = _condition.materialize(context);
  if (context.tuple.get<bool>(_condition.tuple_index())) {
    _emit(context);
  }
}

}  // namespace opossum