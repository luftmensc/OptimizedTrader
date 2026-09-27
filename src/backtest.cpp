#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "BreakoutStrategy.hpp"
#include "MarketData.hpp"
#include "Performance.hpp"
#include "WalkForward.hpp"

struct Options
{
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT"};
    std::string input_dir = "./input";
    std::string output_dir = "./output";
    std::string tag = "backtest";
    std::string start;
    std::string end;
    double capital = 10000.0;
    bool quiet = false;
    bool live = false;
    WalkForwardConfig config;
};

static std::vector<std::string> split(const std::string &text)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ','))
    {
        if (!part.empty())
            parts.push_back(part);
    }
    return parts;
}

static std::vector<int> toInts(const std::string &text)
{
    std::vector<int> values;
    for (const std::string &part : split(text))
        values.push_back(std::stoi(part));
    return values;
}

static std::vector<double> toDoubles(const std::string &text)
{
    std::vector<double> values;
    for (const std::string &part : split(text))
        values.push_back(std::stod(part));
    return values;
}

static void printUsage()
{
    std::cout <<
        "Walk forward backtest of the breakout strategy.\n\n"
        "Data\n"
        "  --symbols A,B          Symbols, input/<SYMBOL>.csv must exist (default BTCUSDT,ETHUSDT,SOLUSDT,BNBUSDT)\n"
        "  --input-dir DIR        (default ./input)\n"
        "  --output-dir DIR       (default ./output)\n"
        "  --start YYYY-MM-DD     First day of trading (default: as early as the lookback allows)\n"
        "  --end YYYY-MM-DD       Data from this day on is not used (default: end of the data)\n"
        "  --tag NAME             Prefix of the output files (default backtest)\n"
        "Strategy\n"
        "  --timeframe N          Minutes of a bar (default 240)\n"
        "  --channels A,B         Searched channel lengths in bars (default 10,20,40,80)\n"
        "  --stop-mode MODE       atr | pct (default atr)\n"
        "  --stops A,B            Searched stop distances: ATR multiples or price ratios (default 2,3,4,6)\n"
        "  --tp-ratios A,B        Searched take profit distances as multiple of the stop, 0 = none (default 0)\n"
        "  --trailing 0|1         Trailing stop (default 1)\n"
        "  --exit-channel R       Exit channel length = R * channel length, 0 = off (default 0)\n"
        "  --trend-ema N          Trend filter ema length in bars, 0 = off (default 200)\n"
        "  --max-hold N           Exit after N bars, 0 = off (default 0)\n"
        "  --cooldown N           Bars to wait after a trade (default 1)\n"
        "  --sides S              both | long | short (default both)\n"
        "Rolling optimization\n"
        "  --lookback-days N      (default 90)\n"
        "  --apply-trades N       (default 10)\n"
        "  --apply-max-days N     (default 30)\n"
        "  --objective O          return | winrate | pf | sharpe (default sharpe)\n"
        "  --min-trades N         (default 5)\n"
        "  --require-positive 0|1 Stay flat when nothing made money on the lookback data (default 0)\n"
        "Execution\n"
        "  --capital X            Split equally between the symbols (default 10000)\n"
        "  --sizing MODE          risk | notional (default risk)\n"
        "  --risk X               Risked ratio of the equity per trade (default 0.01)\n"
        "  --notional X           Position value as ratio of the equity for --sizing notional (default 1)\n"
        "  --max-leverage X       (default 3)\n"
        "  --taker-fee X          (default 0.0005)\n"
        "  --maker-fee X          (default 0.0002)\n"
        "  --slippage X           (default 0.0002)\n"
        "  --funding 0|1          (default 1)\n"
        "  --quiet                Only write the files\n"
        "Paper trading (see scripts/paper_trader.py)\n"
        "  --live 0|1             The end of the data is now: a position at the end stays open, --start is\n"
        "                         used exactly and <tag>_state.json is written (default 0)\n";
}

