#include "FibAlgoTrader.hpp"
#include <csignal>
#include <atomic>

// void signalHandler(int signum)
// {
//     std::cout << "Interrupt signal (" << signum << ") received.\n";
//     running = false;
// }

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


std::tuple<float, size_t, float>
FibAlgoTrader::FindFinalBalanceForGivenDataByTradesAmountThread(std::vector<DataRow> &data, int sensitivity, float tpsl,
                                                                int &total_wins, int &total_losses,
                                                                float multiplier, size_t start_index, size_t max_trades,
                                                                float initial_trade_size, float starting_state_balance)
{
    const size_t dataSize = data.size();

    std::vector<float> close_ma(dataSize, 0.0f);
    std::vector<std::string> positions(dataSize, "None");
    std::vector<float> balances(dataSize, 1000.0f);
    std::vector<float> tp_prices(dataSize, 0.0f);
    std::vector<float> sl_prices(dataSize, 0.0f);

    TradingState state;
    state.balance = starting_state_balance;
    float nextAmount = initial_trade_size;
    float returnToBeginningAmount = 1000.0f;
    size_t trades_made = 0;
    size_t total_trades = 0;
    size_t i = start_index;
    float traded_volume = 0.0f;

    // Introduce a local wait_counter
    int local_wait_counter = 0;

    for (; i < dataSize; ++i)
    {
        // Check if we are skipping candles due to the wait counter
        if (local_wait_counter > 0)
        {
            local_wait_counter--;
            continue; // Skip the rest of the logic for this candle
        }

        if (i >= static_cast<size_t>(sensitivity))
        {
            float high = data[i - sensitivity].close;
            float low = data[i - sensitivity].close;

            for (size_t j = i - sensitivity; j < i; ++j)
            {
                high = std::max(high, data[j].close);
                low = std::min(low, data[j].close);
            }

            const bool closed_above_high = data[i].close > high;
            const bool closed_below_low = data[i].close < low;

            const bool longCondition = closed_above_high;
            const bool shortCondition = closed_below_low;

            if (!state.in_position)
            {
                if (longCondition)
                {
                    state.in_position = true;
                    state.position_type = "Long";
                    state.entry_price = data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price = state.entry_price * (1 + tpsl);
                    state.sl_price = state.entry_price * (1 - tpsl);
                    traded_volume += nextAmount;
                }
                else if (shortCondition)
                {
                    state.in_position = true;
                    state.position_type = "Short";
                    state.entry_price = data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price = state.entry_price * (1 - tpsl);
                    state.sl_price = state.entry_price * (1 + tpsl);
                    traded_volume += nextAmount;
                }
            }
            else
            {
                if (state.position_type == "Long")
                {
                    if (data[i].high >= state.tp_price)
                    {
                        double profit = state.position_size * (state.tp_price - state.entry_price);
                        state.balance += profit;
                        total_wins++;
                        state.trades.push_back({data[i].open_time, state.entry_price, state.tp_price, "Long", profit});
                        state.in_position = false;
                        traded_volume += nextAmount;
                        nextAmount = returnToBeginningAmount;
                        trades_made++;

                        // Set the local wait counter to skip candles
                        local_wait_counter = wait_counter;
                    }
                    else if (data[i].low <= state.sl_price)
                    {
                        double loss = state.position_size * (state.entry_price - state.sl_price);
                        state.balance -= loss;
                        total_losses++;
                        state.trades.push_back({data[i].open_time, state.entry_price, state.sl_price, "Long", -loss});
                        state.in_position = false;
                        traded_volume += nextAmount;
                        nextAmount *= multiplier;
                        trades_made++;
                        // Set the local wait counter to skip candles
                        local_wait_counter = wait_counter;
                    }
                }
                else if (state.position_type == "Short")
                {
                    if (data[i].low <= state.tp_price)
                    {
                        double profit = state.position_size * (state.entry_price - state.tp_price);
                        state.balance += profit;
                        total_wins++;
                        state.trades.push_back({data[i].open_time, state.entry_price, state.tp_price, "Short", profit});
                        state.in_position = false;
                        traded_volume += nextAmount;
                        nextAmount = returnToBeginningAmount;
                        trades_made++;

                        // Set the local wait counter to skip candles
                        local_wait_counter = wait_counter;
                    }
                    else if (data[i].high >= state.sl_price)
                    {
                        double loss = state.position_size * (state.sl_price - state.entry_price);
                        state.balance -= loss;
                        total_losses++;
                        state.trades.push_back({data[i].open_time, state.entry_price, state.sl_price, "Short", -loss});
                        state.in_position = false;
                        traded_volume += nextAmount;
                        nextAmount *= multiplier;
                        trades_made++;
                        // Set the local wait counter to skip candles
                        local_wait_counter = wait_counter;
                    }
                }
            }

            balances[i] = state.balance;
        }
    }

    return {state.balance, start_index + i - 1, nextAmount};
}


