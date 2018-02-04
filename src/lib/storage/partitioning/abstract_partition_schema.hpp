#pragma once

#include "all_type_variant.hpp"
#include "storage/chunk.hpp"
#include "storage/partitioning/partition.hpp"
#include "storage/proxy_chunk.hpp"
#include "types.hpp"

namespace opossum {

/*
 * PartitionSchema determine how to partition a table logically.
 * This is a strategy pattern with AbstractPartitionSchema defining the interface.
 * A PartitionSchema has a number of partitions referencing Chunks of the associated Table.
 * Chunks have to be created in the Table to be then passed by reference (i.e. shared_ptr)
 * to the PartitionSchema.
 */

class AbstractPartitionSchema {
 public:
  AbstractPartitionSchema() = default;
  virtual ~AbstractPartitionSchema() = default;

  AbstractPartitionSchema(AbstractPartitionSchema&&) = default;
  AbstractPartitionSchema& operator=(AbstractPartitionSchema&&) = default;

  virtual std::string name() const = 0;
  virtual PartitionSchemaType get_type() const = 0;
  uint16_t partition_count() const;

  void clear();
  virtual void append(std::vector<AllTypeVariant> values) = 0;
  void append(std::vector<AllTypeVariant> values, PartitionID partition_id);

  virtual PartitionID get_matching_partition_for(std::vector<AllTypeVariant> values) = 0;

  virtual void add_new_chunk(std::shared_ptr<Chunk> chunk, PartitionID partition_id);
  virtual bool is_partitioned() const { return true; }

  std::shared_ptr<Partition> get_partition(PartitionID partition_id);
  std::shared_ptr<Chunk> last_chunk(PartitionID partition_id);

 protected:
  std::vector<std::shared_ptr<Partition>> _partitions;
};

}  // namespace opossum