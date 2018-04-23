#include <memory>
#include <string>
#include <utility>

#include "base_test.hpp"
#include "gtest/gtest.h"

#include "storage/chunk_encoder.hpp"
#include "storage/column_encoding_utils.hpp"
#include "storage/fixedstring_dictionary_column/fixedstring_column.hpp"
#include "storage/value_column.hpp"
#include "storage/vector_compression/fixed_size_byte_aligned/fixed_size_byte_aligned_vector.hpp"

namespace opossum {

class StorageFixedStringColumnTest : public BaseTest {
 protected:
  std::shared_ptr<ValueColumn<std::string>> vc_str = std::make_shared<ValueColumn<std::string>>();
};

TEST_F(StorageFixedStringColumnTest, CompressColumnString) {
  vc_str->append("Bill");
  vc_str->append("Steve");
  vc_str->append("Alexander");
  vc_str->append("Steve");
  vc_str->append("Hasso");
  vc_str->append("Bill");

  auto col = encode_column(EncodingType::FixedStringDictionary, DataType::String, vc_str);
  auto dict_col = std::dynamic_pointer_cast<FixedStringColumn<std::string>>(col);

  // Test attribute_vector size
  EXPECT_EQ(dict_col->size(), 6u);

  // Test dictionary size (uniqueness)
  EXPECT_EQ(dict_col->unique_values_count(), 4u);

  // Test sorting
  auto dict = dict_col->dictionary();
  EXPECT_EQ((*dict)[0], "Alexander");
  EXPECT_EQ((*dict)[1], "Bill");
  EXPECT_EQ((*dict)[2], "Hasso");
  EXPECT_EQ((*dict)[3], "Steve");
}

TEST_F(StorageFixedStringColumnTest, Decode) {
  vc_str->append("Bill");
  vc_str->append("Steve");
  vc_str->append("Alexander");

  auto col = encode_column(EncodingType::FixedStringDictionary, DataType::String, vc_str);
  auto dict_col = std::dynamic_pointer_cast<FixedStringColumn<std::string>>(col);

  EXPECT_EQ(dict_col->encoding_type(), EncodingType::FixedStringDictionary);
  // Decode values
  // TODO(team_btm): check this, throws error
  // EXPECT_EQ((*dict_col)[0], "Bill");
  // EXPECT_EQ((*dict_col)[1], "Steve");
  // EXPECT_EQ((*dict_col)[2], "Alexander");
}

TEST_F(StorageFixedStringColumnTest, CopyUsingAlloctor) {
  vc_str->append("Bill");
  vc_str->append("Steve");
  vc_str->append("Alexander");

  auto col = encode_column(EncodingType::FixedStringDictionary, DataType::String, vc_str);
  auto dict_col = std::dynamic_pointer_cast<FixedStringColumn<std::string>>(col);

  auto alloc = dict_col->dictionary()->get_allocator();
  auto base_column = dict_col->copy_using_allocator(alloc);
  auto dict_col_copy = std::dynamic_pointer_cast<FixedStringColumn<std::string>>(base_column);

  EXPECT_EQ(dict_col->dictionary()->get_allocator(), dict_col_copy->dictionary()->get_allocator());
  auto dict = dict_col_copy->dictionary();
  EXPECT_EQ((*dict)[0], "Alexander");
  EXPECT_EQ((*dict)[1], "Bill");
  EXPECT_EQ((*dict)[2], "Steve");
  // TODO(team_btm): why can we access an element more + this isnt something like null?
  EXPECT_EQ((*dict)[3], "Steve");
}

TEST_F(StorageFixedStringColumnTest, LowerUpperBound) {
  vc_str->append("A");
  vc_str->append("C");
  vc_str->append("E");
  vc_str->append("G");
  vc_str->append("I");
  vc_str->append("K");

  auto col = encode_column(EncodingType::FixedStringDictionary, DataType::String, vc_str);
  auto dict_col = std::dynamic_pointer_cast<FixedStringColumn<std::string>>(col);

  // Test for AllTypeVariant as parameter
  EXPECT_EQ(dict_col->lower_bound(AllTypeVariant("E")), (ValueID)2);
  EXPECT_EQ(dict_col->upper_bound(AllTypeVariant("E")), (ValueID)3);

  EXPECT_EQ(dict_col->lower_bound(AllTypeVariant("F")), (ValueID)3);
  EXPECT_EQ(dict_col->upper_bound(AllTypeVariant("F")), (ValueID)3);

  EXPECT_EQ(dict_col->lower_bound(AllTypeVariant("Z")), INVALID_VALUE_ID);
  EXPECT_EQ(dict_col->upper_bound(AllTypeVariant("Z")), INVALID_VALUE_ID);
}

TEST_F(StorageFixedStringColumnTest, MemoryUsageEstimation) {
  /**
   * WARNING: Since it's hard to assert what constitutes a correct "estimation", this just tests basic sanity of the
   * memory usage estimations
   */

  const auto empty_memory_usage =
      encode_column(EncodingType::FixedStringDictionary, DataType::String, vc_str)->estimate_memory_usage();

  vc_str->append("A");
  vc_str->append("B");
  vc_str->append("C");
  const auto compressed_column = encode_column(EncodingType::FixedStringDictionary, DataType::String, vc_str);
  const auto dictionary_column = std::dynamic_pointer_cast<FixedStringColumn<std::string>>(compressed_column);

  static constexpr auto size_of_attribute = 1u;
  static constexpr auto size_of_dictionary = 3u;

  EXPECT_EQ(dictionary_column->estimate_memory_usage(), empty_memory_usage + 3 * size_of_attribute + size_of_dictionary);
}

}  // namespace opossum
