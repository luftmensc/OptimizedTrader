#ifndef PERFORMANCE_HPP
#define PERFORMANCE_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "BreakoutStrategy.hpp"
#include "MarketData.hpp"
#include "WalkForward.hpp"

// Equity sampled every hour. Open positions are valued with the market price.
struct EquityCurve
{
    int64_t start_time = 0;
    static constexpr int64_t STEP_SECONDS = 3600;
    std::vector<double> equity;
};

struct PerformanceReport
{
    // From the equity curve
    double starting_equity = 0.0;
    double final_equity = 0.0;
    double days = 0.0;
    double total_return = 0.0;
    double cagr = 0.0;
    double annual_volatility = 0.0;
    double sharpe = 0.0;
    double sortino = 0.0;
    double calmar = 0.0;
    double max_drawdown = 0.0;
    double max_drawdown_days = 0.0; // Longest time between two equity highs
    double best_day = 0.0;
    double worst_day = 0.0;
    double positive_months = 0.0; // Ratio of the months with a profit
    std::vector<std::pair<std::string, double>> monthly_returns;

    // From the trades
    size_t trades = 0;
    size_t wins = 0;
    size_t long_trades = 0;
    size_t short_trades = 0;
    size_t max_consecutive_losses = 0;
    double win_rate = 0.0;
    double profit_factor = 0.0;
    double average_win = 0.0;  // Average return of the winning trades on the equity
    double average_loss = 0.0; // Average return of the losing trades on the equity
    double payoff_ratio = 0.0;
    double expectancy = 0.0;   // Average return of a trade on the equity
    double expectancy_r = 0.0; // Average profit of a trade as a multiple of the risked amount
    double average_hold_hours = 0.0;
    double exposure = 0.0; // Ratio of the time with an open position
    double gross_pnl = 0.0;
    double total_fees = 0.0;
    double total_funding = 0.0;
    double long_pnl = 0.0;
    double short_pnl = 0.0;
};

namespace Performance
{
    // start_time and end_time must be on a full hour.
    EquityCurve buildEquityCurve(const MarketData &market, const WalkForwardResult &result,
                                 int64_t start_time, int64_t end_time);

    // Equity of holding the symbol without leverage.
    EquityCurve buildBuyAndHoldCurve(const MarketData &market, double starting_equity,
                                     int64_t start_time, int64_t end_time);

    EquityCurve sum(const std::vector<EquityCurve> &curves);

    // The trades can be from more than one symbol. symbol_count is used for the exposure.
    PerformanceReport analyze(const EquityCurve &curve, const std::vector<Trade> &trades,
                              double starting_equity, double final_equity, size_t symbol_count);
}

#endif // PERFORMANCE_HPP
