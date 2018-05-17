#include <array>
#include <experimental/filesystem>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
#include <iostream>
#include <iomanip>
#include <ctime>

#include <cxxopts.hpp>
#include <statistics/generate_table_statistics.hpp>
#include <experimental/filesystem>
#include <random>

#include "constant_mappings.hpp"
#include "logical_query_plan/logical_plan_root_node.hpp"
#include "logical_query_plan/lqp_translator.hpp"
#include "operators/cardinality_caching_callback.hpp"
#include "operators/join_hash.hpp"
#include "operators/join_sort_merge.hpp"
#include "operators/product.hpp"
#include "concurrency/transaction_context.hpp"
#include "concurrency/transaction_manager.hpp"
#include "operators/table_scan.hpp"
#include "operators/table_wrapper.hpp"
#include "operators/utils/flatten_pqp.hpp"
#include "operators/export_binary.hpp"
#include "operators/import_binary.hpp"
#include "operators/limit.hpp"
#include "optimizer/join_ordering/abstract_join_plan_node.hpp"
#include "cost_model/cost.hpp"
#include "cost_model/cost_model_naive.hpp"
#include "cost_model/cost_model_linear.hpp"
#include "import_export/csv_parser.hpp"
#include "optimizer/join_ordering/dp_ccp.hpp"
#include "optimizer/join_ordering/dp_ccp_top_k.hpp"
#include "optimizer/strategy/join_ordering_rule.hpp"
#include "statistics/table_statistics.hpp"
#include "optimizer/table_statistics_cache.hpp"
#include "planviz/lqp_visualizer.hpp"
#include "planviz/sql_query_plan_visualizer.hpp"
#include "scheduler/current_scheduler.hpp"
#include "statistics/cardinality_estimator_execution.hpp"
#include "statistics/cardinality_estimator_column_statistics.hpp"
#include "statistics/cardinality_estimation_cache.hpp"
#include "statistics/cardinality_estimator_cached.hpp"
#include "statistics/statistics_import_export.hpp"
#include "sql/sql_pipeline_statement.hpp"
#include "sql/sql.hpp"
#include "storage/storage_manager.hpp"
#include "tpch/tpch_db_generator.hpp"
#include "tpch/tpch_queries.hpp"
#include "utils/timer.hpp"
#include "utils/table_generator2.hpp"
#include "utils/format_duration.hpp"
#include "optimizer/table_statistics_cache.hpp"

#include <boost/lexical_cast.hpp>
using boost::lexical_cast;

#include <boost/uuid/uuid.hpp>
using boost::uuids::uuid;

#include <boost/uuid/uuid_generators.hpp>
using boost::uuids::random_generator;

#include <boost/uuid/uuid_io.hpp>
#include <cost_model/cost_feature_lqp_node_proxy.hpp>
#include <cost_model/cost_feature_operator_proxy.hpp>

#include "join_ordering_evaluator/job_join_ordering_workload.hpp"
#include "join_ordering_evaluator/tpch_join_ordering_workload.hpp"
#include "join_ordering_evaluator/join_ordering_evaluator_config.hpp"

