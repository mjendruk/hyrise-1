#pragma once

#include "abstract_expression.hpp"

namespace opossum {

class SelectExpression;

class ExistsExpression : public AbstractExpression {
  explicit ExistsExpression(const std::shared_ptr<SelectExpression>& select);

  /**
   * @defgroup Overrides for AbstractExpression
   * @{
   */
  std::shared_ptr<AbstractExpression> deep_copy() const override;
  std::shared_ptr<AbstractExpression> resolve_expression_columns() const override;
  /**@}*/

  std::shared_ptr<SelectExpression> select;
};

}  // namespace opossum
