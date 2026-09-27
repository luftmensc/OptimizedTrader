#ifndef WALK_FORWARD_HPP
#define WALK_FORWARD_HPP

#include <string>
#include <vector>

#include "BreakoutStrategy.hpp"
#include "MarketData.hpp"

// What the optimization maximizes on the lookback data.
enum class Objective
{
    Return,       // Net return after costs
    WinRate,      // Wins / trades (the original behaviour)
    ProfitFactor, // Sum of wins / sum of losses
    Sharpe        // Mean / deviation of the trade returns * sqrt(trades)
};

struct WalkForwardConfig
{
    StrategyParams base; // Values of the parameters which are not optimized

    // Searched combinations
    std::vector<int> channel_lengths;
    std::vector<double> stop_values;
    std::vector<double> take_profit_ratios;

    // Exit channel length = ratio * channel length, 0 = no exit channel
    double exit_channel_ratio = 0.0;

    int lookback_days = 60;  // Data used for choosing the parameters
    size_t apply_trades = 10; // The chosen parameters are used for this number of trades...
    int apply_max_days = 14;  // ...but not longer than this number of days

    Objective objective = Objective::Return;
    size_t min_trades = 5;         // Combinations with less trades on the lookback data are not trusted
    bool require_positive = false; // Do not trade when no combination made money on the lookback data

    // Live mode: the end of the data is "now", a position at the end of the data stays open
    bool live = false;

    ExecutionConfig execution;
};

struct WindowRecord
{
    size_t optimize_begin = 0;
    size_t apply_begin = 0;
    size_t apply_end = 0;
    bool traded = false;
    int channel_length = 0;
    double stop_value = 0.0;
    double take_profit_ratio = 0.0;
    double lookback_score = 0.0;
    double lookback_return = 0.0;
    size_t lookback_trades = 0;
    size_t applied_trades = 0;
    double applied_return = 0.0;
};

struct WalkForwardResult
{
    std::vector<Trade> trades;
    std::vector<WindowRecord> windows;
    double starting_equity = 0.0;
    double final_equity = 0.0;
    size_t begin_index = 0; // First candle of the first apply window
    size_t end_index = 0;   // One past the last candle
};

namespace WalkForward
{
    // Builds the bars and all the indicators which the configuration needs.
    void prepareMarket(MarketData &market, const WalkForwardConfig &config);

    // Rolling optimization: choose the parameters on the lookback data, trade them on the following
    // data, then repeat. Only the trades of the apply windows are returned, so every trade is made
    // with parameters which were chosen without seeing the future.
    WalkForwardResult run(const MarketData &market, const WalkForwardConfig &config,
                          size_t trade_begin, size_t data_end, double starting_equity);

    int exitChannelLength(int channel_length, double ratio);

    bool parseObjective(const std::string &text, Objective &objective);
}

#endif // WALK_FORWARD_HPP
