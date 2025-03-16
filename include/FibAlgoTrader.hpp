#ifndef FIBALGO_TRADER_HPP
#define FIBALGO_TRADER_HPP

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>
#include <tuple>
#include <limits>
#include <iomanip>
#include <cmath>
#include <thread>
#include <mutex>
#include "DataStructure.hpp"

class FibAlgoTrader
{
public:
    FibAlgoTrader(float multiplier = 1.0f) : multiplier_(multiplier) {}

    std::vector<DataRow>
    readCSV(const std::string &filename);
    std::vector<DataRowV>
    readCSV_V(const std::string &filename);

    LowMidHighFibVectors
    calculateFibLevels(const std::vector<DataRow> &data, int sensitivity, float fib_high_multiplier = 0.764f, float fib_low_multiplier = 0.236f);

    std::tuple<float, size_t>
    FindFinalBalanceForGivenDataByTrades(std::vector<DataRow> &data, int sensitivity, int ma_length, float tpsl,
                                         int &total_wins, int &total_losses, std::vector<DetailedRow> &detailed_data,
                                         float fib_high_multiplier, float fib_low_multiplier, float multiplier, size_t start_index, size_t max_trades);

    TradeSimulationResult
    FindFinalBalanceForGivenDataByTradesAmount(const TradeSimulationParams &params);

    std::tuple<float, size_t, float>
    FindFinalBalanceForGivenDataByTradesAmount_V(std::vector<DataRowV> &data, int sensitivity, float tpsl,
                                                 int &total_wins, int &total_losses, std::vector<DetailedRowHighLow> &detailed_data,
                                                 float multiplier, size_t start_index, size_t max_trades, float initial_trade_size,
                                                 float starting_state_balance, int trading_count, float &total_traded_volume);

    std::tuple<float, size_t, float> 
    FindFinalBalanceForGivenDataByTradesAmountThread(std::vector<DataRow> &data, int sensitivity, float tpsl,
                                                                int &total_wins, int &total_losses,
                                                                float multiplier, size_t start_index, size_t max_trades, 
                                                                float initial_trade_size, float starting_state_balance);


    void
    analyzeForTPSLByTradesAmount(const std::vector<DataRow> &data, int sensitivity, const std::vector<float> &tpsl_values,
                                 ResultHighBroke &local_best_result, float commission_per_trade, size_t max_trades,
                                 float initial_trade_size);

    void
    bestDuoParameterFinder(const std::vector<DataRow> &data, const std::vector<int> &sensitivity_values,
                           const std::vector<float> &tpsl_values, ResultHighBroke &best_result,
                           float commission_per_trade, size_t max_trades, float initial_trade_size);


    OptimizationResult
    performRollingWindowOptimization(const OptimizationParams& params);

    float multiplier_;

    constexpr float getMultiplier() const { return multiplier_; }

    int wait_counter = 5;

private:
    std::mutex mtx;
};

#endif // FIBALGO_TRADER_HPP
