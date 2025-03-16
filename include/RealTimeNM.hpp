// DataFetch.hpp
#ifndef REAL_TIME_NM_HPP
#define REAL_TIME_NM_HPP

#include <string>
#include <vector>
#include "FibAlgoTrader.hpp"

namespace nmTrader
{
    
    struct BestResult
    {
        std::string symbol;
        int best_sensitivity;
        float best_tpsl;
        float best_balance;
        int total_wins;
        int total_losses;
        float win_ratio;
    };

    float calculateWinRatio(int wins, int losses);
    
    void find_best_parameters(const std::string &symbol, const std::string &csv_file, const std::vector<int> &sensitivity_values, const std::vector<float> &tpsl_values, std::vector<BestResult> &results, FibAlgoTrader &trader);
    
    std::vector<std::string> get_symbols_from_directory(const std::string &directory_path);
    
    // Finds the best parameters for all symbols in the directory and saves the results to a CSV file
    void perform_backtesting_and_save(const std::string &directory_path, FibAlgoTrader &trader, const std::vector<int> &sensitivity_values, const std::vector<float> &tpsl_values);

}
 // namespace nmTrader

#endif // REAL_TIME_NM_HPP