// A simple helper (if needed) to avoid NaN issues:
inline float nz(const float &val, float default_val = 0.0f) {
    return std::isnan(val) ? default_val : val;
}

// Compute ADX with a given ADXLen (e.g., 14).
// Returns a std::vector<float> where adx[i] is the ADX for bar i.
std::vector<float> ComputeADX(const std::vector<DataRow>& data, int ADXLen)
{
    const int size = static_cast<int>(data.size());
    std::vector<float> adxValues(size, 0.0f);

    if (size < ADXLen + 1) {
        // Not enough data to compute ADX => return zeros or handle differently
        return adxValues;
    }

    // Arrays to store intermediate values
    std::vector<float> TR(size, 0.0f);
    std::vector<float> DMp(size, 0.0f);  // +DM
    std::vector<float> DMm(size, 0.0f);  // -DM
    std::vector<float> smTR(size, 0.0f);
    std::vector<float> smDMp(size, 0.0f);
    std::vector<float> smDMm(size, 0.0f);
    std::vector<float> DIp(size, 0.0f);
    std::vector<float> DIm(size, 0.0f);
    std::vector<float> DX(size, 0.0f);

    // 1) Calculate True Range (TR), +DM, -DM for each candle
    // Start from i=1, because we compare with candle[i-1]
    for (int i = 1; i < size; i++) {
        float currentHigh  = data[i].high;
        float currentLow   = data[i].low;
        float prevClose    = data[i-1].close;
        float prevHigh     = data[i-1].high;
        float prevLow      = data[i-1].low;

        float moveUp   = currentHigh - prevHigh;
        float moveDown = prevLow - currentLow;

        float range1 = currentHigh - currentLow;
        float range2 = std::fabs(currentHigh - prevClose);
        float range3 = std::fabs(currentLow  - prevClose);

        TR[i]  = std::max(range1, std::max(range2, range3));
        DMp[i] = (moveUp > moveDown && moveUp > 0) ? moveUp : 0.0f;
        DMm[i] = (moveDown > moveUp && moveDown > 0) ? moveDown : 0.0f;
    }

    // 2) Wilder's smoothing for TR, +DM, and -DM
    // We'll do the first "seed" value at index = ADXLen as the sum of the first ADXLen TRs/DMs
    float sumTR  = 0.0f, sumDMp = 0.0f, sumDMm = 0.0f;
    for (int i = 1; i <= ADXLen; i++) {
        sumTR  += TR[i];
        sumDMp += DMp[i];
        sumDMm += DMm[i];
    }
    smTR[ADXLen]  = sumTR;
    smDMp[ADXLen] = sumDMp;
    smDMm[ADXLen] = sumDMm;

    // From ADXLen+1 onward, do typical Wilder smoothing:
    for (int i = ADXLen + 1; i < size; i++) {
        smTR[i]  = smTR[i - 1]  - (smTR[i - 1] / ADXLen) + TR[i];
        smDMp[i] = smDMp[i - 1] - (smDMp[i - 1] / ADXLen) + DMp[i];
        smDMm[i] = smDMm[i - 1] - (smDMm[i - 1] / ADXLen) + DMm[i];
    }

    // 3) Calculate DI+ / DI- (in %), and then DX
    for (int i = ADXLen; i < size; i++) {
        float atr    = smTR[i];
        float pdm    = smDMp[i];
        float ndm    = smDMm[i];
        if (atr == 0.0f) {
            DIp[i] = 0.0f;
            DIm[i] = 0.0f;
            DX[i]  = 0.0f;
        } else {
            DIp[i] = (pdm / atr) * 100.0f;
            DIm[i] = (ndm / atr) * 100.0f;
            DX[i]  = (std::fabs(DIp[i] - DIm[i]) / (DIp[i] + DIm[i])) * 100.0f;
        }
    }

    // 4) Compute final ADX, also using Wilder smoothing on DX
    // The "standard" approach is:
    //  - The first ADX is the average of the first ADXLen "DX" values (starting at i = ADXLen)
    //  - Then from there, apply Wilder's smoothing.

    // Let’s seed ADX at index = ADXLen*2 - 1 (similar to PineScript’s typical offset).
    if (ADXLen * 2 <= size) {
        float sumDX = 0.0f;
        for (int i = ADXLen; i < ADXLen * 2; i++) {
            sumDX += DX[i];
        }
        // average of those ADXLen bars
        adxValues[ADXLen * 2 - 1] = sumDX / (float)ADXLen;

        // then Wilder smoothing for subsequent
        for (int i = ADXLen * 2; i < size; i++) {
            adxValues[i] = ((adxValues[i - 1] * (ADXLen - 1)) + DX[i]) / (float)ADXLen;
        }
    }

    return adxValues;
}

