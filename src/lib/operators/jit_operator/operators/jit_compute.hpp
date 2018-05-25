#pragma once

#include "abstract_jittable.hpp"
#include "jit_expression.hpp"

namespace opossum {

/* The JitCompute operator computes a single expression on the current tuple.
 * Most of the heavy lifting is done by the JitExpression itself.
 */
class JitCompute : public AbstractJittable {
 public:
  explicit JitCompute(const std::shared_ptr<const JitExpression>& expression);

  std::string description() const final;

  std::shared_ptr<const JitExpression> expression();

 private:
  void _consume(JitRuntimeContext& context) const final;

  std::shared_ptr<const JitExpression> _expression;
};

}  // namespace opossum