namespace {

using namespace std::string_literals;  // NOLINT
using namespace opossum;  // NOLINT

struct PlanMeasurement final {
  long duration{0};
  Cost est_cost{0.0f};
  Cost re_est_cost{0.0f};
  Cost aim_cost{0.0f};
  Cost abs_est_cost_error{0.0f};
  Cost abs_re_est_cost_error{0.0f};
};

struct QueryIterationMeasurement final {
  long duration{0};
  size_t cache_hit_count{0};
  size_t cache_miss_count{0};
  size_t cache_size{0};
  size_t cache_distinct_hit_count{0};
  size_t cache_distinct_miss_count{0};
};

struct QueryMeasurement final {
  std::string name;
  long best_plan_duration{0};
};

PlanMeasurement create_plan_measurement(const AbstractCostModel &cost_model,
                                         const std::vector<std::shared_ptr<AbstractOperator>> &operators) {
  PlanMeasurement sample;

  for (const auto &op : operators) {
    const auto aim_cost = cost_model.get_reference_operator_cost(op);
    sample.aim_cost += aim_cost;

    if (op->lqp_node()) {
      const auto est_cost = cost_model.estimate_cost(CostFeatureLQPNodeProxy(op->lqp_node()));
        sample.est_cost += est_cost;
        if (aim_cost) {
          sample.abs_est_cost_error += std::fabs(est_cost - aim_cost);
        }
    }

    const auto re_est_cost = cost_model.estimate_cost(CostFeatureOperatorProxy(op));
      sample.re_est_cost += re_est_cost;
      sample.abs_re_est_cost_error += std::fabs(re_est_cost - aim_cost);
  }

  return sample;
}

std::ostream &operator<<(std::ostream &stream, const PlanMeasurement &sample) {
  stream << sample.duration << "," << sample.est_cost << "," << sample.re_est_cost << "," << sample.aim_cost << ","
         << sample.abs_est_cost_error << "," << sample.abs_re_est_cost_error;
  return stream;
}


std::ostream &operator<<(std::ostream &stream, const QueryIterationMeasurement &sample) {
  stream << sample.duration << "," << sample.cache_hit_count << "," << sample.cache_miss_count << "," <<
         sample.cache_size << "," << sample.cache_distinct_hit_count << "," << sample.cache_distinct_miss_count;
  return stream;
}

std::ostream &operator<<(std::ostream &stream, const QueryMeasurement &sample) {
  stream << sample.name << "," << sample.best_plan_duration;
  return stream;
}

}

static JoinOrderingEvaluatorConfig config;
static std::string evaluation_dir;
static std::string tmp_dot_file_path;
static std::shared_ptr<CardinalityEstimationCache> cardinality_estimation_cache;
static std::shared_ptr<AbstractCardinalityEstimator> fallback_cardinality_estimator;
static std::shared_ptr<AbstractCardinalityEstimator> main_cardinality_estimator;
static std::vector<QueryMeasurement> query_measurements;

struct QueryState final {
  std::string name;
  std::chrono::steady_clock::time_point execution_begin;
  std::string sql;
  std::shared_ptr<AbstractCostModel> cost_model;
  std::shared_ptr<AbstractLQPNode> lqp_root;
  std::shared_ptr<JoinGraph> join_graph;
  bool save_plan_results{false};
  std::vector<QueryIterationMeasurement> measurements;
  long best_plan_microseconds{std::numeric_limits<long>::max()};

  std::unordered_set<std::shared_ptr<AbstractLQPNode>, LQPHash, LQPEqual> executed_plans;
};

struct QueryIterationState final {
  size_t idx{0};
  std::string name;
  std::optional<long> current_plan_timeout;
  std::vector<PlanMeasurement> measurements;
  long best_plan_microseconds{std::numeric_limits<long>::max()};
  size_t executed_plans_count{0};
};

struct JoinPlanState final {
  size_t idx{0};
  JoinPlanNode join_plan;
};