static bool parseOptions(int argc, char **argv, Options &options)
{
    WalkForwardConfig &config = options.config;

    // Defaults of the strategy
    config.base.timeframe_minutes = 240;
    config.base.stop_mode = StopMode::Atr;
    config.base.trailing_stop = true;
    config.base.cooldown_bars = 1;
    config.base.trend_ema_length = 200;
    config.channel_lengths = {10, 20, 40, 80};
    config.stop_values = {2.0, 3.0, 4.0, 6.0};
    config.take_profit_ratios = {0.0};
    config.lookback_days = 90;
    config.apply_trades = 10;
    config.apply_max_days = 30;
    config.objective = Objective::Sharpe;

    for (int i = 1; i < argc; ++i)
    {
        const std::string name = argv[i];
        if (name == "--help" || name == "-h")
        {
            printUsage();
            return false;
        }
        if (name == "--quiet")
        {
            options.quiet = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            std::cerr << "Error: Missing value of " << name << std::endl;
            return false;
        }
        const std::string value = argv[++i];

        try
        {
            if (name == "--symbols") options.symbols = split(value);
            else if (name == "--input-dir") options.input_dir = value;
            else if (name == "--output-dir") options.output_dir = value;
            else if (name == "--start") options.start = value;
            else if (name == "--end") options.end = value;
            else if (name == "--tag") options.tag = value;
            else if (name == "--capital") options.capital = std::stod(value);
            else if (name == "--live") options.live = config.live = std::stoi(value) != 0;
            else if (name == "--timeframe") config.base.timeframe_minutes = std::stoi(value);
            else if (name == "--channels") config.channel_lengths = toInts(value);
            else if (name == "--stops") config.stop_values = toDoubles(value);
            else if (name == "--tp-ratios") config.take_profit_ratios = toDoubles(value);
            else if (name == "--trailing") config.base.trailing_stop = std::stoi(value) != 0;
            else if (name == "--exit-channel") config.exit_channel_ratio = std::stod(value);
            else if (name == "--trend-ema") config.base.trend_ema_length = std::stoi(value);
            else if (name == "--max-hold") config.base.max_hold_bars = std::stoi(value);
            else if (name == "--cooldown") config.base.cooldown_bars = std::stoi(value);
            else if (name == "--lookback-days") config.lookback_days = std::stoi(value);
            else if (name == "--apply-trades") config.apply_trades = std::stoul(value);
            else if (name == "--apply-max-days") config.apply_max_days = std::stoi(value);
            else if (name == "--min-trades") config.min_trades = std::stoul(value);
            else if (name == "--require-positive") config.require_positive = std::stoi(value) != 0;
            else if (name == "--risk") config.execution.risk_per_trade = std::stod(value);
            else if (name == "--notional") config.execution.notional_fraction = std::stod(value);
            else if (name == "--max-leverage") config.execution.max_leverage = std::stod(value);
            else if (name == "--taker-fee") config.execution.taker_fee = std::stod(value);
            else if (name == "--maker-fee") config.execution.maker_fee = std::stod(value);
            else if (name == "--slippage") config.execution.slippage = std::stod(value);
            else if (name == "--funding") config.execution.apply_funding = std::stoi(value) != 0;
            else if (name == "--stop-mode")
            {
                if (value != "atr" && value != "pct")
                    throw std::invalid_argument(value);
                config.base.stop_mode = (value == "atr") ? StopMode::Atr : StopMode::Percent;
            }
            else if (name == "--sizing")
            {
                if (value != "risk" && value != "notional")
                    throw std::invalid_argument(value);
                config.execution.sizing_mode = (value == "risk") ? SizingMode::Risk : SizingMode::Notional;
            }
            else if (name == "--sides")
            {
                if (value != "both" && value != "long" && value != "short")
                    throw std::invalid_argument(value);
                config.base.allow_long = value != "short";
                config.base.allow_short = value != "long";
            }
            else if (name == "--objective")
            {
                if (!WalkForward::parseObjective(value, config.objective))
                    throw std::invalid_argument(value);
            }
            else
            {
                std::cerr << "Error: Unknown option " << name << " (see --help)" << std::endl;
                return false;
            }
        }
        catch (const std::exception &)
        {
            std::cerr << "Error: Invalid value of " << name << ": " << value << std::endl;
            return false;
        }
    }

    auto allPositive = [](const auto &values)
    {
        return !values.empty() && std::all_of(values.begin(), values.end(), [](auto v) { return v > 0; });
    };
    if (options.symbols.empty() || config.base.timeframe_minutes <= 0 || !allPositive(config.channel_lengths) ||
        !allPositive(config.stop_values) || config.take_profit_ratios.empty() || config.lookback_days <= 0 ||
        config.apply_trades == 0 || config.apply_max_days <= 0 || options.capital <= 0.0)
    {
        std::cerr << "Error: The options are not valid (see --help)" << std::endl;
        return false;
    }
    if (!config.base.trailing_stop && config.exit_channel_ratio <= 0.0 && config.base.max_hold_bars <= 0 &&
        std::any_of(config.take_profit_ratios.begin(), config.take_profit_ratios.end(),
                    [](double ratio) { return ratio <= 0.0; }))
    {
        std::cerr << "Warning: Without take profit, trailing stop, exit channel or max hold, "
                     "a winning position is never closed." << std::endl;
    }
    return true;
}

