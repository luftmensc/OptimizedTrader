#include "BreakoutStrategy.hpp"

#include <algorithm>
#include <cmath>

namespace BreakoutStrategy
{
    const char *toString(ExitReason reason)
    {
        switch (reason)
        {
        case ExitReason::StopLoss:
            return "StopLoss";
        case ExitReason::TrailingStop:
            return "TrailingStop";
        case ExitReason::TakeProfit:
            return "TakeProfit";
        case ExitReason::ExitChannel:
            return "ExitChannel";
        case ExitReason::TimeStop:
            return "TimeStop";
        case ExitReason::EndOfData:
            return "EndOfData";
        case ExitReason::Open:
            return "Open";
        }
        return "";
    }

    // Index of the first bar which closes at or after the given candle.
    static size_t firstBarClosingFrom(const TimeframeBars &bars, size_t candle_index)
    {
        if (bars.end_index.empty() || candle_index <= bars.end_index.front())
            return 0;
        const size_t minutes = static_cast<size_t>(bars.minutes);
        return (candle_index - bars.end_index.front() + minutes - 1) / minutes;
    }

    SimulationResult simulate(const MarketData &market, const StrategyParams &params,
                              const ExecutionConfig &execution, const SimulationRange &range,
                              double starting_equity)
    {
        const CandleSeries &candles = market.candles;
        const TimeframeBars &bars = market.bars(params.timeframe_minutes);
        const Channel &entry_channel = bars.channels.at(params.channel_length);
        const Channel *exit_channel =
            params.exit_channel_length > 0 ? &bars.channels.at(params.exit_channel_length) : nullptr;
        const std::vector<double> *ema =
            params.trend_ema_length > 0 ? &bars.emas.at(params.trend_ema_length) : nullptr;

        const size_t data_end = std::min(range.data_end, candles.size());
        const size_t entry_end = std::min(range.entry_end, data_end);
        const size_t cooldown_candles =
            static_cast<size_t>(params.cooldown_bars) * static_cast<size_t>(params.timeframe_minutes);

        SimulationResult result;
        double equity = starting_equity;
        size_t i = range.begin;

        while (result.trades.size() < range.max_trades && equity > 0.0)
        {
            // Flat: look for the next bar which closes outside of the channel
            int side = 0;
            size_t signal_bar = 0;
            for (size_t k = firstBarClosingFrom(bars, i); k < bars.close.size(); ++k)
            {
                if (bars.end_index[k] + 1 >= entry_end)
                    break;

                const double close = bars.close[k];
                const double channel_high = entry_channel.high[k];
                const double channel_low = entry_channel.low[k];
                if (std::isnan(channel_high) || std::isnan(bars.atr[k]) || (ema && std::isnan((*ema)[k])))
                    continue;

                if (params.allow_long && close > channel_high && (!ema || close > (*ema)[k]))
                    side = 1;
                else if (params.allow_short && close < channel_low && (!ema || close < (*ema)[k]))
                    side = -1;

                if (side != 0)
                {
                    signal_bar = k;
                    break;
                }
            }

            if (side == 0)
            {
                i = std::max(i, entry_end);
                break;
            }

            // The signal is known at the close of the bar, the market order is filled on the next open
            const size_t entry_index = bars.end_index[signal_bar] + 1;
            const double entry_price = candles.open[entry_index] * (1.0 + side * execution.slippage);
            const double stop_distance = (params.stop_mode == StopMode::Percent)
                                             ? params.stop_value * entry_price
                                             : params.stop_value * bars.atr[signal_bar];

            double quantity = (execution.sizing_mode == SizingMode::Risk)
                                  ? equity * execution.risk_per_trade / stop_distance
                                  : equity * execution.notional_fraction / entry_price;
            quantity = std::min(quantity, equity * execution.max_leverage / entry_price);

            Trade trade;
            trade.side = side;
            trade.entry_index = entry_index;
            trade.entry_price = entry_price;
            trade.quantity = quantity;
            trade.stop_distance = stop_distance;
            trade.equity_before = equity;
            trade.entry_fee = quantity * entry_price * execution.taker_fee;
            trade.channel_length = params.channel_length;
            trade.stop_value = params.stop_value;
            trade.take_profit_ratio = params.take_profit_ratio;

            const double initial_stop = entry_price - side * stop_distance;
            double stop_price = initial_stop;
            const bool has_take_profit = params.take_profit_ratio > 0.0;
            const double take_profit_price = entry_price + side * params.take_profit_ratio * stop_distance;
            trade.initial_stop_price = initial_stop;
            trade.take_profit_price = has_take_profit ? take_profit_price : 0.0;
            if (range.record_stops)
                trade.stop_path.emplace_back(entry_index, initial_stop);
            double best_close = entry_price;
            int bars_held = 0;
            bool exit_on_open = false;
            bool closed = false;
            double exit_fee_rate = execution.taker_fee;

            size_t j = entry_index;
            for (; j < data_end; ++j)
            {
                if (exit_on_open)
                {
                    trade.exit_price = candles.open[j] * (1.0 - side * execution.slippage);
                    closed = true;
                    break;
                }

                if (execution.apply_funding && j > entry_index && market.funding_rate[j] != 0.0)
                    trade.funding -= side * quantity * candles.open[j] * market.funding_rate[j];

                // If the stop and the take profit are inside of the same candle, the stop is assumed to be first
                const bool stop_hit = (side > 0) ? candles.low[j] <= stop_price : candles.high[j] >= stop_price;
                if (stop_hit)
                {
                    // A candle which opens beyond the stop is filled at the open
                    double fill = (side > 0) ? std::min(stop_price, candles.open[j])
                                             : std::max(stop_price, candles.open[j]);
                    trade.exit_price = fill * (1.0 - side * execution.slippage);
                    trade.exit_reason = (stop_price != initial_stop) ? ExitReason::TrailingStop
                                                                     : ExitReason::StopLoss;
                    closed = true;
                    break;
                }

                const bool take_profit_hit =
                    has_take_profit &&
                    ((side > 0) ? candles.high[j] >= take_profit_price : candles.low[j] <= take_profit_price);
                if (take_profit_hit)
                {
                    trade.exit_price = take_profit_price;
                    trade.exit_reason = ExitReason::TakeProfit;
                    exit_fee_rate = execution.maker_fee;
                    closed = true;
                    break;
                }

                // Decisions which are made at the close of a bar
                const int32_t k = bars.bar_of_candle[j];
                if (k >= 0)
                {
                    ++bars_held;
                    const double close = bars.close[k];

                    if (params.trailing_stop)
                    {
                        best_close = (side > 0) ? std::max(best_close, close) : std::min(best_close, close);
                        const double trailed = best_close - side * stop_distance;
                        const double previous_stop = stop_price;
                        stop_price = (side > 0) ? std::max(stop_price, trailed) : std::min(stop_price, trailed);
                        // The new stop is active from the next candle
                        if (range.record_stops && stop_price != previous_stop)
                            trade.stop_path.emplace_back(j + 1, stop_price);
                    }

                    if (exit_channel)
                    {
                        const double level = (side > 0) ? exit_channel->low[k] : exit_channel->high[k];
                        if (!std::isnan(level) && ((side > 0) ? close < level : close > level))
                        {
                            exit_on_open = true;
                            trade.exit_reason = ExitReason::ExitChannel;
                        }
                    }

                    if (!exit_on_open && params.max_hold_bars > 0 && bars_held >= params.max_hold_bars)
                    {
                        exit_on_open = true;
                        trade.exit_reason = ExitReason::TimeStop;
                    }
                }
            }

            if (!closed)
            {
                // The data ended with an open position. In the live mode it stays open and
                // the result is the value of the position if it would be closed now.
                j = data_end - 1;
                trade.exit_price = candles.close[j] * (1.0 - side * execution.slippage);
                trade.exit_reason = range.close_at_end ? ExitReason::EndOfData : ExitReason::Open;
            }

            trade.exit_index = j;
            trade.exit_fee = quantity * trade.exit_price * exit_fee_rate;
            trade.gross_pnl = side * quantity * (trade.exit_price - entry_price);
            trade.net_pnl = trade.gross_pnl - trade.entry_fee - trade.exit_fee + trade.funding;
            equity += trade.net_pnl;
            result.trades.push_back(trade);

            i = j + 1 + cooldown_candles;
        }

        result.final_equity = equity;
        result.next_index = std::min(i, data_end);
        return result;
    }
}