void evaluate_join_plan(QueryState& query_state,
                        QueryIterationState& query_iteration_state,
                        JoinPlanState& join_plan_state,
                        const std::shared_ptr<AbstractCostModel>& cost_model) {
  out() << "---- JoinPlan " << join_plan_state.idx << ", estimated cost: " << join_plan_state.join_plan.plan_cost << std::endl;

  // Create LQP from join plan
  const auto join_ordered_sub_lqp = join_plan_state.join_plan.lqp;
  for (const auto &parent_relation : query_state.join_graph->output_relations) {
    parent_relation.output->set_input(parent_relation.input_side, join_ordered_sub_lqp);
  }

  // Translate to PQP
  LQPTranslator lqp_translator;
  lqp_translator.add_post_operator_callback(
  std::make_shared<CardinalityCachingCallback>(cardinality_estimation_cache));

  if (config.unique_plans && !query_state.executed_plans.emplace(query_state.lqp_root->left_input()).second) {
    if (config.force_plan_zero && join_plan_state.idx == 0) {
      out() << "----- Plan was already executed, but is rank#0 and --force-plan-zero is set, so it is executed again" << std::endl;
    } else {
      out() << "----- Plan was already executed, skipping" << std::endl;
      return;
    }
  }

  const auto pqp = lqp_translator.translate_node(query_state.lqp_root->left_input());

  auto transaction_context = TransactionManager::get().new_transaction_context();
  pqp->set_transaction_context_recursively(transaction_context);

  // Schedule timeout
  if (query_iteration_state.current_plan_timeout) {
    const auto seconds = *query_iteration_state.current_plan_timeout;
    std::thread timeout_thread([transaction_context, seconds]() {
      std::this_thread::sleep_for(std::chrono::seconds(seconds + 2));
      if (transaction_context->rollback(TransactionPhaseSwitch::Lenient)) {
        out() << "----- Query timeout signalled" << std::endl;
      }
    });
    timeout_thread.detach();
  }

  // Execute Plan
  SQLQueryPlan plan;
  plan.add_tree_by_root(pqp);

  Timer timer;
  CurrentScheduler::schedule_and_wait_for_tasks(plan.create_tasks());

  ++query_iteration_state.executed_plans_count;

  if (!transaction_context->commit(TransactionPhaseSwitch::Lenient)) {
    out() << "----- Query timeout accepted" << std::endl;
    return;
  }

  /**
   * Save measurements
   */
  const auto plan_duration = timer.lap();

  const auto operators = flatten_pqp(pqp);
  auto plan_measurement = create_plan_measurement(*cost_model, operators);
  plan_measurement.duration = plan_duration.count();
  query_iteration_state.measurements[join_plan_state.idx] = plan_measurement;

  /**
   * Visualize
   */
  if (config.visualize) {
    GraphvizConfig graphviz_config;
    graphviz_config.format = "svg";
    VizGraphInfo viz_graph_info;
    viz_graph_info.bg_color = "black";
    try {
      SQLQueryPlanVisualizer visualizer{graphviz_config, viz_graph_info, {}, {}};
      visualizer.set_cost_model(cost_model);
      visualizer.visualize(plan, tmp_dot_file_path,
                           std::string(evaluation_dir + "/viz/") + query_iteration_state.name + "_" + std::to_string(join_plan_state.idx) +
                           "_" +
                           std::to_string(plan_duration.count()) + ".svg");
    } catch (const std::exception &e) {
      out() << "----- Error while visualizing: " << e.what() << std::endl;
    }
  }

  if (query_state.save_plan_results) {
    auto output_wrapper = std::make_shared<TableWrapper>(plan.tree_roots().at(0)->get_output());
    output_wrapper->execute();

    auto limit = std::make_shared<Limit>(output_wrapper, 500);
    limit->execute();

    std::ofstream output_file(query_state.name + ".result.txt");
    output_file << "Total Row Count: " << plan.tree_roots().at(0)->get_output()->row_count() << std::endl;
    output_file << std::endl;
    Print::print(limit->get_output(), 0, output_file);

    query_state.save_plan_results = false;
  }

  /**
   * Adjust dynamic timeout
   */
  const auto plan_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(plan_duration).count();
  if (plan_microseconds < query_iteration_state.best_plan_microseconds) {
    query_iteration_state.best_plan_microseconds = plan_microseconds;

    if (config.dynamic_plan_timeout_enabled) {
      query_iteration_state.current_plan_timeout = (query_iteration_state.best_plan_microseconds / 1000000) * 1.2f + 2;
      out() << "----- New dynamic timeout is " << *query_iteration_state.current_plan_timeout << " seconds"
            << std::endl;
    }
  }

  /**
   * CSV
   */
  if (config.save_query_iterations_results) {
    auto csv = std::ofstream{evaluation_dir + "/" + query_iteration_state.name + ".csv"};
    csv << "Idx,Duration,EstCost,ReEstCost,AimCost,AbsEstCostError,AbsReEstCostError" << "\n";
    for (auto plan_idx = size_t{0}; plan_idx < query_iteration_state.measurements.size(); ++plan_idx) {
      csv << plan_idx << "," << query_iteration_state.measurements[plan_idx] << "\n";
    }
    csv.close();
  }
}