static nlohmann::json toJson(const PerformanceReport &report)
{
    nlohmann::json monthly = nlohmann::json::object();
    for (const auto &month : report.monthly_returns)
        monthly[month.first] = month.second;

    return {
        {"starting_equity", report.starting_equity},
        {"final_equity", report.final_equity},
        {"days", report.days},
        {"total_return", report.total_return},
        {"cagr", report.cagr},
        {"annual_volatility", report.annual_volatility},
        {"sharpe", report.sharpe},
        {"sortino", report.sortino},
        {"calmar", report.calmar},
        {"max_drawdown", report.max_drawdown},
        {"max_drawdown_days", report.max_drawdown_days},
        {"best_day", report.best_day},
        {"worst_day", report.worst_day},
        {"positive_months", report.positive_months},
        {"monthly_returns", monthly},
        {"trades", report.trades},
        {"wins", report.wins},
        {"long_trades", report.long_trades},
        {"short_trades", report.short_trades},
        {"max_consecutive_losses", report.max_consecutive_losses},
        {"win_rate", report.win_rate},
        {"profit_factor", report.profit_factor},
        {"average_win", report.average_win},
        {"average_loss", report.average_loss},
        {"payoff_ratio", report.payoff_ratio},
        {"expectancy", report.expectancy},
        {"expectancy_r", report.expectancy_r},
        {"average_hold_hours", report.average_hold_hours},
        {"exposure", report.exposure},
        {"gross_pnl", report.gross_pnl},
        {"total_fees", report.total_fees},
        {"total_funding", report.total_funding},
        {"long_pnl", report.long_pnl},
        {"short_pnl", report.short_pnl},
    };
}

static std::string percent(double value, int decimals = 1)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(decimals) << value * 100.0 << "%";
    return stream.str();
}

static std::string number(double value, int decimals = 2)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(decimals) << value;
    return stream.str();
}