std::vector<float> ComputeATR(const std::vector<DataRow>& data, int period)
{
    int size = static_cast<int>(data.size());
    std::vector<float> atrValues(size, 0.0f);
    if (size < 2) {
        return atrValues; // Not enough data
    }

    // 1) Compute the initial True Range for each candle:
    std::vector<float> TR(size, 0.0f);
    for (int i = 1; i < size; i++) {
        float currentHigh = data[i].high;
        float currentLow  = data[i].low;
        float prevClose   = data[i - 1].close;

        float range1 = currentHigh - currentLow;
        float range2 = std::fabs(currentHigh - prevClose);
        float range3 = std::fabs(currentLow  - prevClose);

        TR[i] = std::max(range1, std::max(range2, range3));
    }

    // 2) Calculate the first ATR value (we can seed at index = period, summing the first 'period' TRs)
    //    or do it from the second candle. We'll do the standard approach:
    //    ATR[period - 1] = average of TR up to that point. Then from period onward, use Wilder's smoothing.
    float sumTR = 0.0f;
    int startIndex = std::min(period, size - 1);
    for (int i = 1; i <= startIndex; i++) {
        sumTR += TR[i];
    }

    if (startIndex > 0) {
        atrValues[startIndex] = sumTR / static_cast<float>(startIndex);
    }

    // 3) Wilder's smoothing for subsequent bars
    for (int i = startIndex + 1; i < size; i++) {
        // ATR[i] = ( (ATR[i-1] * (period - 1)) + TR[i] ) / period
        float prevATR = atrValues[i - 1];
        atrValues[i] = ( (prevATR * (period - 1)) + TR[i] ) / static_cast<float>(period);
    }

    return atrValues;
}