void evaluate_query_iteration(QueryState &query_state, QueryIterationState &query_iteration_state,
                              const std::shared_ptr<AbstractCostModel> &cost_model) {
  QueryIterationMeasurement measurement;

  auto pipeline_statement = SQL{query_state.sql}.disable_mvcc().pipeline_statement();

  const auto lqp = pipeline_statement.get_optimized_logical_plan();
  query_state.lqp_root = std::shared_ptr<AbstractLQPNode>(LogicalPlanRootNode::make(lqp));
  query_state.join_graph = JoinGraph::from_lqp(lqp);

  DpCcpTopK dp_ccp_top_k{config.max_plan_generation_count ? *config.max_plan_generation_count : DpSubplanCacheTopK::NO_ENTRY_LIMIT,
                         cost_model, main_cardinality_estimator};
  dp_ccp_top_k(query_state.join_graph);

  measurement.cache_hit_count = cardinality_estimation_cache->cache_hit_count();
  measurement.cache_miss_count = cardinality_estimation_cache->cache_miss_count();
  measurement.cache_size = cardinality_estimation_cache->size();
  measurement.cache_distinct_hit_count = cardinality_estimation_cache->distinct_hit_count();
  measurement.cache_distinct_miss_count = cardinality_estimation_cache->distinct_miss_count();

  JoinVertexSet all_vertices{query_state.join_graph->vertices.size()};
  all_vertices.flip();
  const auto join_plans = dp_ccp_top_k.subplan_cache()->get_best_plans(all_vertices);

  query_iteration_state.measurements.resize(join_plans.size());

  out() << "--- Query Iteration " << query_iteration_state.idx
        << " - Generated plans: " << join_plans.size() << std::endl;


  // Shuffle plans
  std::vector<size_t> plan_indices(join_plans.size());
  std::iota(plan_indices.begin(), plan_indices.end(), 0);

  if (config.plan_order_shuffling) {
    if (plan_indices.size() > static_cast<size_t>(*config.plan_order_shuffling)) {
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(plan_indices.begin() + *config.plan_order_shuffling, plan_indices.end(), g);
    }
  }

  for (auto plan_idx_idx = size_t{0}; plan_idx_idx < plan_indices.size(); ++plan_idx_idx) {
    if (config.max_plan_execution_count && query_iteration_state.executed_plans_count >= *config.max_plan_execution_count) {
      out() << "---- Requested number of plans (" << *config.max_plan_execution_count << ") executed, stopping" << std::endl;
      break;
    }

    const auto plan_idx = plan_indices[plan_idx_idx];
    auto join_plan_iter = join_plans.begin();
    std::advance(join_plan_iter, plan_idx);
    const auto &join_plan = *join_plan_iter;

    // Timeout query
    if (config.query_timeout_seconds) {
      const auto now = std::chrono::steady_clock::now();
      const auto query_duration = now - query_state.execution_begin;

      if (std::chrono::duration_cast<std::chrono::seconds>(query_duration).count() >=
          *config.query_timeout_seconds) {
        out() << "---- Query timeout" << std::endl;
        break;
      }
    }

    JoinPlanState join_plan_state;
    join_plan_state.idx = plan_idx;
    join_plan_state.join_plan = join_plan;

    evaluate_join_plan(query_state, query_iteration_state, join_plan_state, cost_model);
  }

  /**
   * Measurement
   */
  measurement.duration = query_iteration_state.measurements[0].duration;
  query_state.measurements[query_iteration_state.idx] = measurement;

  cardinality_estimation_cache->reset_distinct_hit_miss_counts();

  /**
   * CSV
   */
  auto csv = std::ofstream{evaluation_dir + "/" + query_state.name + ".csv"};
  csv << "Idx,Duration,CECacheHitCount,CECacheMissCount,CECacheSize,CECacheDistinctHitCount,CECacheDistinctMissCount" << "\n";
  for (auto query_iteration_idx = size_t{0};
       query_iteration_idx < query_state.measurements.size(); ++query_iteration_idx) {
    csv << query_iteration_idx << "," << query_state.measurements[query_iteration_idx] << "\n";
  }
  csv.close();


}