static void printTable(const std::vector<std::string> &names, const std::vector<PerformanceReport> &reports,
                       const std::vector<double> &buy_and_hold)
{
    const int width = 12;
    auto row = [&](const std::string &label, auto value)
    {
        std::cout << std::left << std::setw(24) << label << std::right;
        for (size_t i = 0; i < reports.size(); ++i)
            std::cout << std::setw(width) << value(i);
        std::cout << "\n";
    };

    row("", [&](size_t i) { return names[i]; });
    row("Total return", [&](size_t i) { return percent(reports[i].total_return); });
    row("Buy and hold", [&](size_t i) { return percent(buy_and_hold[i]); });
    row("CAGR", [&](size_t i) { return percent(reports[i].cagr); });
    row("Volatility (year)", [&](size_t i) { return percent(reports[i].annual_volatility); });
    row("Sharpe", [&](size_t i) { return number(reports[i].sharpe); });
    row("Sortino", [&](size_t i) { return number(reports[i].sortino); });
    row("Calmar", [&](size_t i) { return number(reports[i].calmar); });
    row("Max drawdown", [&](size_t i) { return percent(reports[i].max_drawdown); });
    row("Longest drawdown days", [&](size_t i) { return number(reports[i].max_drawdown_days, 0); });
    row("Worst day", [&](size_t i) { return percent(reports[i].worst_day, 2); });
    row("Positive months", [&](size_t i) { return percent(reports[i].positive_months, 0); });
    row("Trades", [&](size_t i) { return std::to_string(reports[i].trades); });
    row("Win rate", [&](size_t i) { return percent(reports[i].win_rate); });
    row("Profit factor", [&](size_t i) { return number(reports[i].profit_factor); });
    row("Payoff ratio", [&](size_t i) { return number(reports[i].payoff_ratio); });
    row("Expectancy (R)", [&](size_t i) { return number(reports[i].expectancy_r, 3); });
    row("Average hold hours", [&](size_t i) { return number(reports[i].average_hold_hours, 1); });
    row("Exposure", [&](size_t i) { return percent(reports[i].exposure); });
    row("Max losing streak", [&](size_t i) { return std::to_string(reports[i].max_consecutive_losses); });
    row("Gross profit", [&](size_t i) { return number(reports[i].gross_pnl, 0); });
    row("Fees", [&](size_t i) { return number(-reports[i].total_fees, 0); });
    row("Funding", [&](size_t i) { return number(reports[i].total_funding, 0); });
    row("Long profit", [&](size_t i) { return number(reports[i].long_pnl, 0); });
    row("Short profit", [&](size_t i) { return number(reports[i].short_pnl, 0); });
}

