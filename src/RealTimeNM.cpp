#include "RealTimeNM.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include "FibAlgoTrader.hpp"

namespace fs = std::filesystem;

namespace nmTrader
{

    // Function to calculate win ratio
    float calculateWinRatio(int wins, int losses)
    {
        return (wins + losses > 0) ? (static_cast<float>(wins) / (wins + losses)) : 0.0f;
    }
    // Function to find best parameters and save them in a vector
    void find_best_parameters(const std::string &symbol, const std::string &csv_file, const std::vector<int> &sensitivity_values, const std::vector<float> &tpsl_values, std::vector<BestResult> &results, FibAlgoTrader &trader)
    {
        // Read the data for the symbol
        auto data = trader.readCSV(csv_file);

        // Create a result object to store the best parameters
        ResultHighBroke best_result;

        // Find the best parameters for the current symbol
        trader.bestDuoParameterFinder(data, sensitivity_values, tpsl_values, best_result, 0, 99999, 1000.0f);

        // Calculate win ratio
        float win_ratio = calculateWinRatio(best_result.total_wins, best_result.total_losses);

        // Store the result in the vector
        results.push_back({symbol, best_result.best_sensitivity, best_result.best_tpsl, best_result.best_balance, best_result.total_wins, best_result.total_losses, win_ratio});
    }

    // Function to find all CSV files in the folder and extract symbols
    std::vector<std::string> get_symbols_from_directory(const std::string &directory_path)
    {
        std::vector<std::string> symbols;

        for (const auto &entry : fs::directory_iterator(directory_path))
        {
            if (entry.is_regular_file())
            {
                std::string file_name = entry.path().filename().string();

                // Extract the symbol by removing the "1m.csv" part
                std::string symbol = file_name.substr(0, file_name.size() - 4);
                symbols.push_back(symbol);
            }
        }
        return symbols;
    }

    // Finds the best parameters for all symbols in the directory and saves the results to a CSV file
    void perform_backtesting_and_save(const std::string &directory_path, FibAlgoTrader &trader, const std::vector<int> &sensitivity_values, const std::vector<float> &tpsl_values)
    {
        // Fetch all symbols from the directory
        std::vector<std::string> symbols = get_symbols_from_directory(directory_path);

        std::cout << "founded symbols are: " << std::endl;
        for (const auto &symbol : symbols)
        {
            std::cout << symbol << std::endl;
        }

        // Vector to hold CSV file paths corresponding to the symbols
        std::vector<std::string> csv_files;
        for (const auto &symbol : symbols)
        {
            csv_files.push_back(directory_path + symbol.substr(0, symbol.size() - 4) + "1m.csv");
        }

        std::cout << "founded csv file names are: " << std::endl;
        for (const auto &file : csv_files)
        {
            std::cout << file << std::endl;
        }

        std::cout << "symbol size: " << symbols.size() << " csv file size: " << csv_files.size() << std::endl;

        // Vector to hold the best results
        std::vector<BestResult> results;

        // Vector to hold threads
        std::vector<std::thread> threads;

        std::cout << "finding for these symbols: " << std::endl;
        for (size_t i = 15; i < symbols.size(); ++i)
        {
            std::cout << symbols[i] << std::endl;
        }

        // Create a thread for each symbol and CSV file
        for (size_t i = 0; i < symbols.size() - 15; ++i)
        {
            threads.emplace_back(find_best_parameters, symbols[i], csv_files[i], std::ref(sensitivity_values), std::ref(tpsl_values), std::ref(results), std::ref(trader));
        }

        // Wait for all threads to complete
        for (auto &th : threads)
        {
            if (th.joinable())
            {
                th.join();
            }
        }

        std::vector<std::thread> threads2;

        for (size_t i = 15; i < symbols.size(); ++i)
        {
            threads2.emplace_back(find_best_parameters, symbols[i], csv_files[i], std::ref(sensitivity_values), std::ref(tpsl_values), std::ref(results), std::ref(trader));
        }

        // Wait for all threads to complete
        for (auto &th : threads2)
        {
            if (th.joinable())
            {
                th.join();
            }
        }

        // Sort results by final balance in decreasing order
        std::sort(results.begin(), results.end(), [](const BestResult &a, const BestResult &b)
                  { return a.best_balance > b.best_balance; });
        // Open output CSV file
        std::ofstream output_file("best_parameters.csv");

        // Write CSV header
        output_file << "Symbol,Best Sensitivity,Best TPSL,Final Balance,Total Wins,Total Losses,Win Ratio\n";

        // Write the sorted results to the CSV file
        for (const auto &res : results)
        {
            output_file << res.symbol << ","
                        << res.best_sensitivity << ","
                        << res.best_tpsl << ","
                        << res.best_balance << ","
                        << res.total_wins << ","
                        << res.total_losses << ","
                        << res.win_ratio << "\n";
        }

        // Close output CSV file
        output_file.close();

        std::cout << "Best parameters saved for all symbols in sorted order." << std::endl;
    }
} // namespace nmTrader