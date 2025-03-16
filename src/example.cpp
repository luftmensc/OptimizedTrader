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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cpprest/http_client.h>
#include <cpprest/uri_builder.h>
#include <cpprest/json.h>

#include "FibAlgoTrader.hpp"
#include "HelperFunctions.hpp"
#include <cstdlib> // For std::rand and std::srand
#include <ctime>   // For std::time

void runOptimizationForSymbol(const std::string &symbol,
                              const std::string &inputDir,
                              const std::string &outputDir,
                              const std::vector<int> &lookbackDaysArray,
                              const std::vector<int> &applyTradesArray,
                              const std::vector<int> &sensitivityValues,
                              const std::vector<float> &tpslValues,
                              FibAlgoTrader &trader) {
    std::string dateStr = HelperFunctions::getFormattedDate();
    // Generate random number and use it inside the file name so that it's unique
    std::srand(static_cast<unsigned>(std::time(nullptr))); // Seed the random number generator
    int randomNum = std::rand();
    std::string performanceOutput = outputDir + "/performance_" + symbol + "_" + dateStr + "_" + std::to_string(randomNum) + ".csv";

    // Write header once
    {
        std::ofstream perfFile(performanceOutput, std::ios::out);
        perfFile << "Symbol,LookbackDays,ApplyTrades,OverallBalance,OverallReducedBalance,NextAmount,Wins,Losses,TotalTrades,WinRatio" << std::endl;
    }

    std::string csvFilePath = inputDir + "/" + symbol + ".csv";

    // Variables to track the best performance
    float bestOverallBalance = -1;
    float bestOverallReducedBalance = 0;
    int bestLookbackDays = 0;
    int bestApplyTrades = 0;
    float bestNextAmount = 0;
    float bestWinRatio = 0;
    int bestWins = 0;
    int bestLosses = 0;
    int bestTotalTrades = 0;

    // Loop through different parameter combinations
    for (int lookbackDays : lookbackDaysArray) {
        for (int applyTrades : applyTradesArray) {
            std::cout << "Optimizing for " << symbol 
                      << " with lookback days: " << lookbackDays 
                      << " and apply trades: " << applyTrades 
                      << " (CSV: " << csvFilePath << ")" << std::endl;

            OptimizationParams optParams(csvFilePath, sensitivityValues, tpslValues, lookbackDays, applyTrades, 0.00f);
            OptimizationResult result = trader.performRollingWindowOptimization(optParams, outputDir, symbol);
            float winRatio = (result.total_trades > 0) ? static_cast<float>(result.wins) / result.total_trades : 0.0f;

            // Append the results to the performance CSV
            {
                std::ofstream perfFile(performanceOutput, std::ios::app);
                perfFile << symbol << "," << lookbackDays << "," << applyTrades << ","
                         << result.overall_balance << "," << result.overall_reduced_balance << ","
                         << result.final_next_amount << "," << result.wins << "," << result.losses << ","
                         << result.total_trades << "," << winRatio << std::endl;
            }

            // Update best performance if this combination is better
            if (result.overall_balance > bestOverallBalance) {
                bestOverallBalance = result.overall_balance;
                bestOverallReducedBalance = result.overall_reduced_balance;
                bestLookbackDays = lookbackDays;
                bestApplyTrades = applyTrades;
                bestNextAmount = result.final_next_amount;
                bestWins = result.wins;
                bestLosses = result.losses;
                bestTotalTrades = result.total_trades;
                bestWinRatio = winRatio;
            }
        }
    }

    // Print summary for the symbol
    std::cout << "Best performance for " << symbol 
              << " with lookback days: " << bestLookbackDays 
              << " and apply trades: " << bestApplyTrades 
              << " Overall balance: " << bestOverallBalance 
              << " Overall reduced balance: " << bestOverallReducedBalance 
              << " Next amount: " << bestNextAmount 
              << " Wins: " << bestWins << " Losses: " << bestLosses 
              << " Total trades: " << bestTotalTrades 
              << " Win ratio: " << bestWinRatio << std::endl;
}

int main() {
    auto startTime = std::chrono::high_resolution_clock::now();

    FibAlgoTrader trader(1.3);

    // Define parameter arrays
    std::vector<int> sensitivityValues = {100, 200,300,400,500,600};
    std::vector<float> tpslValues = {0.010f, 0.0125f,0.015,0.0175};
    std::vector<int> lookbackDaysArray = {2,5};
    std::vector<int> applyTradesArray = {5,10};
    int time_frame = 1;

    // Set input and output directories
    std::string inputDirectory = "./input";
    std::string outputDirectory = "./output";

    // Get the list of symbols from the input directory
    auto symbols = HelperFunctions::get_symbols_from_directory(inputDirectory);

    // Check CSV ordering in the input folder
    bool csvOrderCorrect = HelperFunctions::checkFolderCSVIncreasingOrder(inputDirectory, time_frame);
    std::cout << "CSV files in folder are " << (csvOrderCorrect ? "correct" : "incorrect") << std::endl;

    // Stop if any of the csv files are not valid.
    if (!csvOrderCorrect) return 10;

    // Process each symbol
    for (const auto &symbol : symbols) {
        runOptimizationForSymbol(symbol, inputDirectory, outputDirectory,
                                 lookbackDaysArray, applyTradesArray,
                                 sensitivityValues, tpslValues, trader);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = endTime - startTime;
    std::cout << "Total time: " << elapsed.count() << "s" << std::endl;

    return 0;
}