int main(int argc, char **argv)
{
    auto start_clock = std::chrono::high_resolution_clock::now();

    Options options;
    if (!parseOptions(argc, argv, options))
        return 1;
    const WalkForwardConfig &config = options.config;

    // Load the symbols in parallel
    std::vector<MarketData> markets(options.symbols.size());
    std::vector<char> loaded(options.symbols.size(), 0);
    {
        std::vector<std::thread> threads;
        for (size_t s = 0; s < options.symbols.size(); ++s)
        {
            threads.emplace_back([&, s]()
            {
                const std::string &symbol = options.symbols[s];
                if (markets[s].load(symbol, options.input_dir + "/" + symbol + ".csv",
                                    options.input_dir + "/funding/" + symbol + ".csv"))
                {
                    WalkForward::prepareMarket(markets[s], config);
                    loaded[s] = 1;
                }
            });
        }
        for (auto &thread : threads)
            thread.join();
    }
    if (std::any_of(loaded.begin(), loaded.end(), [](char ok) { return !ok; }))
        return 2;

    // All the symbols are traded on the same period
    int64_t first_time = 0;
    int64_t last_time = std::numeric_limits<int64_t>::max();
    for (const MarketData &market : markets)
    {
        first_time = std::max(first_time, market.candles.time.front());
        last_time = std::min(last_time, market.candles.time.back() + 60);
    }

    int64_t trade_start = first_time + static_cast<int64_t>(config.lookback_days) * 86400;
    if (!options.start.empty())
    {
        int64_t requested = TimeUtils::parseUtc(options.start);
        if (requested < 0)
        {
            std::cerr << "Error: Invalid value of --start" << std::endl;
            return 1;
        }
        trade_start = std::max(trade_start, requested);
    }
    if (options.live)
        trade_start = (trade_start + 59) / 60 * 60;
    else
        trade_start = (trade_start + 86399) / 86400 * 86400; // Start of a day

    int64_t trade_end = last_time;
    if (!options.end.empty())
    {
        int64_t requested = TimeUtils::parseUtc(options.end);
        if (requested < 0)
        {
            std::cerr << "Error: Invalid value of --end" << std::endl;
            return 1;
        }
        trade_end = std::min(trade_end, requested);
    }
    // The equity curve ends on a full hour
    const int64_t curve_end = (trade_end - 60) / 3600 * 3600;

    // Paper trading can start with no data after the start
    if (!options.live && curve_end - trade_start < 2 * 86400)
    {
        std::cerr << "Error: Not enough data between the start and the end for the lookback of "
                  << config.lookback_days << " days." << std::endl;
        return 2;
    }

    const double symbol_capital = options.capital / options.symbols.size();
    std::vector<WalkForwardResult> results;
    std::vector<EquityCurve> curves;
    std::vector<EquityCurve> hold_curves;
    std::vector<PerformanceReport> reports;
    std::vector<double> buy_and_hold;
    std::vector<Trade> all_trades;
    double final_equity = 0.0;

    for (const MarketData &market : markets)
    {
        WalkForwardResult result = WalkForward::run(market, config, market.indexOfTime(trade_start),
                                                    market.indexOfTime(trade_end), symbol_capital);
        EquityCurve curve = Performance::buildEquityCurve(market, result, trade_start, curve_end);
        EquityCurve hold = Performance::buildBuyAndHoldCurve(market, symbol_capital, trade_start, curve_end);

        reports.push_back(Performance::analyze(curve, result.trades, symbol_capital, result.final_equity, 1));
        buy_and_hold.push_back(hold.equity.empty() ? 0.0 : hold.equity.back() / hold.equity.front() - 1.0);
        all_trades.insert(all_trades.end(), result.trades.begin(), result.trades.end());
        final_equity += result.final_equity;

        results.push_back(std::move(result));
        curves.push_back(std::move(curve));
        hold_curves.push_back(std::move(hold));
    }

    const EquityCurve portfolio_curve = Performance::sum(curves);
    const EquityCurve portfolio_hold = Performance::sum(hold_curves);
    const PerformanceReport portfolio = Performance::analyze(portfolio_curve, all_trades, options.capital,
                                                             final_equity, markets.size());
    const PerformanceReport hold_report = Performance::analyze(
        portfolio_hold, {}, options.capital, portfolio_hold.equity.empty() ? 0.0 : portfolio_hold.equity.back(), 0);

    // Output files
    std::filesystem::create_directories(options.output_dir);
    const std::string prefix = options.output_dir + "/" + options.tag;

    nlohmann::json summary;
    summary["config"] = {
        {"timeframe_minutes", config.base.timeframe_minutes},
        {"stop_mode", config.base.stop_mode == StopMode::Atr ? "atr" : "pct"},
        {"trailing_stop", config.base.trailing_stop},
        {"trend_ema_length", config.base.trend_ema_length},
        {"exit_channel_ratio", config.exit_channel_ratio},
        {"lookback_days", config.lookback_days},
        {"risk_per_trade", config.execution.risk_per_trade},
    };
    summary["period"] = {{"start", TimeUtils::formatUtc(trade_start)}, {"end", TimeUtils::formatUtc(curve_end)}};
    summary["portfolio"] = toJson(portfolio);
    summary["buy_and_hold"] = toJson(hold_report);
    for (size_t s = 0; s < markets.size(); ++s)
    {
        summary["symbols"][markets[s].symbol] = toJson(reports[s]);
        summary["symbols"][markets[s].symbol]["buy_and_hold_return"] = buy_and_hold[s];
    }
    {
        std::ofstream file(prefix + "_summary.json");
        file << summary.dump(2) << std::endl;
    }

    {
        std::ofstream file(prefix + "_equity.csv");
        file << "Time,Portfolio,BuyAndHold";
        for (const MarketData &market : markets)
            file << "," << market.symbol;
        file << "\n" << std::fixed << std::setprecision(2);
        for (size_t i = 0; i < portfolio_curve.equity.size(); ++i)
        {
            file << TimeUtils::formatUtc(trade_start + static_cast<int64_t>(i) * EquityCurve::STEP_SECONDS)
                 << "," << portfolio_curve.equity[i] << "," << portfolio_hold.equity[i];
            for (const EquityCurve &curve : curves)
                file << "," << curve.equity[i];
            file << "\n";
        }
    }

    {
        std::ofstream file(prefix + "_trades.csv");
        file << "Symbol,Side,Entry time,Exit time,Entry price,Exit price,Quantity,Gross,Fees,Funding,Net,"
                "Return,R,Exit reason,Channel,Stop,TakeProfitRatio,Initial stop price,Take profit price\n";
        file << std::setprecision(10);
        for (size_t s = 0; s < markets.size(); ++s)
        {
            for (const Trade &trade : results[s].trades)
            {
                file << markets[s].symbol << "," << (trade.side > 0 ? "Long" : "Short") << ","
                     << TimeUtils::formatUtc(markets[s].candles.time[trade.entry_index]) << ","
                     << TimeUtils::formatUtc(markets[s].candles.time[trade.exit_index]) << ","
                     << trade.entry_price << "," << trade.exit_price << "," << trade.quantity << ","
                     << trade.gross_pnl << "," << trade.entry_fee + trade.exit_fee << "," << trade.funding << ","
                     << trade.net_pnl << "," << trade.net_pnl / trade.equity_before << ","
                     << trade.net_pnl / (trade.quantity * trade.stop_distance) << ","
                     << BreakoutStrategy::toString(trade.exit_reason) << "," << trade.channel_length << ","
                     << trade.stop_value << "," << trade.take_profit_ratio << ","
                     << trade.initial_stop_price << "," << trade.take_profit_price << "\n";
            }
        }
    }

    {
        // Stop price of every trade over the time, a trailing stop has more than one row for a trade
        std::ofstream file(prefix + "_stops.csv");
        file << "Symbol,Entry time,Time,Stop price\n";
        file << std::setprecision(10);
        for (size_t s = 0; s < markets.size(); ++s)
        {
            const CandleSeries &candles = markets[s].candles;
            for (const Trade &trade : results[s].trades)
            {
                for (const auto &stop : trade.stop_path)
                {
                    // A stop which changes with the last candle of the trade is never used
                    if (stop.first > trade.exit_index)
                        continue;
                    file << markets[s].symbol << "," << TimeUtils::formatUtc(candles.time[trade.entry_index])
                         << "," << TimeUtils::formatUtc(candles.time[stop.first]) << "," << stop.second << "\n";
                }
            }
        }
    }

    {
        std::ofstream file(prefix + "_windows.csv");
        file << "Symbol,Apply start,Apply end,Traded,Channel,Stop,TakeProfitRatio,Lookback score,"
                "Lookback return,Lookback trades,Applied trades,Applied return\n";
        for (size_t s = 0; s < markets.size(); ++s)
        {
            const CandleSeries &candles = markets[s].candles;
            for (const WindowRecord &window : results[s].windows)
            {
                file << markets[s].symbol << "," << TimeUtils::formatUtc(candles.time[window.apply_begin]) << ","
                     << TimeUtils::formatUtc(candles.time[window.apply_end - 1] + 60) << ","
                     << (window.traded ? 1 : 0) << "," << window.channel_length << "," << window.stop_value << ","
                     << window.take_profit_ratio << "," << window.lookback_score << ","
                     << window.lookback_return << "," << window.lookback_trades << ","
                     << window.applied_trades << "," << window.applied_return << "\n";
            }
        }
    }

    if (options.live)
    {
        // What the strategy holds and waits for at the end of the data
        nlohmann::json state;
        state["time"] = TimeUtils::formatUtc(last_time);
        state["start"] = TimeUtils::formatUtc(trade_start);
        state["capital"] = options.capital;
        state["equity"] = final_equity;

        for (size_t s = 0; s < markets.size(); ++s)
        {
            const MarketData &market = markets[s];
            const CandleSeries &candles = market.candles;
            const TimeframeBars &bars = market.bars(config.base.timeframe_minutes);
            nlohmann::json symbol;
            symbol["last_candle"] = TimeUtils::formatUtc(candles.time.back());
            symbol["last_price"] = candles.close.back();
            symbol["equity"] = results[s].final_equity;
            symbol["closed_trades"] = reports[s].trades;
            symbol["position"] = nullptr;
            symbol["parameters"] = nullptr;
            symbol["levels"] = nullptr;

            if (!results[s].trades.empty() && results[s].trades.back().exit_reason == ExitReason::Open)
            {
                const Trade &trade = results[s].trades.back();
                symbol["position"] = {
                    {"side", trade.side > 0 ? "Long" : "Short"},
                    {"entry_time", TimeUtils::formatUtc(candles.time[trade.entry_index])},
                    {"entry_price", trade.entry_price},
                    {"quantity", trade.quantity},
                    {"initial_stop_price", trade.initial_stop_price},
                    {"stop_price", trade.stop_path.empty() ? trade.initial_stop_price : trade.stop_path.back().second},
                    {"take_profit_price", trade.take_profit_price},
                    {"unrealized", trade.net_pnl},
                    {"r", trade.net_pnl / (trade.quantity * trade.stop_distance)},
                };
            }

            // Parameters of the last window and the prices which the close of the next bar has to break.
            // close > ema of the bar is the same as close > ema of the bar before it.
            const WindowRecord *window = nullptr;
            for (const WindowRecord &record : results[s].windows)
            {
                if (record.traded)
                    window = &record;
            }
            if (window)
            {
                symbol["parameters"] = {
                    {"channel", window->channel_length},
                    {"stop", window->stop_value},
                    {"take_profit_ratio", window->take_profit_ratio},
                    {"since", TimeUtils::formatUtc(candles.time[window->apply_begin])},
                };

                const size_t count = bars.close.size();
                const size_t length = static_cast<size_t>(window->channel_length);
                if (count >= length && count > 0 && !std::isnan(bars.atr.back()))
                {
                    double high = *std::max_element(bars.close.end() - length, bars.close.end());
                    double low = *std::min_element(bars.close.end() - length, bars.close.end());
                    nlohmann::json levels = {
                        {"bar_close", TimeUtils::formatUtc(bars.time.back() + 2 * 60 * static_cast<int64_t>(bars.minutes))},
                        {"channel_high", high},
                        {"channel_low", low},
                        {"atr", bars.atr.back()},
                    };
                    if (config.base.trend_ema_length > 0)
                    {
                        const double ema = bars.emas.at(config.base.trend_ema_length).back();
                        levels["ema"] = ema;
                        if (!std::isnan(ema))
                        {
                            high = std::max(high, ema);
                            low = std::min(low, ema);
                        }
                    }
                    levels["long_above"] = config.base.allow_long ? nlohmann::json(high) : nlohmann::json(nullptr);
                    levels["short_below"] = config.base.allow_short ? nlohmann::json(low) : nlohmann::json(nullptr);
                    symbol["levels"] = levels;
                }
            }
            state["symbols"][market.symbol] = symbol;
        }

        // The file is replaced with one step, a reader never sees a half written file
        const std::string state_file = prefix + "_state.json";
        {
            std::ofstream file(state_file + ".tmp");
            file << state.dump(2) << std::endl;
        }
        std::filesystem::rename(state_file + ".tmp", state_file);
    }

    if (!options.quiet)
    {
        std::vector<std::string> names = {"Portfolio"};
        std::vector<PerformanceReport> table = {portfolio};
        std::vector<double> holds = {hold_report.total_return};
        for (size_t s = 0; s < markets.size(); ++s)
        {
            names.push_back(markets[s].symbol);
            table.push_back(reports[s]);
            holds.push_back(buy_and_hold[s]);
        }

        std::cout << "Period: " << TimeUtils::formatUtc(trade_start) << " - " << TimeUtils::formatUtc(curve_end)
                  << " (" << number(portfolio.days, 0) << " days), capital " << number(options.capital, 0)
                  << "\n\n";
        printTable(names, table, holds);

        std::chrono::duration<double> elapsed = std::chrono::high_resolution_clock::now() - start_clock;
        std::cout << "\nFiles: " << prefix << "_summary.json, _equity.csv, _trades.csv, _stops.csv, _windows.csv\n"
                  << "Total time: " << number(elapsed.count(), 1) << "s" << std::endl;
    }
    return 0;
}
