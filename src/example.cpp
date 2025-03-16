#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <csignal>
#include <atomic>
#include "DataFetch.hpp"
#include "RealTimeNM.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cpprest/http_client.h> // For HTTP client functionalities
#include <cpprest/uri_builder.h> // For URI builder
#include <cpprest/json.h>        // For JSON handling, if needed

#include "FibAlgoTrader.hpp"
#include "OrderManager.hpp"

std::mutex log_mutex;
std::mutex cout_mutex;

// Thread-safe logger
class Logger {
public:
    explicit Logger(const std::string& filename) : log_file_(filename) {}

    void log(const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream log_stream(log_file_, std::ios_base::app);
        if (log_stream.is_open()) {
            log_stream << message << std::endl;
        }
        log_stream.close();
    }

private:
    std::string log_file_;
    std::mutex log_mutex_;
};
// Function to format a float to 2 decimal places as a string
std::string formatFloat(float value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(5) << value;
    return stream.str();
}

std::string format_float(float value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

struct CoinSettings
{
    std::string coin_name;
    float multiplier;
    float position_size;
    int wait_candles_after_trade;
    int recalc_trade_interval;
    int recalc_lookback_days;
    int loss_streak_limit;
};

// Forward declarations of helper functions
static void initializeStateAndLogging(const CoinSettings& settings, FibAlgoTrader &trader, std::string &csv_file_to_be_saved, 
                                      TradingState &state, Logger &logger, int &best_sensitivity, float &best_tpsl, 
                                      float &initial_position_size, float &current_position_size, 
                                      int &wins, int &losses, int &total_trades, int &consecutive_losses, 
                                      int &trade_count_since_last_recalc, std::string &state_log_file, ResultHighBroke &best_result);

static void handlePositionClosed(OrderManager &order_manager, TradingState &state, const CoinSettings& settings,
                                 Logger &logger, int &wins, int &losses, int &total_trades, int &trade_count_since_last_recalc,
                                 float &current_position_size, int &consecutive_losses, int best_sensitivity, float best_tpsl,
                                 ResultHighBroke &best_result, const std::string &csv_file_to_be_saved, FibAlgoTrader &trader,
                                 const double current_price, const std::string &state_log_file);

static void recalculateBestParameters(FibAlgoTrader &trader, const std::string &csv_file_to_be_saved, ResultHighBroke &best_result, int lookback_days);

static void recalculateBestParameters(FibAlgoTrader &trader, std::string &csv_file_to_be_saved, ResultHighBroke &best_result, int lookback_days, Logger &logger);

static void handlePositionOpen(Logger &logger, const TradingState &state, double current_price, uint64_t &print_counter);

static void handleNoPositionScenario(OrderManager &order_manager, FibAlgoTrader &trader, const CoinSettings& settings,
                                     Logger &logger, TradingState &state, int &wait_candles_remaining, int best_sensitivity,
                                     float best_tpsl, float &current_position_size, int &total_trades, int &wins, int &losses,
                                     const std::string &csv_file_to_be_saved, const std::string &state_log_file);

static void logPositionState(const std::string &state_log_file, const TradingState &state, double current_price,
                             int best_sensitivity, float best_tpsl, int total_trades, int wins, int losses,
                             double high = 0, double low = 0);

static void recalcParametersIfNeeded(FibAlgoTrader &trader, const std::string &csv_file_to_be_saved, const CoinSettings& settings,
                                     Logger &logger, int &trade_count_since_last_recalc, int &best_sensitivity, float &best_tpsl,
                                     ResultHighBroke &best_result);

// ------------------------ HELPER FUNCTION DEFINITIONS ------------------------

static void initializeStateAndLogging(const CoinSettings& settings, FibAlgoTrader &trader, std::string &csv_file_to_be_saved, 
                                      TradingState &state, Logger &logger, int &best_sensitivity, float &best_tpsl, 
                                      float &initial_position_size, float &current_position_size, 
                                      int &wins, int &losses, int &total_trades, int &consecutive_losses, 
                                      int &trade_count_since_last_recalc, std::string &state_log_file, ResultHighBroke &best_result)
{
    logger.log("Starting RealTimeTradeEngineWatcher for " + settings.coin_name);
    auto data = trader.readCSV(csv_file_to_be_saved);
    size_t current_index = data.size() - 1; // Start from the latest candle

    logger.log("Calculating best parameters for the first time for: " + settings.coin_name);
    recalculateBestParameters(trader, csv_file_to_be_saved, best_result, settings.recalc_lookback_days, logger);
    // Hard-coded values for now:
    best_sensitivity = best_result.best_sensitivity;
    best_tpsl = best_result.best_tpsl;
    logger.log("Founded -> Best sensitivity: " + std::to_string(best_sensitivity) + " Best TP/SL: " + std::to_string(best_tpsl));

    state_log_file = settings.coin_name + "_trading_state_log.csv";
    {
        std::ofstream log_file(state_log_file, std::ios_base::trunc);
        log_file << "Time,High,Low,Close,Entry Price,TP Price,SL Price,Balance,Position Size,Position Size in Dollars,Position Type,Best Sensitivity,Best TP/SL,Total Trades,Wins,Losses\n";
    }
}

static void handlePositionClosed(OrderManager &order_manager, TradingState &state, const CoinSettings& settings,
                                 Logger &logger, int &wins, int &losses, int &total_trades, int &trade_count_since_last_recalc,
                                 float &current_position_size, int &consecutive_losses, int best_sensitivity, float best_tpsl,
                                 ResultHighBroke &best_result, const std::string &csv_file_to_be_saved, FibAlgoTrader &trader,
                                 const double current_price, const std::string &state_log_file)
{
    logger.log("Position closed by external order for " + settings.coin_name);
    state.in_position = false;

    int wait_candles_remaining = settings.wait_candles_after_trade; // handled outside if needed
    order_manager.cancelPendingOrders(settings.coin_name);

    bool win_condition_long = (state.position_type == "Long" && state.entry_price < current_price);
    bool win_condition_short = (state.position_type == "Short" && state.entry_price > current_price);

    if (win_condition_long || win_condition_short) {
        wins++;
        total_trades++;
        trade_count_since_last_recalc++;
        current_position_size = settings.position_size; // reset position size
        consecutive_losses = 0;
        double profit = (state.position_type == "Long") ? 
                        state.position_size_qty * (current_price - state.entry_price) :
                        state.position_size_qty * (state.entry_price - current_price);
        state.balance += profit;
        logger.log("TP Hit " + state.position_type[0] + settings.coin_name);
    } else {
        losses++;
        total_trades++;
        trade_count_since_last_recalc++;
        consecutive_losses++;
        double loss_amount = (state.position_type == "Long") ? 
                             state.position_size_qty * (state.entry_price - current_price) :
                             state.position_size_qty * (current_price - state.entry_price);
        state.balance -= loss_amount;
        if (consecutive_losses < settings.loss_streak_limit) {
            current_position_size *= settings.multiplier;
        }
    }

    // Recalculate parameters if needed
    recalcParametersIfNeeded(trader, csv_file_to_be_saved, settings, logger, trade_count_since_last_recalc, 
                             best_sensitivity, best_tpsl, best_result);

    logPositionState(state_log_file, state, current_price, best_sensitivity, best_tpsl, total_trades, wins, losses);
}

static void handlePositionOpen(Logger &logger, const TradingState &state, double current_price, uint64_t &print_counter)
{
    if (print_counter % 10 == 0) {
        logger.log("Current Price: " + std::to_string(current_price) + 
                   " Entry Price: " + std::to_string(state.entry_price) + 
                   " TP Price: " + std::to_string(state.tp_price) + 
                   " SL Price: " + std::to_string(state.sl_price) + 
                   " Difference%: " + std::to_string((current_price - state.entry_price) / state.entry_price * 100.0f));
    }
    print_counter++;
    // Sleep before next price check
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

static void handleNoPositionScenario(OrderManager &order_manager, FibAlgoTrader &trader, const CoinSettings& settings,
                                     Logger &logger, TradingState &state, int &wait_candles_remaining, int best_sensitivity,
                                     float best_tpsl, float &current_position_size, int &total_trades, int &wins, int &losses,
                                     const std::string &csv_file_to_be_saved, const std::string &state_log_file)
{
    // Not in position: Wait until next minute to fetch candle data and check for new signals
    auto now = std::chrono::system_clock::now();
    auto next_minute = std::chrono::time_point_cast<std::chrono::minutes>(now) + std::chrono::minutes(1);
    auto sleep_duration = next_minute - now;

    // Sleep until the start of the next minute
    std::this_thread::sleep_for(sleep_duration);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // Fetch new candle data
    logger.log("Fetching new candle data for " + settings.coin_name);
    nmDataFetcher::fetch_and_save_candles(settings.coin_name, "1m", 1, true, csv_file_to_be_saved);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto new_data = trader.readCSV(csv_file_to_be_saved);
    size_t new_index = new_data.size() - 1;

    // Calculate high/low for sensitivity period
    float high = new_data[new_index - best_sensitivity].close;
    float low = new_data[new_index - best_sensitivity].close;

    for (size_t j = new_index - best_sensitivity; j < new_index; ++j) {
        high = std::max(high, new_data[j].close);
        low = std::min(low, new_data[j].close);
    }

    if (wait_candles_remaining > 0) {
        logger.log("Waiting for " + std::to_string(wait_candles_remaining) + " more candles before looking for new signals for " + settings.coin_name);
        wait_candles_remaining--;
    } else {
        bool closed_above_high = new_data[new_index].close > high;
        bool closed_below_low = new_data[new_index].close < low;

        bool longCondition = closed_above_high;
        bool shortCondition = closed_below_low;

        if (longCondition) {
            state.position_type = "Long";
            state.entry_price = new_data[new_index].close;
            state.tp_price = state.entry_price * (1.0 + best_tpsl);
            state.sl_price = state.entry_price * (1.0 - best_tpsl);
            state.position_size = current_position_size;
            state.position_size_qty = state.position_size / state.entry_price; 
            logger.log("Long Condition for " + settings.coin_name + " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));

            if (order_manager.marketOrder(settings.coin_name, "long", state.position_size_qty, state.sl_price, state.tp_price)) {
                logger.log("Buy order sent for " + settings.coin_name + " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));
                state.in_position = true;
            } else {
                logger.log("Failed to send buy order for " + settings.coin_name);
            }
        } else if (shortCondition) {
            state.position_type = "Short";
            state.entry_price = new_data[new_index].close;
            state.tp_price = state.entry_price * (1.0 - best_tpsl);
            state.sl_price = state.entry_price * (1.0 + best_tpsl);
            state.position_size_qty = current_position_size / state.entry_price; 
            logger.log("Short Condition for " + settings.coin_name + " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));

            if (order_manager.marketOrder(settings.coin_name, "short", state.position_size_qty, state.sl_price, state.tp_price)) {
                logger.log("Sell order sent for " + settings.coin_name + " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));
                state.in_position = true;
            } else {
                logger.log("Failed to send sell order for " + settings.coin_name);
            }
        }

        if (state.in_position) {
            float position_size_in_dollars = state.position_size_qty * new_data[new_index].close;
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                std::ofstream log_file(state_log_file, std::ios_base::app);
                log_file << new_data[new_index].open_time << ","         // time
                         << high << ","                                  // high
                         << low << ","                                   // low
                         << new_data[new_index].close << ","             // close
                         << state.entry_price << ","                     // entry
                         << state.tp_price << ","                        // tp
                         << state.sl_price << ","                        // sl
                         << state.balance << ","                         // balance
                         << state.position_size_qty << ","               // position size qty
                         << position_size_in_dollars << ","              // position size in dollars
                         << state.position_type << ","                   // position type
                         << best_sensitivity << ","                      // best sensitivity
                         << best_tpsl << ","                             // best tpsl
                         << total_trades << ","                          // total trades
                         << wins << ","                                  // wins
                         << losses << ","                                // losses
                         << "\n";
            }
        }
    }

    // Sleep for a short time before next check (e.g., 1 second)
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

static void logPositionState(const std::string &state_log_file, const TradingState &state, double current_price,
                             int best_sensitivity, float best_tpsl, int total_trades, int wins, int losses,
                             double high, double low)
{
    float position_size_in_dollars = 0;
    std::lock_guard<std::mutex> lock(log_mutex);
    std::ofstream log_file(state_log_file, std::ios_base::app);
    log_file << std::time(nullptr) << "," // time
             << high << ","               // high
             << low << ","                // low
             << current_price << ","      // latest price
             << state.entry_price << ","  // entry price
             << state.tp_price << ","     // tp
             << state.sl_price << ","     // sl
             << state.balance << ","      // balance
             << state.position_size_qty << "," // position size qty
             << position_size_in_dollars << ","// position size in dollars
             << "None" << ","             // position type is now none
             << best_sensitivity << ","   // sensitivity
             << best_tpsl << ","          // tpsl
             << total_trades << ","       // total trades
             << wins << ","               // wins
             << losses << "\n";           // losses
}
void recalculateBestParameters(FibAlgoTrader &trader, const std::string &csv_file_to_be_saved, ResultHighBroke &best_result, int lookback_days)
{
    std::vector<int> sensitivity_values = {100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000};
    std::vector<float> tpsl_values = {0.01f, 0.011f, 0.012f, 0.013f, 0.014f, 0.015f, 0.016f, 0.017f, 0.018f, 0.019f, 0.020f};

    int day_size = 24 * 60;                  // 1 day (in minutes)
    int initial_data_number = lookback_days * day_size + sensitivity_values.back(); // Lookback days + max sensitivity

    auto data = trader.readCSV(csv_file_to_be_saved);

    if (data.size() < static_cast<size_t>(initial_data_number))
    {
        std::cerr << "Not enough data to recalculate parameters (need 30 days of minute data)." << std::endl;
        return;
    }

    auto lookback_data = std::vector<DataRow>(data.end() - initial_data_number, data.end());
    trader.bestDuoParameterFinder(lookback_data, sensitivity_values, tpsl_values, best_result, 0, 99999, 1000.0f);

    std::cout << "Best sensitivity: " << best_result.best_sensitivity
              << " Best TP/SL: " << best_result.best_tpsl
              << " Best balance: " << best_result.best_balance
              << " Coin csv file: " << csv_file_to_be_saved
              << std::endl;
}

void recalculateBestParameters(FibAlgoTrader &trader, std::string &csv_file_to_be_saved, ResultHighBroke &best_result, int lookback_days, Logger &logger)

{
    std::vector<int> sensitivity_values = {100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000};
    std::vector<float> tpsl_values = {0.01f, 0.011f, 0.012f, 0.013f, 0.014f, 0.015f, 0.016f, 0.017f, 0.018f, 0.019f, 0.020f};

    int day_size = 24 * 60;                  // 1 day (in minutes)
    int initial_data_number = lookback_days * day_size + sensitivity_values.back(); // Lookback days + max sensitivity

    auto data = trader.readCSV(csv_file_to_be_saved);

    if (data.size() < static_cast<size_t>(initial_data_number))
    {
        std::cerr << "Not enough data to recalculate parameters (need 30 days of minute data)." << std::endl;
        return;
    }

    auto lookback_data = std::vector<DataRow>(data.end() - initial_data_number, data.end());
    trader.bestDuoParameterFinder(lookback_data, sensitivity_values, tpsl_values, best_result, 0, 99999, 1000.0f);

    logger.log("Best sensitivity: " + std::to_string(best_result.best_sensitivity) +
               " Best TP/SL: " + formatFloat(best_result.best_tpsl) +
               " Best balance: " + formatFloat(best_result.best_balance) +
               " Coin csv file: " + csv_file_to_be_saved);
}


static void recalcParametersIfNeeded(FibAlgoTrader &trader, const std::string &csv_file_to_be_saved, const CoinSettings& settings,
                                     Logger &logger, int &trade_count_since_last_recalc, int &best_sensitivity, float &best_tpsl,
                                     ResultHighBroke &best_result)
{
    if (trade_count_since_last_recalc >= settings.recalc_trade_interval) {
        logger.log("Recalculating best parameters for " + settings.coin_name);
        recalculateBestParameters(trader, csv_file_to_be_saved, best_result, settings.recalc_lookback_days);
        // best_sensitivity = 2; // Hard-coded for now
        // best_tpsl = 0.0015;   // Hard-coded for now
        logger.log("Founded -> Best sensitivity: " + std::to_string(best_result.best_sensitivity) +
                   " Best TP/SL: " + std::to_string(best_result.best_tpsl));
        trade_count_since_last_recalc = 0;
    }
}




// Function to handle signal
std::atomic<bool> running(true);
void signalHandler(int signum)
{
    std::cout << "Interrupt signal (" << signum << ") received.\n";
    running = false;
}


void RealTimeTradeEngineWatcher(std::string csv_file_to_be_saved, FibAlgoTrader &trader, const CoinSettings& settings)
{
    // Initial setup
    OrderManager order_manager;
    Logger logger(settings.coin_name + "_real_time_log.txt");
    logger.log("Starting RealTimeTradeEngineWatcher for " + settings.coin_name);
    auto data = trader.readCSV(csv_file_to_be_saved);
    size_t current_index = data.size() - 1; // Start from the latest candle
    TradingState state;
    int wait_candles_remaining = 0;

    int trade_count_since_last_recalc = 0; // Counter for recalculating parameters

    // Counters for win, loss, and total trades
    int wins = 0;
    int losses = 0;
    int total_trades = 0;

    int best_sensitivity = 0;
    float best_tpsl = 0.0f;
    float initial_position_size = settings.position_size;
    float current_position_size = settings.position_size;

    int consecutive_losses = 0; // New counter for consecutive losses

    logger.log("Calculating best parameters for the first time for: " + settings.coin_name);
    ResultHighBroke best_result;
    //recalculateBestParameters(trader, csv_file_to_be_saved, best_result, settings.recalc_lookback_days, logger);
    best_sensitivity = 2;
    best_tpsl = 0.0011;
    logger.log("HACKED Founded -> Best sensitivity: " + std::to_string(best_sensitivity) + " Best TP/SL: " + std::to_string(best_tpsl));
    std::string state_log_file = settings.coin_name + "_trading_state_log.csv";
    {
        std::ofstream log_file(state_log_file, std::ios_base::trunc);
        log_file << "Time,High,Low,Close,Entry Price,TP Price,SL Price,Balance,Position Size,Position Size in Dollars,Position Type,Best Sensitivity,Best TP/SL,Total Trades,Wins,Losses" << "\n";
        log_file.close();
    } 

    uint64_t print_counter=0;

    while (running)
    {
        auto [status, side] = order_manager.getPositionStatus(settings.coin_name);
        double current_price = nmDataFetcher::fetchCurrentPrice(settings.coin_name);
        if (state.in_position && status == "closed")
        {
            logger.log("Position closed by external order for " + settings.coin_name);
            state.in_position = false;
            wait_candles_remaining = settings.wait_candles_after_trade; // Set wait period
            order_manager.cancelPendingOrders(settings.coin_name);

            if(state.position_type == "Long" && (state.entry_price<current_price))
            {
                wins++;
                total_trades++;
                trade_count_since_last_recalc++;
                current_position_size = initial_position_size;
                consecutive_losses = 0;
                state.balance += state.position_size_qty * (current_price - state.entry_price);
                logger.log("TP Hit L" + settings.coin_name);
                
            }
            else if(state.position_type == "Short" && (state.entry_price>current_price))
            {
                wins++;
                total_trades++;
                trade_count_since_last_recalc++;
                current_position_size = initial_position_size;
                consecutive_losses = 0;
                state.balance += state.position_size_qty * (state.entry_price - current_price);
                logger.log("TP Hit S" + settings.coin_name);
            }
            else
            {
                losses++;
                total_trades++;
                trade_count_since_last_recalc++;
                consecutive_losses++;
                if (state.position_type == "Long") {
                    state.balance -= state.position_size_qty * (state.entry_price - current_price);
                } else {
                    state.balance -= state.position_size_qty * (current_price - state.entry_price);
                }
                if(consecutive_losses < settings.loss_streak_limit)
                {
                    current_position_size *= settings.multiplier;
                }
            }

            // Recalculate parameters after the specified number of trades
            if (trade_count_since_last_recalc >= settings.recalc_trade_interval)
            {
                logger.log("Recalculating best parameters for " + settings.coin_name);
                recalculateBestParameters(trader, csv_file_to_be_saved, best_result, settings.recalc_lookback_days);
                best_sensitivity = 2;
                best_tpsl = 0.0015;
                logger.log("HACKED AGAIN Founded -> Best sensitivity: " + std::to_string(best_result.best_sensitivity) + " Best TP/SL: " + std::to_string(best_result.best_tpsl));
                trade_count_since_last_recalc = 0;               // Reset trade count
            }
            float position_size_in_dollars = 0;

            {
                std::lock_guard<std::mutex> lock(log_mutex);
                std::ofstream log_file(state_log_file, std::ios_base::app);
                log_file << std::time(nullptr) << ","                    // Log the time
                            << 0 << ","                                       // High (not applicable here)
                            << 0 << ","                                       // Low (not applicable here)
                            << current_price << ","                           // Latest price
                            << state.entry_price << ","                       // Entry price
                            << state.tp_price << ","                          // TP price
                            << state.sl_price << ","                          // SL price
                            << state.balance << ","                           // Current balance
                            << state.position_size_qty << ","                     // Position size
                            << position_size_in_dollars << ","                // Position size in dollars
                            << "None" << ","                                  // Position type
                            << best_sensitivity << ","                        // Current sensitivity
                            << best_tpsl << ","                               // Current TP/SL
                            << total_trades << ","                            // Total trades
                            << wins << ","                                    // Wins
                            << losses << ","                                  // Losses
                            << "\n";

                log_file.close();
            }
        }
        else if (status == "open")
        {
            if(print_counter%10==0)
            {
                logger.log("Current Price: " + std::to_string(current_price) + 
                            " Entry Price: " + std::to_string(state.entry_price) + 
                            " TP Price: " + std::to_string(state.tp_price) + 
                            " SL Price: " + std::to_string(state.sl_price) + 
                            " Difference%: " + std::to_string((current_price - state.entry_price) / state.entry_price * 100.0f)
                            );
            }
            print_counter++;
            // Sleep before next price check
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        }
        else
        {
            // Not in position: Wait until next minute to fetch candle data and check for new signals
            auto now = std::chrono::system_clock::now();
            auto next_minute = std::chrono::time_point_cast<std::chrono::minutes>(now) + std::chrono::minutes(1);
            auto sleep_duration = next_minute - now;

            // Sleep until the start of the next minute
            std::this_thread::sleep_for(sleep_duration);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            // Fetch new candle data
            logger.log("Fetching new candle data for " + settings.coin_name);
            nmDataFetcher::fetch_and_save_candles(settings.coin_name, "1m", 1, true, csv_file_to_be_saved);
            std::this_thread::sleep_for(std::chrono::seconds(1));

            auto new_data = trader.readCSV(csv_file_to_be_saved);
            size_t new_index = new_data.size() - 1;

            // Calculate high and low for the last 'sensitivity' period
            float high = new_data[new_index - best_sensitivity].close;
            float low = new_data[new_index - best_sensitivity].close;

            for (size_t j = new_index - best_sensitivity; j < new_index; ++j)
            {
                high = std::max(high, new_data[j].close);
                low = std::min(low, new_data[j].close);
            }

            if (wait_candles_remaining > 0)
            {
                logger.log("Waiting for " + std::to_string(wait_candles_remaining) + " more candles before looking for new signals for " + settings.coin_name);
                wait_candles_remaining--;
            }
            else
            {
                bool closed_above_high = new_data[new_index].close > high;
                bool closed_below_low = new_data[new_index].close < low;

                bool longCondition = closed_above_high;
                bool shortCondition = closed_below_low;

                if (longCondition)
                {
                    state.position_type = "Long";
                    state.entry_price = new_data[new_index].close;
                    state.tp_price = state.entry_price * (1.0 + best_tpsl);
                    state.sl_price = state.entry_price * (1.0 - best_tpsl);
                    state.position_size = current_position_size;
                    state.position_size_qty = state.position_size / state.entry_price; // Use passed position size
                    logger.log("Long Condition for " + settings.coin_name+ " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));
                     
                    if(order_manager.marketOrder(settings.coin_name, "long", state.position_size_qty, state.sl_price, state.tp_price))
                    {
                        logger.log("Buy order sent for " + settings.coin_name + " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));
                        state.in_position = true;
                    }
                    else
                    {
                        logger.log("Failed to send buy order for " + settings.coin_name);
                    }
                }
                else if (shortCondition)
                {
                    state.position_type = "Short";
                    state.entry_price = new_data[new_index].close;
                    state.tp_price = state.entry_price * (1.0 - best_tpsl);
                    state.sl_price = state.entry_price * (1.0 + best_tpsl);
                    state.position_size_qty = current_position_size / state.entry_price; // Use passed position size
                    logger.log("Short Condition for " + settings.coin_name+ " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));
                    if(order_manager.marketOrder(settings.coin_name, "short", state.position_size_qty, state.sl_price, state.tp_price))
                    {
                        logger.log("Sell order sent for " + settings.coin_name + " with TP/SL: " + formatFloat(state.tp_price) + "/" + formatFloat(state.sl_price));
                        state.in_position = true;
                    }
                    else
                    {
                        logger.log("Failed to send sell order for " + settings.coin_name);
                    }
                }

                if (state.in_position)
                {
                    // Log the new position
                    float position_size_in_dollars = state.position_size_qty * new_data[new_index].close;

                    {
                        std::lock_guard<std::mutex> lock(log_mutex);
                        std::ofstream log_file(state_log_file, std::ios_base::app);
                        log_file << new_data[new_index].open_time << ","              // Log the time
                                 << high << ","                                       // Lookback high
                                 << low << ","                                        // Lookback low
                                 << new_data[new_index].close << ","                  // Latest close value
                                 << state.entry_price << ","                          // Entry price
                                 << state.tp_price << ","                             // TP price
                                 << state.sl_price << ","                             // SL price
                                 << state.balance << ","                              // Current balance
                                 << state.position_size_qty << ","                        // Position size
                                 << position_size_in_dollars << ","                   // Position size in dollars
                                 << state.position_type << ","                        // Position type
                                 << best_sensitivity << ","                           // Current sensitivity
                                 << best_tpsl << ","                                  // Current TP/SL
                                 << total_trades << ","                               // Total trades
                                 << wins << ","                                       // Wins
                                 << losses << ","                                     // Losses
                                 << "\n";
                    }
                }
            }

            // Sleep for a short time before next check (e.g., 1 second)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    logger.log("Real-time trading terminated for " + settings.coin_name);
}

void RealTimeTradeEngineWatcher2(std::string csv_file_to_be_saved, FibAlgoTrader &trader, const CoinSettings& settings)
{
    OrderManager order_manager;
    Logger logger(settings.coin_name + "_real_time_log.txt");

    TradingState state;
    int wait_candles_remaining = 0;
    int trade_count_since_last_recalc = 0;
    int wins = 0, losses = 0, total_trades = 0;
    int consecutive_losses = 0;
    int best_sensitivity = 0;
    float best_tpsl = 0.0f;
    float initial_position_size = settings.position_size;
    float current_position_size = settings.position_size;

    ResultHighBroke best_result;
    std::string state_log_file;

    initializeStateAndLogging(settings, trader, csv_file_to_be_saved, state, logger, best_sensitivity, best_tpsl, 
                              initial_position_size, current_position_size, wins, losses, total_trades, consecutive_losses,
                              trade_count_since_last_recalc, state_log_file, best_result);

    uint64_t print_counter = 0;

    while (running)
    {
        auto [status, side] = order_manager.getPositionStatus(settings.coin_name);
        double current_price = nmDataFetcher::fetchCurrentPrice(settings.coin_name);

        if (state.in_position && status == "closed") {
            handlePositionClosed(order_manager, state, settings, logger, wins, losses, total_trades, 
                                 trade_count_since_last_recalc, current_position_size, consecutive_losses,
                                 best_sensitivity, best_tpsl, best_result, csv_file_to_be_saved, trader, current_price, state_log_file);
        } 
        else if (status == "open") {
            handlePositionOpen(logger, state, current_price, print_counter);
        } 
        else {
            handleNoPositionScenario(order_manager, trader, settings, logger, state, wait_candles_remaining, best_sensitivity, 
                                     best_tpsl, current_position_size, total_trades, wins, losses, csv_file_to_be_saved, state_log_file);
        }
    }

    logger.log("Real-time trading terminated for " + settings.coin_name);
}
// int main() {

//     //signal(SIGINT, signalHandler);

//     FibAlgoTrader trader_rune(1.0);
//     FibAlgoTrader trader_popcat(1.0);

//     std::string csv_file_rune = "./realtime_csvs/rune_1min_backtest.csv";
//     std::string csv_file_popcat = "./realtime_csvs/popcat_1min_backtest.csv";

//     int day_size = 24 * 60;                  // 1 day (in minutes)

//     int first_lookback_rune = 30 * day_size;  // 30 days of data
//     int first_lookback_popcat = 1 * day_size;  // 30 days of data

//     // Fetch initial data for each coin
//     nmDataFetcher::fetch_and_save_candles("RUNEUSDT", "1m", first_lookback_rune, false, csv_file_rune);
//     // nmDataFetcher::fetch_and_save_candles("RUNEUSDT", "1m", first_lookback_popcat, false, csv_file_popcat);


//     float position_size = 200.0f; // Example position size

//     int recalc_trade_interval_rune = 3;
//     int recalc_lookback_days_rune = 2;

//     int recalc_trade_interval_popcat = 20;
//     int recalc_lookback_days_popcat = 15;
    
//     int wait_candles=3;

//     // CoinSettings popcat_settings = {"POPCATUSDT", 1.5, position_size, wait_candles, recalc_trade_interval_popcat, recalc_lookback_days_popcat, 3};
//     // std::thread popcat_thread(RealTimeTradeEngineWatcher,  csv_file_popcat,  std::ref(trader_popcat), popcat_settings);

//     CoinSettings rune_settings = {"RUNEUSDT", 1.5, position_size, wait_candles, recalc_trade_interval_rune, recalc_lookback_days_rune, 6};
//     std::thread rune_thread(RealTimeTradeEngineWatcher2,  csv_file_rune,  std::ref(trader_rune), rune_settings);

//     //
//     rune_thread.join();

//     return 0;
// }



void fetch_data_for_symbol(const std::string& symbol, const std::string& timeframe, int day_size, bool append_or_new_save, const std::string& file_path) {
    nmDataFetcher::fetch_and_save_candles(symbol, timeframe, day_size, append_or_new_save, file_path);
}
float calculateAverageVolatility(const std::vector<DataRow> &data)
{
    if (data.empty())
    {
        std::cerr << "Error: Data vector is empty!" << std::endl;
        return 0.0f;
    }

    float totalVolatility = 0.0f;
    size_t count = 0;

    for (const auto &row : data)
    {
        if (row.low != 0.0f) // Bölme hatasını önlemek için
        {
            float percentageVolatility = ((row.high - row.low) / row.low) * 100.0f;
            totalVolatility += percentageVolatility;
            ++count;
        }
    }

    if (count == 0)
    {
        std::cerr << "Error: No valid data rows for volatility calculation!" << std::endl;
        return 0.0f;
    }

    float averageVolatility = totalVolatility / static_cast<float>(count);
    return averageVolatility;
}


// //Backtest for multiple symbols

 int main() {
    FibAlgoTrader trader(1.5);
    std::string timeframe = "1m";    // Timeframe, e.g., 1 minute
    std::string api_key = "your_api_key";
    std::string api_secret = "your_api_secret";
    bool append_or_new_save = false; // true = append to file, false = overwrite
    //start measuring perfroamnce
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<int> sensitivity_values = {100,110,120,130,140,150,160,170,180,190,200,210,220,230,240,250,260,270,280,290,300,310,320,330,340,350,360,370,380,390,400,410,420,430,440,450,460,470,480,490,500};
    std::vector<float> tpsl_values = {0.015f, 0.0175f, 0.020f, 0.025f};
    std::vector<int> lookback_days_array = {2};
    std::vector<int> apply_trades_array = {4};

    // Signal handler
    //signal(SIGINT, signalHandler);

    // Get symbols from the directory
    std::string directory_path = "./input/1min_bt_general/";
    auto symbols = nmTrader::get_symbols_from_directory(directory_path);
    std::vector<std::pair<std::string, float>> symbolVolatilities;

    std::cout << "Checking folder if increasing order: " << nmDataFetcher::checkFolderCSVIncreasingOrder(directory_path) << std::endl;
    std::cout << "Found sybols are: ";
    for(auto s : symbols)
    {
        std::cout << s << " ";
    }
    std::string performance_output = "./input/three_months_1min/performances/performances_filter.csv";
    std::string best_performance_output = "./input/three_months_1min/performances/best_performances_filter.csv";

    std::ofstream performance_csv(performance_output, std::ios::out);
    std::ofstream best_performance_csv(best_performance_output, std::ios::out);

    performance_csv << "Symbol,LookbackDays,ApplyTrades,OverallBalance,OverallReducedBalance,NextAmount,Wins,Losses,TotalTrades,WinRatio" << std::endl;
    best_performance_csv << "Symbol,BestLookbackDays,BestApplyTrades,BestOverallBalance,BestReducedBalance,BestNextAmount,Wins,Losses,TotalTrades,WinRatio" << std::endl;
    
    // Iterate over symbols and perform rolling window optimization
    for (const auto& symbol_pair : symbols) {
        std::string csv_file_to_be_saved = directory_path + symbol_pair + ".csv";

        // Initialize best performance variables for each symbol
        float best_overall_balance = -1;
        float best_overall_reduced_balance = 0;
        int best_lookback_days = 0;
        int best_apply_trades = 0;
        float best_next_amount = 0;
        float best_win_ratio = 0;
        int best_wins = 0;
        int best_losses = 0;
        int best_total_trades = 0;

        
        for (int lookback_days : lookback_days_array) {
            for (size_t apply_trades : apply_trades_array) {
                std::cout << "Performing rolling window optimization for " << symbol_pair
                          << " with lookback days: " << lookback_days << " and apply trades: " << apply_trades << "csv: " << csv_file_to_be_saved << std::endl;

                OptimizationParams optParams(csv_file_to_be_saved,
                             sensitivity_values,
                             tpsl_values,
                             lookback_days,
                             apply_trades,
                             0.00f);
                // Perform the rolling window optimization for each coin
                OptimizationResult result = trader.performRollingWindowOptimization(optParams);
                // Calculate the win ratio
                float win_ratio = (result.total_trades > 0) ? static_cast<float>(result.wins) / result.total_trades : 0;

                // Write results to the performance CSV file
                {
                    std::ofstream performance_csv(performance_output, std::ios::app);
                    performance_csv << symbol_pair << "," << lookback_days << "," << apply_trades << ","
                                    << result.overall_balance << "," << result.overall_reduced_balance << ","
                                    << result.final_next_amount << "," << result.wins << "," << result.losses << ","
                                    << result.total_trades << "," << win_ratio << std::endl;
                }

                // Check if this is the best performance for the coin
                if (result.overall_balance > best_overall_balance) {
                    best_overall_balance = result.overall_balance;
                    best_overall_reduced_balance = result.overall_reduced_balance;
                    best_lookback_days = lookback_days;
                    best_apply_trades = apply_trades;
                    best_next_amount = result.final_next_amount;
                    best_wins = result.wins;
                    best_losses = result.losses;
                    best_total_trades = result.total_trades;
                    best_win_ratio = win_ratio;
                }
            }
        }

        std::cout << "Best performance for " << symbol_pair << " with lookback days: " << best_lookback_days
                  << " and apply trades: " << best_apply_trades << " Overall balance: " << best_overall_balance
                  << " Overall reduced balance: " << best_overall_reduced_balance << " Next amount: " << best_next_amount
                  << " Wins: " << best_wins << " Losses: " << best_losses << " Total trades: " << best_total_trades
                  << " Win ratio: " << best_win_ratio << std::endl;

        // Write the best performance for the coin to best_performance.csv
        {
            std::ofstream best_performance_csv(best_performance_output, std::ios::app);
            best_performance_csv << symbol_pair << "," << best_lookback_days << "," << best_apply_trades << ","
                                 << best_overall_balance << "," << best_overall_reduced_balance << "," << best_next_amount << ","
                                 << best_wins << "," << best_losses << ","
                                 << best_total_trades << "," << best_win_ratio << std::endl;
        }
    }

    //end measuring performance
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::cout << "Total time: " << elapsed.count() << "s" << std::endl;
    return 0;
}



// int main() {
//     FibAlgoTrader trader(1.5);
//     std::string timeframe = "1m";    // Timeframe, e.g., 1 minute
//     std::string api_key = "your_api_key";
//     std::string api_secret = "your_api_secret";
//     bool append_or_new_save = false; // true = append to file, false = overwrite

//     // Signal handler
//     signal(SIGINT, signalHandler);

//     std::vector<int> sensitivity_values = {200, 250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000};
//     std::vector<float> tpsl_values = {0.01f, 0.011f, 0.012f, 0.013f, 0.014f, 0.015f, 0.016f, 0.017f, 0.018f, 0.019f, 0.020f};

//     // List of symbols and corresponding CSV files
//     std::vector<std::string> symbols = {"ARBUSDT", "MINAUSDT", "NEARUSDT", "SOLUSDT", "BNBUSDT",
//                                         "ZROUSDT", "AVAXUSDT", "OPUSDT", "INJUSDT", "TIAUSDT",
//                                         "FETUSDT", "SEIUSDT", "SUIUSDT", "FTMUSDT", "DOGEUSDT",
//                                         "RUNEUSDT", "BTCUSDT", "ETHUSDT", "CTSIUSDT", "CRVUSDT",
//                                         "TRBUSDT", "BLZUSDT", "ALGOUSDT", "STRKUSDT", "ORDIUSDT",
//                                         "1000BONKUSDT", "ALGOUSDT", "CKBUSDT", "REEFUSDT", "OGNUSDT",
//                                         "1000RATSUSDT"};

//     int day_size = 24 * 60;                  // 1 dayd
//     int initial_data_number = 5 * day_size; // 30 days of data

//     // Directory path to your CSV files
//     std::string directory_path = "input/1min_tradingview/";

//     std::string backtest_path = "input/1min_oldbacktest/";

//     //nmDataFetcher::fetch_data_for_symbols(symbols, initial_data_number, timeframe, append_or_new_save);

//     /*auto data = trader.readCSV("input/1min_oldbacktest/ARB1m.csv");

//     ResultHighBroke best_result;

//     auto first_half_data = std::vector<DataRow>(data.begin(), data.begin() + data.size() / 2);
//     auto second_half_data = std::vector<DataRow>(data.begin() + data.size() / 2, data.end());

//     std::cout<<"First half data starting and ending time: "<<first_half_data[0].open_time<<" "<<first_half_data[first_half_data.size()-1].open_time<<std::endl;
//     std::cout<<"Second half data starting and ending time: "<<second_half_data[0].open_time<<" "<<second_half_data[second_half_data.size()-1].open_time<<std::endl;

//     trader.bestDuoParameterFinder(first_half_data, sensitivity_values, tpsl_values, best_result, 0, 99999, 1000.0f);
//     std::cout << "Best sensitivity: " << best_result.best_sensitivity << " Best TP/SL: " << best_result.best_tpsl << " Best balance: " << best_result.best_balance << std::endl;

//     int apply_wins = 0;
//     int apply_losses = 0;
//     int overall_trades = 0;
//     float overall_balance = 1000.0f;
//     float next_amount = 1000.0f;
//     float total_trade_volume = 0.0f;
//     std::vector<DetailedRowHighLow> detailed_data_;

//     [[maybe_unused]] auto [final_balance, last_index, updated_next_amount] = trader.FindFinalBalanceForGivenDataByTradesAmountThread(second_half_data,
//                                                                                                                                      best_result.best_sensitivity, best_result.best_tpsl, apply_wins,
//                                                                                                                                      apply_losses, detailed_data_, 1.5f, 0, 99999, 1000.0f, 1000.0f);

//     std::cout << "Final balance: " << final_balance << " wins: " << apply_wins << " losses: " << apply_losses << " next amount: " << next_amount << std::endl;

//     std::cout << "Saving detailed data to a csv file..." << std::endl;

//     auto csv_filename = "detailed_data";
//     std::ofstream detailed_output(csv_filename);
//     detailed_output << "Open Time,Close,Position,Balance,Entry Price,TP Price,SL Price,Traded Volume,Lose Streak,Next Amount,Trade Count" << std::endl;
//     for (const auto &row : detailed_data_)
//     {
//       detailed_output
//           << row.open_time << ","
//           << row.close << ","
//           << row.position << ","
//           << row.balance << ","
//           << row.open_pos_value << ","
//           << row.tp << ","
//           << row.sl << ","
//           << row.trade_volume << ","
//           << row.lose_streak << ","
//           << row.amount << ","
//           << row.trade_size << std::endl;
//     }
//     detailed_output.close();

//     // Perform backtesting and save results*/
//nmTrader::perform_backtesting_and_save(directory_path, trader, sensitivity_values, tpsl_values);

// }

/* MultiThread Backtest Main*/

//  int main() {

//    FibAlgoTrader trader(1.5f);

//    std::string current_coin_name = "RUNE";
//    //auto input_file = "input/test_1min_files/" + current_coin_name + "_1min_105days.csv";
//    auto input_file = "rune30days1min.csv";
//    //set timer
//    auto start = std::chrono::high_resolution_clock::now();

//    std::vector<int> sensitivity_values = {200, 250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900,  950, 1000};
//    std::vector<float> tpsl_values = {0.010f, 0.012f, 0.014f, 0.016f, 0.018f, 0.020f};

//    std::vector<int> lookback_days_array = {30};
//    std::vector<size_t> apply_trades_array = {9999999};

//    float commission_per_trade = 0.0f;

//    auto data = trader.readCSV(input_file);
//    std::vector<DetailedRowHighLow> detailed_data_;
//    int apply_wins = 0;
//    int apply_losses = 0;
//    int overall_trades = 0;
//    float overall_balance = 1000.0f;
//    float next_amount = 1000.0f;
//    float total_trade_volume = 0.0f;
//    apply_trades_array = {99999999};


//    std::cout << "Saving detailed data to a csv file..." << std::endl;

//    auto csv_filename = "detailed_data_" + current_coin_name + "_30_99999999_1min.csv";
//    std::ofstream detailed_output(csv_filename);
//    detailed_output << "Open Time,Close,Position,Balance,Entry Price,TP Price,SL Price,Traded Volume,Lose Streak,Next Amount,Trade Count" << std::endl;
//    for (const auto &row : detailed_data_)
//    {
//      detailed_output
//          << row.open_time << ","
//          << row.close << ","
//          << row.position << ","
//          << row.balance << ","
//          << row.open_pos_value << ","
//          << row.tp << ","
//          << row.sl << ","
//          << row.trade_volume << ","
//          << row.lose_streak << ","
//          << row.amount << ","
//          << row.trade_size << std::endl;
//    }
//    detailed_output.close();

//    for (int lookback_days : lookback_days_array)
//    {
//      for (size_t apply_trades : apply_trades_array)
//      {
//        // std::string csv_filename = "csv_output/detailed_data_" + std::to_string(lookback_days) + "_" + std::to_string(apply_trades) + ".csv";
//        std::string csv_filename = "csv_output/detailed_data_" + current_coin_name + "_" + std::to_string(lookback_days) + "_" + std::to_string(apply_trades) + "_1min.csv";
//        auto [overall_balance, overall_reduced_balance, final_next_amount] = trader.performRollingWindowOptimization(
//            input_file, sensitivity_values, tpsl_values, lookback_days, apply_trades, commission_per_trade, true, csv_filename);

//        std::cout << "Lookback Days: " << lookback_days << " Apply Trades: " << apply_trades << std::endl;
//        std::cout << "Overall Balance: " << overall_balance << " Overall Reduced Balance (with commission): " << overall_reduced_balance << " Next Amount: " << final_next_amount << std::endl;
//        std::cout << "************** Current coin: " << current_coin_name << " **************" << std::endl;
//      }
//    }
//    // end timer
//    auto end = std::chrono::high_resolution_clock::now();
//    std::chrono::duration<double> elapsed = end - start;
//    std::cout << "Elapsed time: " << elapsed.count() << " s\n";

//    return 0;
//  }


