#pragma once

#include "abstract_expression.hpp"

namespace opossum {

enum class FunctionType {
  Substring
};

class FunctionExpression : public AbstractExpression {
 public:
  FunctionExpression(const FunctionType function_type,
                      const std::vector<std::shared_ptr<AbstractExpression>>& arguments);

  std::shared_ptr<AbstractExpression> deep_copy() const override;
  std::string as_column_name() const override;

  FunctionType function_type;

 protected:
  bool _shallow_equals(const AbstractExpression& expression) const override;
  size_t _on_hash() const override;
};

} // namespace opossum
