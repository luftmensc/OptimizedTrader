#include "FibAlgoTrader.hpp"
#include "HelperFunctions.hpp"
#include <csignal>
#include <atomic>

std::vector<DataRow> FibAlgoTrader::readCSV(const std::string &filename)
{
    std::vector<DataRow> data;
    std::ifstream file(filename);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open the file!" << std::endl;
        return data;
    }

    std::string line;
    std::getline(file, line); // Skip the header line

    while (std::getline(file, line))
    {
        std::stringstream ss(line);
        DataRow row;
        std::string temp;

        std::getline(ss, row.open_time, ',');
        std::getline(ss, temp, ',');
        row.open = std::stof(temp);
        std::getline(ss, temp, ',');
        row.high = std::stof(temp);
        std::getline(ss, temp, ',');
        row.low = std::stof(temp);
        std::getline(ss, temp, ',');
        row.close = std::stof(temp);

        data.push_back(row);
    }

    file.close();
    return data;
}

ResultHighBroke FibAlgoTrader::optimizeParameters(const std::vector<DataRow> &data,
                                                  const OptimizationParams &params,
                                                  float initialTradeSize)
{
    size_t totalPairs = params.sensitivity_values.size() * params.tpsl_values.size();
    std::vector<ResultHighBroke> localResults(totalPairs);
    std::vector<std::thread> threads;
    size_t index = 0;

    // Loop over each combination of sensitivity and tpsl
    for (int sensitivity : params.sensitivity_values)
    {
        for (float tpsl : params.tpsl_values)
        {
            // Capture the current index by value.
            size_t localIndex = index;
            threads.emplace_back([this, &data, sensitivity, tpsl, initialTradeSize, &localResults, localIndex]()
                                 {
                // Create local counters that will be updated via the reference parameters.
                int wins = 0;
                int losses = 0;
                float tradedVolume = 0.0f;
                // Create a local copy of the data since TradeSimulationParams expects a non-const reference.
                std::vector<DataRow> localData = data;

                // Construct the simulation parameters using the required 12 arguments.
                TradeSimulationParams simParams(
                    localData,           // data reference
                    sensitivity,         // sensitivity
                    tpsl,                // tpsl
                    wins,                // total_wins (reference)
                    losses,              // total_losses (reference)
                    1.0f,                // multiplier (adjust as needed)
                    0,                   // start_index (beginning of localData)
                    999999,              // max_trades (large number for optimization)
                    initialTradeSize,    // initial_trade_size
                    1000.0f,             // starting_state_balance
                    1000.0f,             // first balance
                    0,                   // trading_count (if used)
                    tradedVolume         // total_traded_volume (reference)
                );

                // Run simulation for this parameter combination
                TradeSimulationResult simResult = this->simulateTradesOptimizing(simParams);

                // Use the local wins/losses counters (updated by reference) rather than simResult members.
                int totalTrades = wins + losses;
                float winRate = (totalTrades > 0) ? static_cast<float>(wins) / totalTrades : 0.0f;

                // Use an explicit constructor (or brace initialization) for ResultHighBroke.
                localResults[localIndex] = ResultHighBroke{ simResult.final_balance, sensitivity, tpsl, wins, losses, winRate }; });
            ++index;
        }
    }

    // Wait for all threads to complete
    for (auto &thread : threads)
    {
        thread.join();
    }

    // Find and return the best parameter combination based on win rate
    ResultHighBroke bestResult{};
    for (const auto &res : localResults)
    {
        if (res.best_win_rate > bestResult.best_win_rate)
            bestResult = res;
    }
    return bestResult;
}

