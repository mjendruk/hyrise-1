#pragma once

#include <cxxopts.hpp>
#include <json.hpp>

#include <chrono>
#include <iostream>
#include <unordered_map>

#include "storage/chunk.hpp"
#include "storage/chunk_encoder.hpp"
#include "storage/encoding_type.hpp"

namespace opossum {

/**
 * IndividualQueries runs each query a number of times and then the next one
 * PermutedQuerySets runs the queries as sets permuting their order after each run (this exercises caches)
 */
enum class BenchmarkMode { IndividualQueries, PermutedQuerySets };

using Duration = std::chrono::high_resolution_clock::duration;
using TimePoint = std::chrono::high_resolution_clock::time_point;

using NamedQuery = std::pair<std::string, std::string>;
using NamedQueries = std::vector<NamedQuery>;

using EncodingMapping = std::unordered_map<std::string, std::map<std::string, ColumnEncodingSpec>>;

/**
 * @return std::cout if `verbose` is true, otherwise returns a discarding stream
 */
std::ostream& get_out_stream(const bool verbose);

struct QueryBenchmarkResult {
  size_t num_iterations = 0;
  Duration duration = Duration{};
};

using QueryID = size_t;
using BenchmarkResults = std::unordered_map<std::string, QueryBenchmarkResult>;

/**
 * Loosely copying the functionality of benchmark::State
 * keep_running() returns false once enough iterations or time has passed.
 */
struct BenchmarkState {
  enum class State { NotStarted, Running, Over };

  BenchmarkState(const size_t max_num_iterations, const Duration max_duration);

  bool keep_running();

  State state{State::NotStarted};
  TimePoint begin = TimePoint{};
  TimePoint end = TimePoint{};

  size_t num_iterations = 0;
  size_t max_num_iterations;
  Duration max_duration;
};

/**
 * Represents the column encodings specified for a benchmark. If encoding (and vector compression) were specified via
 * command line args, this will contain no custom encoding mapping but only the column default. This will lead to each
 * column in each chunk to be encoded/compressed by this default. If a JSON config was provided, a column specific
 * encoding/compression can be chosen (same in each chunk). The JSON config must look like this:
 *
 * All encoding/compression types can be viewed with the `help` command or seen in constant_mappings.cpp.
 * The encoding is always required, the compression is optional.
 *
 * {
 *   "default": {
 *     "encoding": <ENCODING_TYPE_STRING>,               // required
 *     "compression": <VECTOR_COMPRESSION_TYPE_STRING>,  // optional
 *   },
 *
 *   "custom": {
 *     <TABLE_NAME>: {
 *       <COLUMN_NAME>: {
 *         "encoding": <ENCODING_TYPE_STRING>,
 *         "compression": <VECTOR_COMPRESSION_TYPE_STRING>
 *       },
 *       <COLUMN_NAME>: {
 *         "encoding": <ENCODING_TYPE_STRING>
 *       }
 *     },
 *     <TABLE_NAME>: {
 *       <COLUMN_NAME>: {
 *         "encoding": <ENCODING_TYPE_STRING>,
 *         "compression": <VECTOR_COMPRESSION_TYPE_STRING>
 *       }
 *     }
 *   }
 * }
 */
struct EncodingConfig {
  EncodingConfig();
  EncodingConfig(ColumnEncodingSpec default_encoding_spec, EncodingMapping encoding_mapping);
  explicit EncodingConfig(ColumnEncodingSpec default_encoding_spec);

  const ColumnEncodingSpec default_encoding_spec;
  const EncodingMapping encoding_mapping;

  static ColumnEncodingSpec encoding_spec_from_strings(const std::string& encoding_str,
                                                       const std::string& compression_str);
  static EncodingType encoding_string_to_type(const std::string& encoding_str);
  static std::optional<VectorCompressionType> compression_string_to_type(const std::string& compression_str);

  nlohmann::json to_json() const;
};

/**
 * Represents the configurable options for a benchmark run. This can be provided via command line args (see `help`
 * command or look at the basic_cli_options in benchmark_runner.cpp). This can also be provided as a JSON config file.
 * The options are indentical to and behave like the CLI options. Example:
 * {
 *   "verbose": true,
 *   "scheduler": true,
 *   "chunk_size": 10000,
 *   "time": 5
 * }
 *
 * The JSON config can also include benchmark-specific options (e.g. TPCH's scale option). They will be parsed like the
 * CLI options.
 * {
 *   "verbose": true,
 *   "scale": 0.01
 * }
 */
struct BenchmarkConfig {
  BenchmarkConfig(const BenchmarkMode benchmark_mode, const bool verbose, const ChunkOffset chunk_size,
                  const EncodingConfig encoding_config, const size_t max_num_query_runs, const Duration& max_duration,
                  const UseMvcc use_mvcc, const std::optional<std::string>& output_file_path,
                  const bool enable_scheduler, const bool enable_visualization, std::ostream& out);

  static BenchmarkConfig get_default_config();

  const BenchmarkMode benchmark_mode = BenchmarkMode::IndividualQueries;
  const bool verbose = false;
  const ChunkOffset chunk_size = Chunk::MAX_SIZE;
  const EncodingConfig encoding_config = EncodingConfig{};
  const size_t max_num_query_runs = 1000;
  const Duration max_duration = std::chrono::seconds(5);
  const UseMvcc use_mvcc = UseMvcc::No;
  const std::optional<std::string> output_file_path = std::nullopt;
  const bool enable_scheduler = false;
  const bool enable_visualization = false;
  std::ostream& out;

 private:
  BenchmarkConfig() : out(std::cout) {}
};

class CLIConfigParser {
 public:
  static bool cli_has_json_config(const int argc, char** argv);

  static nlohmann::json parse_json_config_file(const std::string& json_file_str);

  static nlohmann::json basic_cli_options_to_json(const cxxopts::ParseResult& parse_result);

  static BenchmarkConfig parse_basic_options_json_config(const nlohmann::json& json_config);

  static BenchmarkConfig parse_basic_cli_options(const cxxopts::ParseResult& parse_result);

  static EncodingConfig parse_encoding_config(const std::string& encoding_file_str);
};

}  // namespace opossum