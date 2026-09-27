#ifndef BREAKOUT_STRATEGY_HPP
#define BREAKOUT_STRATEGY_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "MarketData.hpp"

enum class StopMode
{
    Percent, // Stop distance = stop_value * entry price
    Atr      // Stop distance = stop_value * ATR of the signal bar
};

enum class SizingMode
{
    Risk,    // The loss at the stop is risk_per_trade * equity
    Notional // The position value is notional_fraction * equity
};

enum class ExitReason
{
    StopLoss,
    TrailingStop,
    TakeProfit,
    ExitChannel,
    TimeStop,
    EndOfData,
    Open // The position is still open (live mode), it is valued with the last price
};

// Parameters of the breakout strategy. The rolling optimization searches channel_length,
// stop_value and take_profit_ratio, the others are the same for all the combinations.
struct StrategyParams
{
    int timeframe_minutes = 60;
    int channel_length = 50;        // "Sensitivity": number of bars of the breakout channel
    StopMode stop_mode = StopMode::Atr;
    double stop_value = 2.0;
    double take_profit_ratio = 0.0; // Take profit distance = ratio * stop distance, 0 = no take profit
    bool trailing_stop = false;     // The stop follows the best close with the stop distance
    int exit_channel_length = 0;    // Exit when the close breaks the opposite channel of this length, 0 = off
    int trend_ema_length = 0;       // Only trade in the direction of the ema, 0 = off
    int max_hold_bars = 0;          // Exit after this number of bars, 0 = off
    int cooldown_bars = 5;          // Bars to wait after closing a trade
    bool allow_long = true;
    bool allow_short = true;
};

// Costs and position sizing.
struct ExecutionConfig
{
    double taker_fee = 0.0005; // Market and stop orders
    double maker_fee = 0.0002; // Take profit limit orders
    double slippage = 0.0002;  // Price slippage of market and stop orders
    SizingMode sizing_mode = SizingMode::Risk;
    double risk_per_trade = 0.01;
    double notional_fraction = 1.0;
    double max_leverage = 3.0;
    bool apply_funding = true;
};

struct Trade
{
    int side = 0; // +1 long, -1 short
    size_t entry_index = 0;
    size_t exit_index = 0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double quantity = 0.0;
    double stop_distance = 0.0; // Initial stop distance in price
    double equity_before = 0.0;
    double gross_pnl = 0.0;
    double entry_fee = 0.0;
    double exit_fee = 0.0;
    double funding = 0.0; // Negative = paid
    double net_pnl = 0.0;
    ExitReason exit_reason = ExitReason::EndOfData;
    int channel_length = 0;
    double stop_value = 0.0;
    double take_profit_ratio = 0.0;
    double initial_stop_price = 0.0;
    double take_profit_price = 0.0; // 0 = no take profit
    // Stop price of the trade: (first candle with this stop, stop price). Only with record_stops.
    std::vector<std::pair<size_t, double>> stop_path;
};

struct SimulationRange
{
    size_t begin = 0;      // First candle that can close a signal bar
    size_t entry_end = 0;  // No new positions are opened at or after this candle
    size_t data_end = 0;   // An open position is closed at the close of the candle before this one
    size_t max_trades = 0; // Stop after this number of closed trades
    bool record_stops = false; // Save the stop price changes of the trades (for the charts)
    bool close_at_end = true;  // false: a position at the end of the data stays open (ExitReason::Open)
};

struct SimulationResult
{
    std::vector<Trade> trades;
    double final_equity = 0.0;
    size_t next_index = 0; // First candle which is not processed yet (includes the cooldown)
};

namespace BreakoutStrategy
{
    SimulationResult simulate(const MarketData &market, const StrategyParams &params,
                              const ExecutionConfig &execution, const SimulationRange &range,
                              double starting_equity);

    const char *toString(ExitReason reason);
}

#endif // BREAKOUT_STRATEGY_HPP
