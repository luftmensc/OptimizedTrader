#include "Performance.hpp"

#include <algorithm>
#include <cmath>

namespace Performance
{
    constexpr double DAYS_PER_YEAR = 365.0; // Crypto is traded every day
    constexpr int64_t SECONDS_PER_DAY = 86400;

    EquityCurve buildEquityCurve(const MarketData &market, const WalkForwardResult &result,
                                 int64_t start_time, int64_t end_time)
    {
        EquityCurve curve;
        curve.start_time = start_time;

        const CandleSeries &candles = market.candles;
        const std::vector<Trade> &trades = result.trades;
        size_t next_trade = 0;

        for (int64_t time = start_time; time <= end_time; time += EquityCurve::STEP_SECONDS)
        {
            const size_t index = market.indexOfTime(time);
            if (index >= candles.size())
                break;

            // Trades which were closed before this candle are part of the realized equity
            while (next_trade < trades.size() && trades[next_trade].exit_index < index)
                ++next_trade;

            double equity = result.final_equity;
            if (next_trade < trades.size())
            {
                const Trade &trade = trades[next_trade];
                equity = trade.equity_before;
                if (trade.entry_index <= index)
                {
                    equity += trade.side * trade.quantity * (candles.open[index] - trade.entry_price) -
                              trade.entry_fee;
                }
            }
            curve.equity.push_back(equity);
        }
        return curve;
    }

    EquityCurve buildBuyAndHoldCurve(const MarketData &market, double starting_equity,
                                     int64_t start_time, int64_t end_time)
    {
        EquityCurve curve;
        curve.start_time = start_time;

        const CandleSeries &candles = market.candles;
        const size_t first = market.indexOfTime(start_time);
        if (first >= candles.size())
            return curve;

        for (int64_t time = start_time; time <= end_time; time += EquityCurve::STEP_SECONDS)
        {
            const size_t index = market.indexOfTime(time);
            if (index >= candles.size())
                break;
            curve.equity.push_back(starting_equity * candles.open[index] / candles.open[first]);
        }
        return curve;
    }

    EquityCurve sum(const std::vector<EquityCurve> &curves)
    {
        EquityCurve total;
        if (curves.empty())
            return total;

        total.start_time = curves.front().start_time;
        size_t size = curves.front().equity.size();
        for (const EquityCurve &curve : curves)
            size = std::min(size, curve.equity.size());

        total.equity.assign(size, 0.0);
        for (const EquityCurve &curve : curves)
        {
            for (size_t i = 0; i < size; ++i)
                total.equity[i] += curve.equity[i];
        }
        return total;
    }

    static void analyzeCurve(const EquityCurve &curve, PerformanceReport &report)
    {
        const std::vector<double> &equity = curve.equity;
        if (equity.size() < 2)
            return;

        report.days = static_cast<double>(equity.size() - 1) * EquityCurve::STEP_SECONDS / SECONDS_PER_DAY;
        report.total_return = report.final_equity / report.starting_equity - 1.0;
        if (report.final_equity > 0.0 && report.days > 0.0)
            report.cagr = std::pow(report.final_equity / report.starting_equity, DAYS_PER_YEAR / report.days) - 1.0;
        else
            report.cagr = -1.0;

        // Drawdown on the hourly equity
        double peak = equity.front();
        size_t peak_index = 0;
        size_t longest = 0;
        for (size_t i = 0; i < equity.size(); ++i)
        {
            if (equity[i] >= peak)
            {
                peak = equity[i];
                peak_index = i;
            }
            report.max_drawdown = std::max(report.max_drawdown, 1.0 - equity[i] / peak);
            longest = std::max(longest, i - peak_index);
        }
        report.max_drawdown_days = static_cast<double>(longest) * EquityCurve::STEP_SECONDS / SECONDS_PER_DAY;
        if (report.max_drawdown > 0.0)
            report.calmar = report.cagr / report.max_drawdown;

        // Daily returns: equity at 00:00 UTC of every day
        const size_t steps_per_day = SECONDS_PER_DAY / EquityCurve::STEP_SECONDS;
        std::vector<double> daily_returns;
        std::string current_month;
        double month_start_equity = equity.front();
        double previous = equity.front();

        for (size_t i = 0; i < equity.size(); i += steps_per_day)
        {
            const int64_t time = curve.start_time + static_cast<int64_t>(i) * EquityCurve::STEP_SECONDS;
            // The equity at 00:00 is the result of the previous day
            const std::string month = TimeUtils::formatUtc(time - 1).substr(0, 7);

            if (i > 0)
            {
                daily_returns.push_back(previous > 0.0 ? equity[i] / previous - 1.0 : 0.0);
                if (month != current_month && !current_month.empty())
                {
                    report.monthly_returns.emplace_back(current_month, previous / month_start_equity - 1.0);
                    month_start_equity = previous;
                }
                current_month = month;
            }
            previous = equity[i];
        }
        if (!current_month.empty() && month_start_equity > 0.0)
            report.monthly_returns.emplace_back(current_month, previous / month_start_equity - 1.0);

        if (daily_returns.size() < 2)
            return;

        double sum = 0.0;
        double downside_squares = 0.0;
        report.best_day = daily_returns.front();
        report.worst_day = daily_returns.front();
        for (double daily_return : daily_returns)
        {
            sum += daily_return;
            if (daily_return < 0.0)
                downside_squares += daily_return * daily_return;
            report.best_day = std::max(report.best_day, daily_return);
            report.worst_day = std::min(report.worst_day, daily_return);
        }
        const double mean = sum / daily_returns.size();

        double squares = 0.0;
        for (double daily_return : daily_returns)
            squares += (daily_return - mean) * (daily_return - mean);
        const double deviation = std::sqrt(squares / (daily_returns.size() - 1));
        // Downside deviation with 0 as the minimum acceptable return
        const double downside_deviation = std::sqrt(downside_squares / daily_returns.size());

        report.annual_volatility = deviation * std::sqrt(DAYS_PER_YEAR);
        if (deviation > 0.0)
            report.sharpe = mean / deviation * std::sqrt(DAYS_PER_YEAR);
        if (downside_deviation > 0.0)
            report.sortino = mean / downside_deviation * std::sqrt(DAYS_PER_YEAR);

        size_t positive = 0;
        for (const auto &month : report.monthly_returns)
            positive += month.second > 0.0;
        if (!report.monthly_returns.empty())
            report.positive_months = static_cast<double>(positive) / report.monthly_returns.size();
    }

