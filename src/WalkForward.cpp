#include "WalkForward.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

namespace WalkForward
{
    constexpr size_t MINUTES_PER_DAY = 24 * 60;

    struct Candidate
    {
        StrategyParams params;
        double score = 0.0;
        double net_return = 0.0;
        size_t trades = 0;
    };

    int exitChannelLength(int channel_length, double ratio)
    {
        if (ratio <= 0.0)
            return 0;
        return std::max(1, static_cast<int>(std::lround(channel_length * ratio)));
    }

    bool parseObjective(const std::string &text, Objective &objective)
    {
        if (text == "return")
            objective = Objective::Return;
        else if (text == "winrate")
            objective = Objective::WinRate;
        else if (text == "pf")
            objective = Objective::ProfitFactor;
        else if (text == "sharpe")
            objective = Objective::Sharpe;
        else
            return false;
        return true;
    }

    void prepareMarket(MarketData &market, const WalkForwardConfig &config)
    {
        std::vector<int> channel_lengths = config.channel_lengths;
        for (int length : config.channel_lengths)
        {
            int exit_length = exitChannelLength(length, config.exit_channel_ratio);
            if (exit_length > 0)
                channel_lengths.push_back(exit_length);
        }
        market.prepare(config.base.timeframe_minutes, channel_lengths, {config.base.trend_ema_length}, 14);
    }

    static std::vector<Candidate> buildCandidates(const WalkForwardConfig &config)
    {
        std::vector<Candidate> candidates;
        for (int channel_length : config.channel_lengths)
        {
            for (double stop_value : config.stop_values)
            {
                for (double take_profit_ratio : config.take_profit_ratios)
                {
                    Candidate candidate;
                    candidate.params = config.base;
                    candidate.params.channel_length = channel_length;
                    candidate.params.stop_value = stop_value;
                    candidate.params.take_profit_ratio = take_profit_ratio;
                    candidate.params.exit_channel_length =
                        exitChannelLength(channel_length, config.exit_channel_ratio);
                    candidates.push_back(candidate);
                }
            }
        }
        return candidates;
    }

    static double calculateScore(Objective objective, const SimulationResult &result, double starting_equity)
    {
        const std::vector<Trade> &trades = result.trades;
        if (trades.empty())
            return 0.0;

        switch (objective)
        {
        case Objective::Return:
            return result.final_equity / starting_equity - 1.0;

        case Objective::WinRate:
        {
            size_t wins = 0;
            for (const Trade &trade : trades)
                wins += trade.net_pnl > 0.0;
            return static_cast<double>(wins) / trades.size();
        }

        case Objective::ProfitFactor:
        {
            double wins = 0.0;
            double losses = 0.0;
            for (const Trade &trade : trades)
                (trade.net_pnl > 0.0 ? wins : losses) += std::fabs(trade.net_pnl);
            // Capped, so that a few trades without any loss do not look better than everything else
            return (losses > 0.0) ? std::min(wins / losses, 10.0) : (wins > 0.0 ? 10.0 : 0.0);
        }

        case Objective::Sharpe:
        {
            if (trades.size() < 2)
                return 0.0;
            double sum = 0.0;
            double sum_squares = 0.0;
            for (const Trade &trade : trades)
            {
                double trade_return = trade.net_pnl / trade.equity_before;
                sum += trade_return;
                sum_squares += trade_return * trade_return;
            }
            double mean = sum / trades.size();
            double variance = (sum_squares - trades.size() * mean * mean) / (trades.size() - 1);
            if (variance <= 0.0)
                return 0.0;
            return mean / std::sqrt(variance) * std::sqrt(static_cast<double>(trades.size()));
        }
        }
        return 0.0;
    }

