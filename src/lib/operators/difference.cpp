#include "difference.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "storage/reference_column.hpp"
#include "type_cast.hpp"
#include "utils/assert.hpp"

namespace opossum {
Difference::Difference(const std::shared_ptr<const AbstractOperator> left_in,
                       const std::shared_ptr<const AbstractOperator> right_in)
    : AbstractReadOnlyOperator(OperatorType::Difference, left_in, right_in) {}

const std::string Difference::name() const { return "Difference"; }

std::shared_ptr<AbstractOperator> Difference::_on_recreate(
    const std::vector<AllParameterVariant>& args, const std::shared_ptr<AbstractOperator>& recreated_input_left,
    const std::shared_ptr<AbstractOperator>& recreated_input_right) const {
  return std::make_shared<Difference>(recreated_input_left, recreated_input_right);
}

std::shared_ptr<const Table> Difference::_on_execute() {
  DebugAssert(input_table_left()->column_definitions() == input_table_right()->column_definitions(),
              "Input tables must have same number of columns");

  auto output = std::make_shared<Table>(input_table_left()->column_definitions(), TableType::References);

  // 1. We create a set of all right input rows as concatenated strings.

  auto right_input_row_set = std::unordered_set<std::string>(input_table_right()->row_count());

  // Iterating over all chunks and for each chunk over all columns
  for (ChunkID chunk_id{0}; chunk_id < input_table_right()->chunk_count(); chunk_id++) {
    auto chunk = input_table_right()->get_chunk(chunk_id);
    // creating a temporary row representation with strings to be filled column wise
    auto string_row_vector = std::vector<std::stringstream>(chunk->size());
    for (ColumnID column_id{0}; column_id < input_table_right()->column_count(); column_id++) {
      const auto base_column = chunk->get_column(column_id);

      // filling the row vector with all values from this column
      auto row_string_buffer = std::stringstream{};
      for (ChunkOffset chunk_offset = 0; chunk_offset < base_column->size(); chunk_offset++) {
        // Previously we called a virtual method of the BaseColumn interface here.
        // It was replaced with a call to the subscript operator as that is equally slow.
        const auto value = (*base_column)[chunk_offset];
        append_string_representation(string_row_vector[chunk_offset], value);
      }
    }

    // Remove duplicate rows by adding all rows to a unordered set
    std::transform(string_row_vector.cbegin(), string_row_vector.cend(),
                   std::inserter(right_input_row_set, right_input_row_set.end()), [](auto& x) { return x.str(); });
  }

  // 2. Now we check for each chunk of the left input which rows can be added to the output

  // Iterating over all chunks and for each chunk over all columns
  for (ChunkID chunk_id{0}; chunk_id < input_table_left()->chunk_count(); chunk_id++) {
    const auto in_chunk = input_table_left()->get_chunk(chunk_id);

    ChunkColumns output_columns;

    // creating a map to share pos_lists (see table_scan.hpp)
    std::unordered_map<std::shared_ptr<const PosList>, std::shared_ptr<PosList>> out_pos_list_map;

    for (ColumnID column_id{0}; column_id < input_table_left()->column_count(); column_id++) {
      const auto base_column = in_chunk->get_column(column_id);
      // temporary variables needed to create the reference column
      const auto referenced_column = std::dynamic_pointer_cast<const ReferenceColumn>(
          input_table_left()->get_chunk(chunk_id)->get_column(column_id));
      auto out_column_id = column_id;
      auto out_referenced_table = input_table_left();
      std::shared_ptr<const PosList> in_pos_list;

      if (referenced_column) {
        // if the input column was a reference column then the output column must reference the same values/objects
        out_column_id = referenced_column->referenced_column_id();
        out_referenced_table = referenced_column->referenced_table();
        in_pos_list = referenced_column->pos_list();
      }

      // automatically creates the entry if it does not exist
      std::shared_ptr<PosList>& pos_list_out = out_pos_list_map[in_pos_list];

      if (!pos_list_out) {
        pos_list_out = std::make_shared<PosList>();
      }

      // creating a ReferenceColumn for the output
      auto out_reference_column = std::make_shared<ReferenceColumn>(out_referenced_table, out_column_id, pos_list_out);
      output_columns.push_back(out_reference_column);
    }

    // for all offsets check if the row can be added to the output
    for (ChunkOffset chunk_offset = 0; chunk_offset < in_chunk->size(); chunk_offset++) {
      // creating string representation off the row at chunk_offset
      auto row_string_buffer = std::stringstream{};
      for (ColumnID column_id{0}; column_id < input_table_left()->column_count(); column_id++) {
        const auto base_column = in_chunk->get_column(column_id);

        // Previously a virtual method of the BaseColumn interface was called here.
        // It was replaced with a call to the subscript operator as that is equally slow.
        const auto value = (*base_column)[chunk_offset];
        append_string_representation(row_string_buffer, value);
      }
      const auto row_string = row_string_buffer.str();

      // we check if the recently created row_string is contained in the left_input_row_set
      auto search = right_input_row_set.find(row_string);
      if (search == right_input_row_set.end()) {
        for (auto pos_list_pair : out_pos_list_map) {
          if (pos_list_pair.first) {
            pos_list_pair.second->emplace_back(pos_list_pair.first->at(chunk_offset));
          } else {
            pos_list_pair.second->emplace_back(RowID{chunk_id, chunk_offset});
          }
        }
      }
    }

    // Only add chunk if it would contain any tuples
    if (!output_columns.empty() && output_columns[0]->size() > 0) {
      output->append_chunk(output_columns);
    }
  }

  return output;
}

void Difference::append_string_representation(std::ostream& row_string_buffer, const AllTypeVariant value) {
  const auto string_value = type_cast<std::string>(value);
  const auto length = static_cast<uint32_t>(string_value.length());

  // write value as string
  row_string_buffer << string_value;

  // write byte representation of length
  row_string_buffer.write(reinterpret_cast<const char*>(&length), sizeof(length));
}

}  // namespace opossum
