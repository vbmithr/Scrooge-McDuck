#include "functions/scanner.hpp"
#include "duckdb/execution/operator/persistent/csv_reader_options.hpp"
#include "duckdb/main/relation/read_csv_relation.hpp"

namespace scrooge {

int64_t IntervalInEpoch(std::string &interval) {
  // ble string checkaroo
  if (interval == "1d") {
    return duckdb::Interval::SECS_PER_DAY;
  } else if (interval == "5d") {
    return 5 * duckdb::Interval::SECS_PER_DAY;
  } else if (interval == "1wk") {
    return 7 * duckdb::Interval::SECS_PER_DAY;
  } else if (interval == "1mo") {
    return 30 * duckdb::Interval::SECS_PER_DAY;
  } else if (interval == "3mo") {
    return 90 * duckdb::Interval::SECS_PER_DAY;
  }
  return 0;
}

struct YahooFunctionData : public duckdb::TableFunctionData {
  YahooFunctionData(std::unique_ptr<duckdb::Connection> conn_p,
                    std::string &symbol_p, int64_t from_epoch_p,
                    int64_t to_epoch_p, std::string &interval_p)
      : conn(std::move(conn_p)), symbol(symbol_p), from_epoch(from_epoch_p),
        to_epoch(to_epoch_p), interval(interval_p) {
    auto interval_epoch = IntervalInEpoch(interval);
    // We have to do this hacky thing to keep yahoo finance requests happy
    int expected_tuples = (to_epoch - from_epoch) / interval_epoch + 1;
    if (expected_tuples > 60) {
      increment_epoch = (to_epoch - from_epoch) / (expected_tuples / 60 + 1);
    } else {
      increment_epoch = (to_epoch - from_epoch);
    }
    cur_to_epoch = from_epoch + increment_epoch < to_epoch
                       ? from_epoch + increment_epoch
                       : to_epoch;
  }
  std::shared_ptr<duckdb::Relation> plan;
  std::unique_ptr<duckdb::Connection> conn;
  std::string symbol;
  int64_t from_epoch;
  int64_t cur_to_epoch;
  int64_t to_epoch;
  std::string interval;
  int64_t increment_epoch;
};

duckdb::shared_ptr<duckdb::ReadCSVRelation>
GeneratePlan(YahooFunctionData &bind_data) {
  if (bind_data.cur_to_epoch > bind_data.to_epoch) {
    // we are done
    return nullptr;
  }
  auto from = std::to_string(bind_data.from_epoch);
  auto to = std::to_string(bind_data.cur_to_epoch);
  // Increment start
  bind_data.from_epoch += bind_data.increment_epoch;
  bind_data.cur_to_epoch += bind_data.increment_epoch;

  std::string url = "https://query1.finance.yahoo.com/v7/finance/download/" +
                    bind_data.symbol + "?period1=" + from + "&period2=" + to +
                    "&interval=" + bind_data.interval + "&events=history";
  std::vector<duckdb::ColumnDefinition> column_def;
  column_def.emplace_back("Date", duckdb::LogicalType::DATE);
  column_def.emplace_back("Open", duckdb::LogicalType::DOUBLE);
  column_def.emplace_back("High", duckdb::LogicalType::DOUBLE);
  column_def.emplace_back("Low", duckdb::LogicalType::DOUBLE);
  column_def.emplace_back("Close", duckdb::LogicalType::DOUBLE);
  column_def.emplace_back("Adj Close", duckdb::LogicalType::DOUBLE);
  column_def.emplace_back("Volume", duckdb::LogicalType::HUGEINT);
  auto csv_rel = duckdb::make_shared<duckdb::ReadCSVRelation>(
      bind_data.conn->context, url, std::move(column_def));
  csv_rel->AddNamedParameter("HEADER", true);
  csv_rel->AddNamedParameter("NULLSTR", "null");
  return csv_rel;
}

void ValidInterval(std::string &interval) {
  std::unordered_set<std::string> valid_interval{"1d", "5d", "1wk", "1mo",
                                                 "3mo"};
  if (valid_interval.find(interval) == valid_interval.end()) {
    std::string accepted_intervals =
        "1d: 1 day interval\n5d: 5 day interval\n1wk: 1 week interval\n1mo: 1 "
        "month interval\n3mo: 3 month interval\n";
    throw duckdb::InvalidInputException(
        "Interval is not valid, you should use one of the following valid "
        "intervals: \n" +
        accepted_intervals);
  }
}

std::unique_ptr<duckdb::FunctionData>
YahooScanner::Bind(duckdb::ClientContext &context,
                   duckdb::TableFunctionBindInput &input,
                   std::vector<duckdb::LogicalType> &return_types,
                   std::vector<std::string> &names) {
  if (input.inputs[1].type() != duckdb::LogicalType::VARCHAR &&
      input.inputs[1].type() != duckdb::LogicalType::DATE) {
    throw duckdb::InvalidInputException(
        "Start Period must be a Date or a Date-VARCHAR ");
  }
  if (input.inputs[2].type() != duckdb::LogicalType::VARCHAR &&
      input.inputs[2].type() != duckdb::LogicalType::DATE) {
    throw duckdb::InvalidInputException(
        "Start Period must be a Date or a Date-VARCHAR ");
  }
  auto symbol = input.inputs[0].GetValueUnsafe<std::string>();
  auto from_date = input.inputs[1].GetValue<duckdb::date_t>();
  auto to_date = input.inputs[2].GetValue<duckdb::date_t>();
  auto from = duckdb::Date::Epoch(input.inputs[1].GetValue<duckdb::date_t>());
  auto to = duckdb::Date::Epoch(input.inputs[2].GetValue<duckdb::date_t>());
  auto interval = input.inputs[3].GetValue<std::string>();
  ValidInterval(interval);
  if (to_date <= from_date) {
    throw duckdb::InvalidInputException(
        "The End period must be higher than the start period");
  }
  auto result = duckdb::make_unique<YahooFunctionData>(
      duckdb::make_unique<duckdb::Connection>(*context.db), symbol, from, to,
      interval);
  result->plan = GeneratePlan(*result);
  for (auto &column : result->plan->Columns()) {
    return_types.emplace_back(column.Type());
    names.emplace_back(column.Name());
  }
  return std::move(result);
}
void YahooScanner::Scan(duckdb::ClientContext &context,
                        duckdb::TableFunctionInput &data_p,
                        duckdb::DataChunk &output) {

  auto &data = (YahooFunctionData &)*data_p.bind_data;
  if (!data.plan) {
    return;
  }
  std::unique_ptr<duckdb::QueryResult> res = data.plan->Execute();
  auto result_chunk = res->Fetch();
  if (!result_chunk) {
    return;
  }
  output.Move(*result_chunk);
  data.plan = GeneratePlan(data);
}
} // namespace scrooge