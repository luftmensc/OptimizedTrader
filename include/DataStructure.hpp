#pragma once

#include <vector>
#include <string>
#include <tuple>

struct DataRow {    std::string open_time;
    float open;
    float high;
    float low;
    float close;
};

struct Trade
{
    std::string open_time;
    float opening_value;
    float closing_value;
    std::string direction;
    float profit_or_loss;
};

struct TradingState
{
    bool in_position = false;
    std::string position_type;
    double entry_price = 0.0f;
    double sl_price = 0.0f;
    double tp_price = 0.0f;
    double balance = 1000.0f;
    double position_size = 0.0f;
    double position_size_qty = 0.0f;
    int total_wins = 0;
    int total_losses = 0;
    int trade_count_since_last_recalc = 0;
    std::vector<Trade> trades;
    float entry_atr =   0.0f;
    float entry_adx = 0.0f;
    std::string entry_time;
};

struct ResultHighBroke
{
    double best_balance = 0.0f;
    int best_sensitivity = 0;
    float best_tpsl = 0.0f;
    int total_wins = 0;
    int total_losses = 0;
    float best_win_rate = 0.0f;
};

struct OptimizationParams {
    std::string csv_file;
    std::vector<int> sensitivity_values;
    std::vector<float> tpsl_values;
    int lookback_days;
    size_t apply_trades;
    float commission_per_trade;
    // Constructor for convenience
    OptimizationParams(const std::string& csv,
                       const std::vector<int>& sensitivity,
                       const std::vector<float>& tpsl,
                       int lookback,
                       size_t applyTrades,
                       float commission)
        : csv_file(csv),
          sensitivity_values(sensitivity),
          tpsl_values(tpsl),
          lookback_days(lookback),
          apply_trades(applyTrades),
          commission_per_trade(commission){}
};

struct OptimizationResult {
    float overall_balance;
    float overall_reduced_balance;
    float final_next_amount;
    int wins;
    int losses;
    int total_trades;

    // Constructor for convenience
    OptimizationResult(float balance,
                       float reducedBalance,
                       float nextAmount,
                       int winCount,
                       int lossCount,
                       int tradeCount)
        : overall_balance(balance),
          overall_reduced_balance(reducedBalance),
          final_next_amount(nextAmount),
          wins(winCount),
          losses(lossCount),
          total_trades(tradeCount) {}
};

struct TradeSimulationParams {
    std::vector<DataRow> &data;
    int sensitivity;
    float tpsl;
    int &total_wins;
    int &total_losses;
    float multiplier;
    size_t start_index;
    size_t max_trades;
    float initial_trade_size;
    float starting_state_balance;
    float first_balance;
    int trading_count;
    float &total_traded_volume;
    bool loggingEnabled = false;
    std::string logFileName = "";

    // Constructor
    TradeSimulationParams(std::vector<DataRow> &data_,
                          int sensitivity_,
                          float tpsl_,
                          int &total_wins_,
                          int &total_losses_,
                          float multiplier_,
                          size_t start_index_,
                          size_t max_trades_,
                          float initial_trade_size_,
                          float starting_state_balance_,
                          float first_balance_,
                          int trading_count_,
                          float &total_traded_volume_
                        )
        : data(data_),
          sensitivity(sensitivity_),
          tpsl(tpsl_),
          total_wins(total_wins_),
          total_losses(total_losses_),
          multiplier(multiplier_),
          start_index(start_index_),
          max_trades(max_trades_),
          initial_trade_size(initial_trade_size_),
          starting_state_balance(starting_state_balance_),
          first_balance(first_balance_),
          trading_count(trading_count_),
          total_traded_volume(total_traded_volume_)
    {}
};

struct TradeSimulationResult {
    double final_balance;
    size_t last_index;
    float updated_next_amount;

    // Constructor
    TradeSimulationResult(double balance,
                          size_t index,
                          float nextAmount)
        : final_balance(balance),
          last_index(index),
          updated_next_amount(nextAmount) {}
};
