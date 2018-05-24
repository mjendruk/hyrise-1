#include <boost/algorithm/string.hpp>

#include <fstream>

#include "benchmark_utils.hpp"
#include "scheduler/current_scheduler.hpp"
#include "scheduler/node_queue_scheduler.hpp"
#include "scheduler/topology.hpp"
#include "utils/filesystem.hpp"
#include "utils/performance_warning.hpp"

namespace opossum {

std::ostream& get_out_stream(const bool verbose) {
  if (verbose) {
    return std::cout;
  }

  // Create no-op stream that just swallows everything streamed into it
  // See https://stackoverflow.com/a/11826666
  class NullBuffer : public std::streambuf {
   public:
    int overflow(int c) { return c; }
  };

  static NullBuffer null_buffer;
  static std::ostream null_stream(&null_buffer);
  return null_stream;
}

BenchmarkState::BenchmarkState(const size_t max_num_iterations, const opossum::Duration max_duration)
    : max_num_iterations(max_num_iterations), max_duration(max_duration) {}

bool BenchmarkState::keep_running() {
  switch (state) {
    case State::NotStarted:
      begin = std::chrono::high_resolution_clock::now();
      state = State::Running;
      break;
    case State::Over:
      return false;
    default: {}
  }

  if (num_iterations >= max_num_iterations) {
    end = std::chrono::high_resolution_clock::now();
    state = State::Over;
    return false;
  }

  end = std::chrono::high_resolution_clock::now();
  const auto duration = end - begin;
  if (duration >= max_duration) {
    state = State::Over;
    return false;
  }

  num_iterations++;

  return true;
}

BenchmarkConfig::BenchmarkConfig(const BenchmarkMode benchmark_mode, const bool verbose, const ChunkOffset chunk_size,
                                 const EncodingType encoding_type, const size_t max_num_query_runs,
                                 const Duration& max_duration, const UseMvcc use_mvcc,
                                 const std::optional<std::string>& output_file_path, const bool enable_scheduler,
                                 const bool enable_visualization, std::ostream& out)
    : benchmark_mode(benchmark_mode),
      verbose(verbose),
      chunk_size(chunk_size),
      encoding_type(encoding_type),
      max_num_query_runs(max_num_query_runs),
      max_duration(max_duration),
      use_mvcc(use_mvcc),
      output_file_path(output_file_path),
      enable_scheduler(enable_scheduler),
      enable_visualization(enable_visualization),
      out(out) {}

BenchmarkConfig BenchmarkConfig::get_default_config() { return BenchmarkConfig(); }

bool CLIConfigParser::cli_has_json_config(const int argc, char** argv) {
  return argc > 1 && boost::algorithm::ends_with(argv[1], ".json");
}

nlohmann::json CLIConfigParser::config_file_to_json(const std::string& json_file_str) {
  Assert(filesystem::is_regular_file(json_file_str), "No such file " + json_file_str);

  nlohmann::json json_config;
  std::ifstream json_file{json_file_str};
  json_file >> json_config;

  return json_config;
}

BenchmarkConfig CLIConfigParser::parse_default_json_config(const nlohmann::json& json_config) {
  const auto default_config = BenchmarkConfig::get_default_config();

  // Should the benchmark be run in verbose mode
  const auto verbose = json_config.value("verbose", default_config.verbose);
  auto& out = get_out_stream(verbose);

  // In non-verbose mode, disable performance warnings
  std::optional<PerformanceWarningDisabler> performance_warning_disabler;
  if (!verbose) {
    performance_warning_disabler.emplace();
  }

  // Display info about output destination
  std::optional<std::string> output_file_path;
  const auto output_file_string = json_config.value("output", "");
  if (!output_file_string.empty()) {
    output_file_path = output_file_string;
    out << "- Writing benchmark results to '" << *output_file_path << "'" << std::endl;
  } else {
    out << "- Writing benchmark results to stdout" << std::endl;
  }

  // Display info about MVCC being enabled or not
  const auto enable_mvcc = json_config.value("mvcc", default_config.use_mvcc == UseMvcc::Yes);
  const auto use_mvcc = enable_mvcc ? UseMvcc::Yes : UseMvcc::No;
  out << "- MVCC is " << (enable_mvcc ? "enabled" : "disabled") << std::endl;

  // Initialise the scheduler if the benchmark was requested to run multi-threaded
  const auto enable_scheduler = json_config.value("scheduler", default_config.enable_scheduler);
  if (enable_scheduler) {
    const auto topology = Topology::create_numa_topology();
    out << "- Running in multi-threaded mode, with the following Topology:" << std::endl;
    topology->print(out);

    const auto scheduler = std::make_shared<NodeQueueScheduler>(topology);
    CurrentScheduler::set(scheduler);
  } else {
    out << "- Running in single-threaded mode" << std::endl;
  }

  // Determine benchmark and display it
  const auto benchmark_mode_str = json_config.value("mode", "IndividualQueries");
  auto benchmark_mode = BenchmarkMode::IndividualQueries;  // Just to init it deterministically
  if (benchmark_mode_str == "IndividualQueries") {
    benchmark_mode = BenchmarkMode::IndividualQueries;
  } else if (benchmark_mode_str == "PermutedQuerySets") {
    benchmark_mode = BenchmarkMode::PermutedQuerySets;
  } else {
    throw std::runtime_error("Invalid benchmark mode: '" + benchmark_mode_str + "'");
  }
  out << "- Running benchmark in '" << benchmark_mode_str << "' mode" << std::endl;

  const auto enable_visualization = json_config.value("visualize", default_config.enable_visualization);
  out << "- Visualization is " << (enable_visualization ? "on" : "off") << std::endl;

  // Get the specified encoding type
  const auto encoding_type_str = json_config.value("encoding", "dictionary");
  auto encoding_type = EncodingType::Dictionary;  // Just to init it deterministically
  if (encoding_type_str == "dictionary") {
    encoding_type = EncodingType::Dictionary;
  } else if (encoding_type_str == "runlength") {
    encoding_type = EncodingType::RunLength;
  } else if (encoding_type_str == "frameofreference") {
    encoding_type = EncodingType::FrameOfReference;
  } else if (encoding_type_str == "unencoded") {
    encoding_type = EncodingType::Unencoded;
  } else {
    throw std::runtime_error("Invalid encoding type: '" + encoding_type_str + "'");
  }

  out << "- Encoding is '" << encoding_type_str << "'" << std::endl;

  // Get all other variables
  const auto chunk_size = json_config.value("chunk_size", default_config.chunk_size);
  out << "- Chunk size is " << chunk_size << std::endl;

  const auto max_runs = json_config.value("runs", default_config.max_num_query_runs);
  out << "- Max runs per query is " << max_runs << std::endl;

  const auto max_duration = json_config.value("time", default_config.max_duration.count());
  out << "- Max duration per query is " << max_duration << " seconds" << std::endl;
  const Duration timeout_duration = std::chrono::duration_cast<opossum::Duration>(std::chrono::seconds{max_duration});

  return BenchmarkConfig{
      benchmark_mode, verbose,          chunk_size,       encoding_type,        max_runs, timeout_duration,
      use_mvcc,       output_file_path, enable_scheduler, enable_visualization, out};
}

BenchmarkConfig CLIConfigParser::parse_default_cli_options(const cxxopts::ParseResult& parse_result) {
  return parse_default_json_config(default_cli_options_to_json(parse_result));
}

nlohmann::json CLIConfigParser::default_cli_options_to_json(const cxxopts::ParseResult& parse_result) {
  nlohmann::json json_config;

  json_config.emplace("verbose", parse_result["verbose"].as<bool>());
  json_config.emplace("runs", parse_result["runs"].as<size_t>());
  json_config.emplace("chunk_size", parse_result["chunk_size"].as<ChunkOffset>());
  json_config.emplace("time", parse_result["time"].as<size_t>());
  json_config.emplace("mode", parse_result["mode"].as<std::string>());
  json_config.emplace("encoding", parse_result["encoding"].as<std::string>());
  json_config.emplace("scheduler", parse_result["scheduler"].as<bool>());
  json_config.emplace("mvcc", parse_result["mvcc"].as<bool>());
  json_config.emplace("visualize", parse_result["visualize"].as<bool>());

  std::string output_file_path;
  if (parse_result.count("output") > 0) {
    output_file_path = parse_result["output"].as<std::string>();
  }
  json_config.emplace("output", output_file_path);

  return json_config;
}
}  // namespace opossum