    static void analyzeTrades(const std::vector<Trade> &trades, size_t symbol_count, PerformanceReport &report)
    {
        // Order of closing, so that the losing streak is correct for more than one symbol.
        // Open positions are part of the equity but they are not counted as trades.
        std::vector<const Trade *> ordered;
        for (const Trade &trade : trades)
        {
            if (trade.exit_reason != ExitReason::Open)
                ordered.push_back(&trade);
        }
        report.trades = ordered.size();
        if (ordered.empty())
            return;

        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const Trade *a, const Trade *b) { return a->exit_index < b->exit_index; });

        double win_sum = 0.0;
        double loss_sum = 0.0;
        double win_returns = 0.0;
        double loss_returns = 0.0;
        double return_sum = 0.0;
        double r_sum = 0.0;
        double held_candles = 0.0;
        size_t losing_streak = 0;

        for (const Trade *trade : ordered)
        {
            const double trade_return = trade->net_pnl / trade->equity_before;
            return_sum += trade_return;
            r_sum += trade->net_pnl / (trade->quantity * trade->stop_distance);
            held_candles += static_cast<double>(trade->exit_index - trade->entry_index + 1);

            report.gross_pnl += trade->gross_pnl;
            report.total_fees += trade->entry_fee + trade->exit_fee;
            report.total_funding += trade->funding;

            if (trade->side > 0)
            {
                ++report.long_trades;
                report.long_pnl += trade->net_pnl;
            }
            else
            {
                ++report.short_trades;
                report.short_pnl += trade->net_pnl;
            }

            if (trade->net_pnl > 0.0)
            {
                ++report.wins;
                win_sum += trade->net_pnl;
                win_returns += trade_return;
                losing_streak = 0;
            }
            else
            {
                loss_sum -= trade->net_pnl;
                loss_returns += trade_return;
                ++losing_streak;
                report.max_consecutive_losses = std::max(report.max_consecutive_losses, losing_streak);
            }
        }

        const size_t losses = report.trades - report.wins;
        report.win_rate = static_cast<double>(report.wins) / report.trades;
        report.profit_factor = (loss_sum > 0.0) ? win_sum / loss_sum : 0.0;
        report.average_win = report.wins > 0 ? win_returns / report.wins : 0.0;
        report.average_loss = losses > 0 ? loss_returns / losses : 0.0;
        report.payoff_ratio = (report.average_loss < 0.0) ? report.average_win / -report.average_loss : 0.0;
        report.expectancy = return_sum / report.trades;
        report.expectancy_r = r_sum / report.trades;
        report.average_hold_hours = held_candles / report.trades / 60.0;
        if (report.days > 0.0 && symbol_count > 0)
            report.exposure = held_candles / (report.days * 24.0 * 60.0 * symbol_count);
    }

    PerformanceReport analyze(const EquityCurve &curve, const std::vector<Trade> &trades,
                              double starting_equity, double final_equity, size_t symbol_count)
    {
        PerformanceReport report;
        report.starting_equity = starting_equity;
        report.final_equity = final_equity;
        analyzeCurve(curve, report);
        analyzeTrades(trades, symbol_count, report);
        return report;
    }
}
