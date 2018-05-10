#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

#include "SQLParser.h"
#include "SQLParserResult.h"
#include "benchmark_runner.hpp"
#include "cxxopts.hpp"
#include "json.hpp"
#include "planviz/lqp_visualizer.hpp"
#include "planviz/sql_query_plan_visualizer.hpp"
#include "scheduler/current_scheduler.hpp"
#include "scheduler/node_queue_scheduler.hpp"
#include "scheduler/topology.hpp"
#include "sql/sql_pipeline.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "tpch/tpch_db_generator.hpp"
#include "tpch/tpch_queries.hpp"

/**
 * This benchmark measures Hyrise's performance executing the TPC-H *queries*, it doesn't (yet) support running the
 * TPC-H *benchmark* exactly as it is specified.
 * (Among other things, the TPC-H requires performing data refreshes and has strict requirements for the number of
 * sessions running in parallel. See http://www.tpc.org/tpch/default.asp for more info)
 * The benchmark offers a wide range of options (scale_factor, chunk_size, ...) but most notably it offers two modes:
 * IndividualQueries and PermutedQuerySets. See docs on BenchmarkMode for details.
 * The benchmark will stop issuing new queries if either enough iterations have taken place or enough time has passed.
 *
 * main() is mostly concerned with parsing the CLI options while BenchmarkRunner.run() performs the actual benchmark
 * logic.
 */

int main(int argc, char* argv[]) {
  auto cli_options = opossum::BenchmarkRunner::get_default_cli_options("TPCH Benchmark");

  // clang-format off
  cli_options.add_options()
      ("s,scale", "Database scale factor (1.0 ~ 1GB)", cxxopts::value<float>()->default_value("0.001"))
      ("queries", "Specify queries to run, default is all that are supported", cxxopts::value<std::vector<opossum::QueryID>>()); // NOLINT
  // clang-format on

  const auto cli_parse_result = cli_options.parse(argc, argv);

  // Display usage and quit
  if (cli_parse_result.count("help")) {
    std::cout << cli_options.help({}) << std::endl;
    return 0;
  }

  const bool verbose = cli_parse_result["verbose"].as<bool>();
  auto& out = opossum::get_out_stream(verbose);

  const auto config = opossum::BenchmarkRunner::parse_default_cli_options(cli_parse_result, cli_options);

  // Build list of query ids to be benchmarked and display it
  std::vector<opossum::QueryID> query_ids;
  if (cli_parse_result.count("queries")) {
    const auto cli_query_ids = cli_parse_result["queries"].as<std::vector<opossum::QueryID>>();
    for (const auto cli_query_id : cli_query_ids) {
      query_ids.emplace_back(cli_query_id);
    }
  } else {
    std::transform(opossum::tpch_queries.begin(), opossum::tpch_queries.end(), std::back_inserter(query_ids),
                   [](auto& pair) { return pair.first; });
  }
  out << "- Benchmarking Queries ";
  for (const auto query_id : query_ids) {
    out << (query_id) << " ";
  }
  out << std::endl;

  const auto scale_factor = cli_parse_result["scale"].as<float>();

  // Run the benchmark
  opossum::BenchmarkRunner::create_tpch(config, query_ids, scale_factor).run();
}