    static void evaluateCandidates(const MarketData &market, const WalkForwardConfig &config,
                                   std::vector<Candidate> &candidates, size_t begin, size_t end)
    {
        SimulationRange range;
        range.begin = begin;
        range.entry_end = end;
        range.data_end = end; // Nothing after the lookback data is visible to the optimization
        range.max_trades = std::numeric_limits<size_t>::max();

        std::atomic<size_t> next{0};
        auto worker = [&]()
        {
            for (size_t index = next++; index < candidates.size(); index = next++)
            {
                Candidate &candidate = candidates[index];
                SimulationResult result =
                    BreakoutStrategy::simulate(market, candidate.params, config.execution, range, 1.0);
                candidate.score = calculateScore(config.objective, result, 1.0);
                candidate.net_return = result.final_equity - 1.0;
                candidate.trades = result.trades.size();
            }
        };

        size_t thread_count = std::max<size_t>(1, std::thread::hardware_concurrency());
        thread_count = std::min(thread_count, candidates.size());
        if (thread_count <= 1)
        {
            worker();
            return;
        }

        std::vector<std::thread> threads;
        for (size_t t = 0; t < thread_count; ++t)
            threads.emplace_back(worker);
        for (auto &thread : threads)
            thread.join();
    }

    // Returns the chosen candidate or nullptr when it is better not to trade.
    static const Candidate *chooseCandidate(const WalkForwardConfig &config,
                                            const std::vector<Candidate> &candidates)
    {
        const Candidate *best = nullptr;
        for (const Candidate &candidate : candidates)
        {
            if (candidate.trades < config.min_trades)
                continue;
            if (config.require_positive && candidate.net_return <= 0.0)
                continue;
            if (!best || candidate.score > best->score)
                best = &candidate;
        }

        if (!best && !config.require_positive)
        {
            // Not enough trades on the lookback data: use the combination with the best return
            for (const Candidate &candidate : candidates)
            {
                if (!best || candidate.net_return > best->net_return)
                    best = &candidate;
            }
        }
        return best;
    }

    WalkForwardResult run(const MarketData &market, const WalkForwardConfig &config,
                          size_t trade_begin, size_t data_end, double starting_equity)
    {
        const size_t lookback_candles = static_cast<size_t>(config.lookback_days) * MINUTES_PER_DAY;
        const size_t apply_max_candles = static_cast<size_t>(config.apply_max_days) * MINUTES_PER_DAY;
        data_end = std::min(data_end, market.candles.size());

        WalkForwardResult result;
        result.starting_equity = starting_equity;
        result.final_equity = starting_equity;
        result.end_index = data_end;

        std::vector<Candidate> candidates = buildCandidates(config);
        if (candidates.empty() || apply_max_candles == 0)
            return result;

        size_t cursor = std::max(trade_begin, lookback_candles);
        result.begin_index = std::min(cursor, data_end);
        double equity = starting_equity;

        while (cursor < data_end && equity > 0.0)
        {
            WindowRecord window;
            window.optimize_begin = cursor - lookback_candles;
            window.apply_begin = cursor;

            if (candidates.size() > 1 || config.require_positive)
                evaluateCandidates(market, config, candidates, window.optimize_begin, cursor);
            const Candidate *chosen = (candidates.size() > 1 || config.require_positive)
                                          ? chooseCandidate(config, candidates)
                                          : &candidates.front();

            if (!chosen)
            {
                // No combination is good enough, stay out of the market and check again later
                cursor = std::min(cursor + apply_max_candles, data_end);
                window.apply_end = cursor;
                result.windows.push_back(window);
                continue;
            }

            SimulationRange range;
            range.begin = cursor;
            range.entry_end = std::min(cursor + apply_max_candles, data_end);
            range.data_end = data_end;
            range.max_trades = config.apply_trades;
            range.record_stops = true;
            range.close_at_end = !config.live;

            SimulationResult applied =
                BreakoutStrategy::simulate(market, chosen->params, config.execution, range, equity);

            window.traded = true;
            window.channel_length = chosen->params.channel_length;
            window.stop_value = chosen->params.stop_value;
            window.take_profit_ratio = chosen->params.take_profit_ratio;
            window.lookback_score = chosen->score;
            window.lookback_return = chosen->net_return;
            window.lookback_trades = chosen->trades;
            window.applied_trades = applied.trades.size();
            window.applied_return = applied.final_equity / equity - 1.0;

            result.trades.insert(result.trades.end(), applied.trades.begin(), applied.trades.end());
            equity = applied.final_equity;

            // The simulation always moves forward, because entry_end is after the cursor
            cursor = std::max(applied.next_index, cursor + 1);
            window.apply_end = cursor;
            result.windows.push_back(window);
        }

        result.final_equity = equity;
        return result;
    }
}
