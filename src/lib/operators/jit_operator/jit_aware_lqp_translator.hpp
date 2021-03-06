#pragma once

#include "../jit_operator_wrapper.hpp"
#include "logical_query_plan/lqp_translator.hpp"
#include "operators/jit_expression.hpp"

namespace opossum {

/* This class can be used as a drop-in specialization for the LQPTranslator.
 * The JitAwareLQPTranslator will try to translate multiple AbstractLQPNodes into a single JitOperatorWrapper, whenever
 * that is possible and seems beneficial. Otherwise, it will fall back to the LQPTranslator.
 *
 * It works in two steps:
 * 1) Determine if we can/should add a JitOperatorWrapper node here and which nodes we can replace:
 *    Starting from the current node, we perform a breadth-first search through the query tree.
 *    For each node we will determine whether it is jittable (based on the node's type and parameters).
 *    We will follow each branch of the tree until we hit a non-jittable node. Since StoredTableNodes are not jittable,
 *    this is guaranteed to happen for all branches.
 *    All non-jittable nodes encountered this way are stored in a set.
 *    Once the BFS terminates, we only continue if the number of jittable nodes is greater than two and the
 *    set of non-jittable nodes we encountered only contains a single node. This is then used as the input
 *    node to the chain of jit operators.
 * 2) Once we know which nodes we want to jit, we can start building out JitOperatorWrapper:
 *    We start by adding a JitReadTuples node. This node is passed to all translation functions during the construction
 *    of further operators. If any jit operator depends on a column or literal value, this value is registered with the
 *    JitReadTuples operator. The operator returns a JitTupleValue that serves as a placeholder in the requesting
 *    operator. The JitReadTuples operator will make sure that the actual value is then accessible through the
 *    JitTupleValue at runtime.
 *    The output columns are determined by the top-most ProjectionNode. If there is no ProjectionNode, all columns from
 *    the input node are considered as outputs.
 *    In case we find any PredicateNode or UnionNode during our traversal, we need to create a JitFilter operator.
 *    Whenever a non-primitive value (such as a predicate conditions, LQPExpression of LQPColumnReferences - which
 *    can in turn reference a LQPExpression in a ProjectionNode) is encountered, it is converted to an JitExpression
 *    by a helper method first. We then add a JitCompute operator to our chain and use its result value instead of the
 *    original non-primitive value.
 */
class JitAwareLQPTranslator final : public LQPTranslator {
 public:
  JitAwareLQPTranslator();
  std::shared_ptr<AbstractOperator> translate_node(const std::shared_ptr<AbstractLQPNode>& node) const final;

 private:
  std::shared_ptr<JitOperatorWrapper> _try_translate_node_to_jit_operators(
      const std::shared_ptr<AbstractLQPNode>& node) const;

  std::shared_ptr<const JitExpression> _try_translate_node_to_jit_expression(
      const std::shared_ptr<AbstractLQPNode>& node, JitReadTuples& jit_source,
      const std::shared_ptr<AbstractLQPNode>& input_node) const;

  std::shared_ptr<const JitExpression> _try_translate_predicate_to_jit_expression(
      const std::shared_ptr<PredicateNode>& node, JitReadTuples& jit_source,
      const std::shared_ptr<AbstractLQPNode>& input_node) const;

  std::shared_ptr<const JitExpression> _try_translate_expression_to_jit_expression(
      const LQPExpression& lqp_expression, JitReadTuples& jit_source,
      const std::shared_ptr<AbstractLQPNode>& input_node) const;

  std::shared_ptr<const JitExpression> _try_translate_column_to_jit_expression(
      const LQPColumnReference& lqp_column_reference, JitReadTuples& jit_source,
      const std::shared_ptr<AbstractLQPNode>& input_node) const;

  std::shared_ptr<const JitExpression> _try_translate_variant_to_jit_expression(
      const AllParameterVariant& value, JitReadTuples& jit_source,
      const std::shared_ptr<AbstractLQPNode>& input_node) const;

  // Returns whether the part of the query plan represented by this LQP node filters tuples in some way.
  // This information is needed when converting a PredicateNode to a JitExpression to determine whether the
  // PredicateNode is part of a conjunction.
  // Example: SELECT ... WHERE A > 3 AND B < 4;
  // The LQP represents the WHERE clause as two consecutive PredicateNodes. When translating to a JitExpressions, the
  // first PredicateNode (A > 3) gets translated into a conjuction, with its condition being the
  // left-hand side: (A > 3) AND ...
  // The right-hand side of the conjunction is created by translating the second PredicateNode (B < 4) to JitExpression.
  // Since the second predicate has no further PredicateNodes following is, it can be translated into a simple
  // expression without the need to add an additional AND node.
  // This helper method distinguish these two cases for a given node.
  bool _input_is_filtered(const std::shared_ptr<AbstractLQPNode>& node) const;

  // Returns whether an LQP node with its current configuration can be part of an operator pipeline.
  bool _node_is_jittable(const std::shared_ptr<AbstractLQPNode>& node, const bool allow_aggregate_node) const;

  // Traverses the LQP in a breadth-first fashion and passes all visited nodes to a lambda. The boolean returned
  // from the lambda determines whether the current node should be explored further.
  void _visit(const std::shared_ptr<AbstractLQPNode>& node,
              std::function<bool(const std::shared_ptr<AbstractLQPNode>&)> func) const;
};

}  // namespace opossum
