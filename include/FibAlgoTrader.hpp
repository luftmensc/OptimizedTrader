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
    FibAlgoTrader(float multiplier = 1.0f, int wait_counter = 5) : m_Multiplier(multiplier), m_WaitCounter(wait_counter) {}

    std::vector<DataRow> readCSV(const std::string &filename);

    OptimizationResult performRollingWindowOptimization(
        const OptimizationParams &param,
        std::string logging_output_directory,
        std::string symbol
    );

    ResultHighBroke optimizeParameters(
        const std::vector<DataRow> &data,
        const OptimizationParams &params,
        float initialTradeSize
    );

    TradeSimulationResult simulateTradesApplying(TradeSimulationParams &params);

    TradeSimulationResult simulateTradesOptimizing(TradeSimulationParams &params);

    // Martingale multiplier
    float m_Multiplier;

    constexpr float getMultiplier() const { return m_Multiplier; }

    // Cooldown after closing a trade.
    int m_WaitCounter = 5;

private:
    std::mutex mtx;
};

#endif // FIBALGO_TRADER_HPP