TradeSimulationResult
FibAlgoTrader::FindFinalBalanceForGivenDataByTradesAmount(const TradeSimulationParams &params)
{
    const size_t dataSize = params.data.size();
    // std::vector<std::string> positions(dataSize, "None");
    std::vector<float> balances(dataSize, 1000.0f);
    std::vector<float> tp_prices(dataSize, 0.0f);
    std::vector<float> sl_prices(dataSize, 0.0f);

    // Make sure you have "entry_time" in your TradingState structure:
    // struct TradingState {
    //     ...
    //     bool in_position = false;
    //     std::string position_type;
    //     float entry_price = 0.0f;
    //     float position_size = 0.0f;
    //     float tp_price = 0.0f;
    //     float sl_price = 0.0f;
    //     float balance = 0.0f;
    //     float entry_adx = 0.0f;
    //     std::string entry_time; // <-- NEW FIELD: for storing open time
    //     ...
    // };
    TradingState state;
    state.balance = params.starting_state_balance;

    float nextAmount = params.initial_trade_size;
    float returnToBeginningAmount = 1000.0f;
    size_t trades_made = 0;
    size_t i = params.start_index;
    float traded_volume = 0.0f;

    int ADXLen = 14 * 5; // or whichever length you prefer
    std::vector<float> adx = ComputeADX(params.data, ADXLen);

    // Introduce a local wait_counter
    int local_wait_counter = 0;

    for (; i < dataSize && trades_made < params.max_trades; ++i)
    {
        if (local_wait_counter > 0)
        {
            local_wait_counter--;
            continue; // Skip the rest of the logic for this candle
        }

        if (i >= static_cast<size_t>(params.sensitivity))
        {
            float high = params.data[i - params.sensitivity].close;
            float low  = params.data[i - params.sensitivity].close;

            // Range over the past 'sensitivity' candles (excluding current)
            for (size_t j = i - params.sensitivity; j < i; ++j)
            {
                high = std::max(high, params.data[j].close);
                low  = std::min(low,  params.data[j].close);
            }

            const bool closed_above_high = (params.data[i].close > high);
            const bool closed_below_low  = (params.data[i].close < low);

            // Extract the hour from open_time
            std::tm timeInfo = {};
            strptime(params.data[i].open_time.c_str(), "%Y-%m-%d %H:%M:%S", &timeInfo);
            int hour = timeInfo.tm_hour;
            int minute = timeInfo.tm_min;

            // Condition: only open position if time is between 00:41 and 08:41
            const bool isWithinTradingHours = 1; // 6 14 %55.6 12 20 %49

            const bool longCondition  = closed_above_high&&isWithinTradingHours;
            const bool shortCondition = closed_below_low&&isWithinTradingHours;

            if (!state.in_position)
            {
                // Attempt to open LONG
                if (longCondition /* && isWithinTradingHours */)
                {
                    state.in_position   = true;
                    state.position_type = "Long";
                    state.entry_price   = params.data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price      = state.entry_price * (1.0f + params.tpsl);
                    state.sl_price      = state.entry_price * (1.0f - params.tpsl);
                    
                    // Record ADX at entry
                    float currentAdx = (i < adx.size()) ? adx[i] : 0.0f;
                    state.entry_adx   = currentAdx;

                    // Save the *open time* of the trade
                    state.entry_time = params.data[i].open_time;

                    params.total_traded_volume += nextAmount;
                }
                // Attempt to open SHORT
                else if (shortCondition /* && isWithinTradingHours */)
                {
                    state.in_position   = true;
                    state.position_type = "Short";
                    state.entry_price   = params.data[i].close;
                    state.position_size = nextAmount / state.entry_price;
                    state.tp_price      = state.entry_price * (1.0f - params.tpsl);
                    state.sl_price      = state.entry_price * (1.0f + params.tpsl);
                    
                    float currentAdx = (i < adx.size()) ? adx[i] : 0.0f;
                    state.entry_adx   = currentAdx;

                    // Save the *open time* of the trade
                    state.entry_time = params.data[i].open_time;

                    params.total_traded_volume += nextAmount;
                }
            }
            else
            {
                // If we already have an open position...
                if (state.position_type == "Long")
                {
                    if (params.data[i].high >= state.tp_price)
                    {
                        double profit = state.position_size * (state.tp_price - state.entry_price);
                        state.balance += profit;
                        params.total_wins++;
                        state.trades.push_back({
                            params.data[i].open_time,
                            state.entry_price,
                            state.tp_price,
                            "Long",
                            profit
                        });
                        state.in_position = false;
                        params.total_traded_volume += nextAmount;
                        nextAmount = returnToBeginningAmount;
                        trades_made++;
                        local_wait_counter = wait_counter;
                        
                        // Write to benchmark.csv
                        {
                            std::ofstream csvFile("./benchmark.csv", std::ios::app);
                            if (csvFile.is_open())
                            {
                                // record the entry_time + ADX + WIN
                                csvFile << state.entry_time << ","
                                        << state.entry_adx   << ",WIN\n";
                            }
                        }
                    }
                    else if (params.data[i].low <= state.sl_price)
                    {
                        double loss = state.position_size * (state.entry_price - state.sl_price);
                        state.balance -= loss;
                        params.total_losses++;
                        state.trades.push_back({
                            params.data[i].open_time,
                            state.entry_price,
                            state.sl_price,
                            "Long",
                            -loss
                        });
                        state.in_position = false;
                        params.total_traded_volume += nextAmount;
                        nextAmount *= params.multiplier;
                        trades_made++;
                        local_wait_counter = wait_counter;

                        
                        // Write to ./benchmark.csv
                        {
                            std::ofstream csvFile("./benchmark.csv", std::ios::app);
                            if (csvFile.is_open())
                            {
                                // record the entry_time + ADX + LOSS
                                csvFile << state.entry_time << ","
                                        << state.entry_adx   << ",LOSS\n";
                            }
                        }
                    }
                }
                else if (state.position_type == "Short")
                {
                    if (params.data[i].low <= state.tp_price)
                    {
                        double profit = state.position_size * (state.entry_price - state.tp_price);
                        state.balance += profit;
                        params.total_wins++;
                        state.trades.push_back({
                            params.data[i].open_time,
                            state.entry_price,
                            state.tp_price,
                            "Short",
                            profit
                        });
                        state.in_position = false;
                        params.total_traded_volume += nextAmount;
                        nextAmount = returnToBeginningAmount;
                        trades_made++;
                        local_wait_counter = wait_counter;
                        
                        // Write to ./benchmark.csv
                        {
                            std::ofstream csvFile("./benchmark.csv", std::ios::app);
                            if (csvFile.is_open())
                            {
                                csvFile << state.entry_time << ","
                                        << state.entry_adx   << ",WIN\n";
                            }
                        }
                    }
                    else if (params.data[i].high >= state.sl_price)
                    {
                        double loss = state.position_size * (state.sl_price - state.entry_price);
                        state.balance -= loss;
                        params.total_losses++;
                        state.trades.push_back({
                            params.data[i].open_time,
                            state.entry_price,
                            state.sl_price,
                            "Short",
                            -loss
                        });
                        state.in_position = false;
                        params.total_traded_volume += nextAmount;
                        nextAmount *= params.multiplier;
                        trades_made++;
                        local_wait_counter = wait_counter;
                        
                        // Write to benchmark.csv
                        {
                            std::ofstream csvFile("./benchmark.csv", std::ios::app);
                            if (csvFile.is_open())
                            {
                                csvFile << state.entry_time << ","
                                        << state.entry_adx   << ",LOSS\n";
                            }
                        }
                    }
                }
            }
            balances[i] = state.balance;
        }
    }

    return { TradeSimulationResult(state.balance, params.start_index + i - 1, nextAmount) };
}