TradeSimulationResult FibAlgoTrader::simulateTradesOptimizing(TradeSimulationParams &params)
{

    const size_t dataSize = params.data.size();
    TradingState state;
    state.balance = params.starting_state_balance;
    float nextAmount = params.initial_trade_size;
    size_t tradesMade = 0;
    size_t i = params.start_index;
    const int waitCounterConst = 5;
    int localWaitCounter = waitCounterConst;

    for (; i < dataSize && tradesMade < params.max_trades; ++i)
    {
        if (localWaitCounter > 0)
        {
            localWaitCounter--;
            continue;
        }

        if (i >= static_cast<size_t>(params.sensitivity))
        {
            float high = params.data[i - params.sensitivity].close;
            float low = params.data[i - params.sensitivity].close;
            for (size_t j = i - params.sensitivity; j < i; ++j)
            {
                high = std::max(high, params.data[j].close);
                low = std::min(low, params.data[j].close);
            }
            bool longCondition = (params.data[i].close > high);
            bool shortCondition = (params.data[i].close < low);

            if (!state.in_position)
            {
                if (longCondition)
                {
                    state.in_position = true;
                    state.position_type = "Long";
                    state.entry_price = params.data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price = state.entry_price * (1.0f + params.tpsl);
                    state.sl_price = state.entry_price * (1.0f - params.tpsl);
                    params.total_traded_volume += nextAmount;
                }
                else if (shortCondition)
                {
                    state.in_position = true;
                    state.position_type = "Short";
                    state.entry_price = params.data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price = state.entry_price * (1.0f - params.tpsl);
                    state.sl_price = state.entry_price * (1.0f + params.tpsl);
                    params.total_traded_volume += nextAmount;
                }
            }
            else
            {
                if (state.position_type == "Long")
                {
                    if (params.data[i].high >= state.tp_price)
                    {
                        double profit = state.position_size * (state.tp_price - state.entry_price);
                        state.balance += profit;
                        params.total_wins++;
                        state.in_position = false;
                        nextAmount = params.initial_trade_size;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                    else if (params.data[i].low <= state.sl_price)
                    {
                        double loss = state.position_size * (state.entry_price - state.sl_price);
                        state.balance -= loss;
                        params.total_losses++;
                        state.in_position = false;
                        nextAmount *= params.multiplier;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                }
                else if (state.position_type == "Short")
                {
                    if (params.data[i].low <= state.tp_price)
                    {
                        double profit = state.position_size * (state.entry_price - state.tp_price);
                        state.balance += profit;
                        params.total_wins++;
                        state.in_position = false;
                        nextAmount = params.initial_trade_size;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                    else if (params.data[i].high >= state.sl_price)
                    {
                        double loss = state.position_size * (state.sl_price - state.entry_price);
                        state.balance -= loss;
                        params.total_losses++;
                        state.in_position = false;
                        nextAmount *= params.multiplier;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                }
            }
        }
    }

    return TradeSimulationResult{state.balance, params.start_index + i - 1, nextAmount};
}

TradeSimulationResult FibAlgoTrader::simulateTradesApplying(TradeSimulationParams &params)
{
    std::ofstream logFile;

    logFile.open(params.logFileName, std::ios::out | std::ios::app);
    if (logFile.tellp() == 0)
    {
        logFile << "Open time,Open,High,Low,Close,Balance\n";
    }

    const size_t dataSize = params.data.size();
    TradingState state;
    state.balance = params.starting_state_balance;
    float nextAmount = params.initial_trade_size;
    size_t tradesMade = 0;
    size_t i = params.start_index;
    const int waitCounterConst = 5;
    int localWaitCounter = waitCounterConst;
    float returned_balance = params.first_balance;

    for (; i < dataSize && tradesMade < params.max_trades; ++i)
    {
        if (logFile.is_open())
        {
            const DataRow &row = params.data[i];
            logFile << row.open_time << "," << row.open << ","
                    << row.high << "," << row.low << ","
                    << row.close << "," << state.balance << "\n";
        }

        if (localWaitCounter > 0)
        {
            localWaitCounter--;
            continue;
        }

        if (i >= static_cast<size_t>(params.sensitivity))
        {
            float high = params.data[i - params.sensitivity].close;
            float low = params.data[i - params.sensitivity].close;
            for (size_t j = i - params.sensitivity; j < i; ++j)
            {
                high = std::max(high, params.data[j].close);
                low = std::min(low, params.data[j].close);
            }
            bool longCondition = (params.data[i].close > high);
            bool shortCondition = (params.data[i].close < low);

            if (!state.in_position)
            {
                if (longCondition)
                {
                    state.in_position = true;
                    state.position_type = "Long";
                    state.entry_price = params.data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price = state.entry_price * (1.0f + params.tpsl);
                    state.sl_price = state.entry_price * (1.0f - params.tpsl);
                    params.total_traded_volume += nextAmount;
                }
                else if (shortCondition)
                {
                    state.in_position = true;
                    state.position_type = "Short";
                    state.entry_price = params.data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price = state.entry_price * (1.0f - params.tpsl);
                    state.sl_price = state.entry_price * (1.0f + params.tpsl);
                    params.total_traded_volume += nextAmount;
                }
            }
            else
            {
                if (state.position_type == "Long")
                {
                    if (params.data[i].high >= state.tp_price)
                    {
                        double profit = state.position_size * (state.tp_price - state.entry_price);
                        state.balance += profit;
                        params.total_wins++;
                        state.in_position = false;
                        nextAmount = returned_balance;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                    else if (params.data[i].low <= state.sl_price)
                    {
                        double loss = state.position_size * (state.entry_price - state.sl_price);
                        state.balance -= loss;
                        params.total_losses++;
                        state.in_position = false;
                        nextAmount *= params.multiplier;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                }
                else if (state.position_type == "Short")
                {
                    if (params.data[i].low <= state.tp_price)
                    {
                        double profit = state.position_size * (state.entry_price - state.tp_price);
                        state.balance += profit;
                        params.total_wins++;
                        state.in_position = false;
                        nextAmount = returned_balance;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                    else if (params.data[i].high >= state.sl_price)
                    {
                        double loss = state.position_size * (state.sl_price - state.entry_price);
                        state.balance -= loss;
                        params.total_losses++;
                        state.in_position = false;
                        nextAmount *= params.multiplier;
                        tradesMade++;
                        localWaitCounter = waitCounterConst;
                    }
                }
            }
        }
    }

    if (logFile.is_open())
        logFile.close();

    return TradeSimulationResult{state.balance, i, nextAmount};
}

OptimizationResult FibAlgoTrader::performRollingWindowOptimization(const OptimizationParams &params,
                                                                     std::string logging_output_directory,
                                                                     std::string symbol)
{
    std::vector<DataRow> allData = readCSV(params.csv_file);
    if (allData.empty())
    {
        std::cerr << "Error: No data found in the CSV file." << std::endl;
        return {1000.0f, 1000.0f, 1000.0f, 0, 0, 0};
    }

    constexpr size_t MINUTES_PER_DAY = 24 * 60;
    int maxSensitivity = params.sensitivity_values.back();
    size_t lookbackSize = params.lookback_days * MINUTES_PER_DAY + maxSensitivity;

    float overallBalance = 1000.0f;
    float overallReducedBalance = 1000.0f;
    float nextAmount = 1000.0f;
    int overallWins = 0;
    int overallLosses = 0;
    int overallTrades = 0;
    float totalTradeVolume = 0.0f;
    float firstBalance = overallBalance;

    size_t startIndex = lookbackSize;

    // Create a unique log file name that includes the lookbackDays and applyTrades values
    std::string logFileName = logging_output_directory + "/all_trading_logs_" + symbol + "_" +
                              std::to_string(params.lookback_days) + "ld_" +
                              std::to_string(params.apply_trades) + "at_" +
                              HelperFunctions::getFormattedDate() + ".csv";

    while (startIndex < allData.size())
    {
        // Optimization phase: get lookback data
        std::vector<DataRow> lookbackData(allData.begin() + startIndex - lookbackSize,
                                          allData.begin() + startIndex);
        ResultHighBroke bestResult = optimizeParameters(lookbackData, params, 1000);

        // Application phase: build the applyData vector.
        std::vector<DataRow> applyData(allData.begin() + startIndex - bestResult.best_sensitivity,
                                       allData.end());

        // Prepare local counters for simulation
        int applyWins = 0;
        int applyLosses = 0;
        float tradedVolume = totalTradeVolume;

        // Construct the TradeSimulationParams.
        TradeSimulationParams applyParams(
            applyData,
            bestResult.best_sensitivity,
            bestResult.best_tpsl,
            applyWins,
            applyLosses,
            m_Multiplier,
            bestResult.best_sensitivity, // start_index for applyData
            params.apply_trades,
            nextAmount,
            overallBalance,
            firstBalance,
            0, // trading_count (useless here)
            tradedVolume
        );

        // Use the new log file name with the parameter info.
        applyParams.logFileName = logFileName;

        TradeSimulationResult result = simulateTradesApplying(applyParams);

        overallBalance = result.final_balance;
        overallWins += applyWins;
        overallLosses += applyLosses;
        overallTrades += (applyWins + applyLosses);
        nextAmount = result.updated_next_amount;

        // Update startIndex using the number of full-data candles processed
        startIndex += (result.last_index - bestResult.best_sensitivity);

        if ((result.last_index - bestResult.best_sensitivity) == 0)
        {
            std::cerr << "Warning: No progress in simulation. Exiting loop." << std::endl;
            break;
        }
    }

    return OptimizationResult(overallBalance, overallReducedBalance, nextAmount,
                              overallWins, overallLosses, overallTrades);
}


