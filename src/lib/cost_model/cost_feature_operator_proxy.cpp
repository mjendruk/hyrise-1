#include "cost_feature_operator_proxy.hpp"

#include "operators/abstract_operator.hpp"
#include "operators/join_hash.hpp"
#include "operators/join_sort_merge.hpp"
#include "operators/join_nested_loop.hpp"
#include "operators/product.hpp"
#include "operators/table_scan.hpp"
#include "storage/table.hpp"
#include "resolve_type.hpp"

namespace opossum {

CostFeatureOperatorProxy::CostFeatureOperatorProxy(const std::shared_ptr<AbstractOperator>& op):
  _op(op) {

}

CostFeatureVariant CostFeatureOperatorProxy::_extract_feature_impl(const CostFeature cost_feature) const {
  switch (cost_feature) {
    case CostFeature::LeftInputRowCount:
      if (!_op->input_table_left()) return 0.0f;
      return static_cast<float>(_op->input_table_left()->row_count());
    case CostFeature::RightInputRowCount:
      if (!_op->input_table_right()) return 0.0f;
      return static_cast<float>(_op->input_table_right()->row_count());
    case CostFeature::LeftInputIsReferences:
      if (!_op->input_table_left()) return false;
      return _op->input_table_left()->type() == TableType::References;
    case CostFeature::RightInputIsReferences:
      if (!_op->input_table_right()) return false;
      return _op->input_table_right()->type() == TableType::References;
    case CostFeature::OutputRowCount:
      Assert(_op->get_output(), "Can't extract CostFeature since the output table is not available");
      return static_cast<float>(_op->get_output()->row_count());

    case CostFeature::LeftDataType:
    case CostFeature::RightDataType: {
      auto column_id = INVALID_COLUMN_ID;

      if (const auto join_op = std::dynamic_pointer_cast<AbstractJoinOperator>(_op); join_op) {
        if (cost_feature == CostFeature::LeftDataType) {
          Assert(_op->input_table_left(), "Input operator must be executed");
          return join_op->input_table_left()->column_data_type(join_op->column_ids().first);
        } else {
          Assert(_op->input_table_right(), "Input operator must be executed");
          return join_op->input_table_right()->column_data_type(join_op->column_ids().second);
        }
      } else if (_op->type() == OperatorType::TableScan) {
        Assert(_op->input_table_left(), "Input operator must be executed");

        const auto table_scan = std::dynamic_pointer_cast<TableScan>(_op);
        if (cost_feature == CostFeature::LeftDataType) {
          return table_scan->input_table_left()->column_data_type(table_scan->left_column_id());
        } else {
          if (table_scan->right_parameter().type() == typeid(AllTypeVariant)) {
            return data_type_from_all_type_variant(boost::get<AllTypeVariant>(table_scan->right_parameter()));
          } else {
            Assert(is_column_id(table_scan->right_parameter()), "Expected LQPColumnReference");
            const auto column_id = boost::get<ColumnID>(table_scan->right_parameter());
            return table_scan->input_table_left()->column_data_type(column_id);
          }
        }

      } else {
        return DataType::Null;
      }
    }

    case CostFeature::PredicateCondition:
      if (const auto join_op = std::dynamic_pointer_cast<AbstractJoinOperator>(_op); join_op) {
        return join_op->predicate_condition();
      } else if (_op->type() == OperatorType::TableScan) {
        return std::static_pointer_cast<TableScan>(_op)->predicate_condition();
      } else {
        return PredicateCondition::Equals;
      }

    case CostFeature::RightOperandIsColumn:
      if (_op->type() == OperatorType::TableScan) {
        return is_column_id(std::static_pointer_cast<TableScan>(_op)->right_parameter());
      } else {
        return false;
      }

    case CostFeature::OperatorType:
      return _op->type();

    default:
      Fail("Extraction of this feature is not implemented. Maybe it should be handled in AbstractCostFeatureProxy?");
  }
}

}  // namespace opossum