void FibAlgoTrader::bestDuoParameterFinder(const std::vector<DataRow> &data, const std::vector<int> &sensitivity_values,
                                           const std::vector<float> &tpsl_values, ResultHighBroke &best_result,
                                           float commission_per_trade, size_t max_trades, float initial_trade_size)
{
    const std::vector<DataRow> original_data = data;
    std::vector<std::thread> threads;
    std::vector<ResultHighBroke> local_results(sensitivity_values.size());

    for (size_t i = 0; i < sensitivity_values.size(); ++i)
    {
        threads.emplace_back([this, &original_data, sensitivity = sensitivity_values[i], &tpsl_values, &local_results, i, commission_per_trade, max_trades, initial_trade_size]()
                             { this->analyzeForTPSLByTradesAmount(original_data, sensitivity, tpsl_values, local_results[i], commission_per_trade, max_trades, initial_trade_size); });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    for (const auto &local_result : local_results)
    {
        float win_rate = (float)local_result.total_wins / (local_result.total_wins + local_result.total_losses);
        if (local_result.best_win_rate > best_result.best_win_rate)
        {
            best_result = local_result;
        }
    }
}

void FibAlgoTrader::analyzeForTPSLByTradesAmount(const std::vector<DataRow> &data, int sensitivity, const std::vector<float> &tpsl_values,
                                                 ResultHighBroke &local_best_result, float commission_per_trade, size_t max_trades, float initial_trade_size)
{
    for (const float tpsl : tpsl_values)
    {
        std::vector<DataRow> local_data = data;
        int total_wins = 0;
        int total_losses = 0;
        auto [final_balance, end_index, next_amount] = FindFinalBalanceForGivenDataByTradesAmountThread(local_data, sensitivity, tpsl, total_wins,
                                                       total_losses, 1, 0, max_trades, initial_trade_size,1000.0f);

        float win_rate = (float)total_wins / (total_wins + total_losses);

        //compare for best win rate
        if (win_rate > local_best_result.best_win_rate)
        {
            local_best_result.best_balance = final_balance;
            local_best_result.best_sensitivity = sensitivity;
            local_best_result.best_tpsl = tpsl;
            local_best_result.total_wins = total_wins;
            local_best_result.total_losses = total_losses;
            local_best_result.best_win_rate = win_rate;
        }
    }
}

OptimizationResult FibAlgoTrader::performRollingWindowOptimization(const OptimizationParams& params)
{
    std::vector<DataRow> all_data = readCSV(params.csv_file);

    if (all_data.empty())
    {
        std::cout << "Error: No data found in the CSV file." << std::endl;
        return {0.0f, 0.0f, 0.0f, 0, 0, 0};
    }

    size_t data_size = all_data.size();
    size_t day_size = 24 * 60; 
    int max_sensitivity = params.sensitivity_values.back();
    size_t lookback_size = params.lookback_days * day_size + max_sensitivity;

    float overall_balance = 1000.0f;
    float overall_reduced_balance = 1000.0f;
    float starting_state_balance = 1000.0f;
    float updated_state_balance = starting_state_balance;
    int overall_wins = 0;
    int overall_losses = 0;
    int overall_trades = 0;
    float next_amount = 1000.0f; // Initial amount for the first trade
    float total_trade_volume = 0.0f;

    size_t start_index = lookback_size;
    size_t prev_start_index = 0;

    while (start_index < data_size)
    {
        if (prev_start_index == start_index)
        {
            std::cout << "Error: Stuck in a loop, breaking out of the loop. prev: " << prev_start_index << " current: " << start_index << std::endl;
            break;
        }

        // Get the lookback data
        std::vector<DataRow> lookback_data(all_data.begin() + start_index - lookback_size, all_data.begin() + start_index);

        ResultHighBroke best_result;

        bestDuoParameterFinder(lookback_data, params.sensitivity_values, params.tpsl_values, best_result, params.commission_per_trade, 999999, next_amount);

        int apply_wins = 0;
        int apply_losses = 0;
        auto apply_data = std::vector<DataRow>(all_data.begin() + start_index - best_result.best_sensitivity, all_data.end());

        TradeSimulationParams apply_params(apply_data, best_result.best_sensitivity, best_result.best_tpsl,
                                        apply_wins, apply_losses, multiplier_, 0, params.apply_trades, 
                                        next_amount, overall_balance, overall_trades, total_trade_volume);

        TradeSimulationResult result = FindFinalBalanceForGivenDataByTradesAmount(apply_params);

        overall_balance = result.final_balance;
        overall_wins += apply_wins;
        overall_losses += apply_losses;
        int total_trades = apply_wins + apply_losses;
        overall_trades += total_trades;

        prev_start_index = start_index;
        start_index += result.last_index;
        next_amount = result.updated_next_amount; // Use the updated amount for the next set of trades
    }

    // Return the final performance values including wins, losses, and total trades
    return OptimizationResult(overall_balance, overall_reduced_balance, next_amount, overall_wins, overall_losses, overall_trades);
}