int main(int argc, char ** argv) {
  std::cout << "Hyrise Join Ordering Evaluator" << std::endl;


  /**
   * Parse CLI options
   */
  cxxopts::Options cli_options_description{"Hyrise Join Ordering Evaluator", ""};

  config.add_options(cli_options_description);

  cli_options_description.parse_positional("queries");
  const auto cli_parse_result = cli_options_description.parse(argc, argv);

  // Display usage and quit
  if (cli_parse_result.count("help")) {
    std::cout << cli_options_description.help({}) << std::endl;
    return 0;
  }

  config.parse(cli_parse_result);

  /**
   * Create evaluation dir
   */
  evaluation_dir = "join_order_evaluations/" + config.evaluation_name;
  tmp_dot_file_path = evaluation_dir + "/" + boost::lexical_cast<std::string>((boost::uuids::random_generator())()) + ".dot";
  std::experimental::filesystem::create_directories(evaluation_dir);
  std::experimental::filesystem::create_directory(evaluation_dir + "/viz");

  /**
   * Load workload
   */
  out() << "-- Setting up workload" << std::endl;
  config.workload->setup();
  out() << std::endl;

  /**
   * Setup CardinalityEstimator
   */
  cardinality_estimation_cache = std::make_shared<CardinalityEstimationCache>();
  if (config.cardinality_estimation_mode == CardinalityEstimationMode::Cached) {
    fallback_cardinality_estimator = std::make_shared<CardinalityEstimatorColumnStatistics>();
    main_cardinality_estimator = std::make_shared<CardinalityEstimatorCached>(cardinality_estimation_cache,
                                                                              CardinalityEstimationCacheMode::ReadOnly, fallback_cardinality_estimator);
  } else {
    fallback_cardinality_estimator = std::make_shared<CardinalityEstimatorExecution>();
    main_cardinality_estimator = std::make_shared<CardinalityEstimatorCached>(cardinality_estimation_cache,
                                                                              CardinalityEstimationCacheMode::ReadAndUpdate, fallback_cardinality_estimator);
  }

  /**
   * The actual evaluation
   */
  for (const auto& cost_model : config.cost_models) {
    out() << "- Evaluating Cost Model " << cost_model->name() << std::endl;

    query_measurements.resize(config.workload->query_count());

    for (auto query_idx = size_t{0}; query_idx < config.workload->query_count(); ++query_idx) {
      QueryState query_state;

      query_state.name = config.workload->get_query_name(query_idx) + "-" + cost_model->name() + "-" + (IS_DEBUG ? "debug"s : "release"s);
      query_state.sql = config.workload->get_query(query_idx);
      query_state.execution_begin = std::chrono::steady_clock::now();
      query_state.save_plan_results = config.save_results;
      query_state.measurements.resize(config.iterations_per_query);

      out() << "-- Evaluating Query: " << query_state.name << std::endl;

      if (config.cardinality_estimation_cache_log) {
        cardinality_estimation_cache->set_log(std::make_shared<std::ofstream>(evaluation_dir + "/CardinalityEstimationCache-" + query_state.name + ".log"));
      }

      for (auto query_iteration_idx = size_t{0}; query_iteration_idx < config.iterations_per_query; ++query_iteration_idx) {
        QueryIterationState query_iteration_state;
        query_iteration_state.name = query_state.name + "-" + std::to_string(query_iteration_idx);
        query_iteration_state.current_plan_timeout = config.plan_timeout_seconds;
        query_iteration_state.idx = query_iteration_idx;

        evaluate_query_iteration(query_state, query_iteration_state, cost_model);

        query_state.best_plan_microseconds = std::min(query_iteration_state.best_plan_microseconds, query_state.best_plan_microseconds);
      }

      QueryMeasurement query_measurement;
      query_measurement.name = query_state.name;
      query_measurement.best_plan_duration = query_state.best_plan_microseconds;
      query_measurements[query_idx] = query_measurement;
      auto csv = std::ofstream{evaluation_dir + "/Queries-" + cost_model->name() + ".csv"};
      csv << "Idx,Name,BestPlanDuration" << "\n";
      for (auto query_idx = size_t{0};
           query_idx < query_measurements.size(); ++query_idx) {
        csv << query_idx << "," << query_measurements[query_idx] << "\n";
      }
      csv.close();

//      if (cardinality_estimation_cache->log()) {
//        cardinality_estimation_cache->print(*cardinality_estimation_cache->log());
//      }

      if (config.isolate_queries) {
        cardinality_estimation_cache->clear();
      }
    }
  }
}
