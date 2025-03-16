// DataFetch.hpp
#ifndef DATA_FETCH_HPP
#define DATA_FETCH_HPP

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace nmDataFetcher
{

    // Converts the timeframe string (e.g., "1m", "15m") to milliseconds
    long long get_timeframe_in_milliseconds(const std::string &interval);

    // Fetches the candle data for a given symbol, timeframe, and number of candles
    std::vector<nlohmann::json> fetch_klines_data(const std::string &symbol, const std::string &interval, int num_candles);

    // Saves the fetched candle data to a CSV file, appending if requested
    void save_to_csv(const std::string &filename, const std::vector<nlohmann::json> &klines, bool append);

    // Main function to fetch and save candles to a CSV file
    void fetch_and_save_candles(const std::string &symbol, const std::string &timeframe, int num_candles, bool append_or_new_save, const std::string &csv_file_to_be_saved);

    void fetch_symbol_data(const std::string &symbol, const std::string &csv_file, const std::string &timeframe, int initial_data_number, bool append_or_new_save);

    void fetch_data_for_symbols(const std::vector<std::string> &symbols, int initial_data_number, const std::string &timeframe, bool append_or_new_save);

    // Function to parse a datetime string and return the corresponding timestamp
    std::time_t parseDateTime(const std::string &datetime);

    // Function to check if the data in a CSV file is in increasing order of date and time
    bool checkCSVIncreasingOrder(const std::string &filename);

    bool checkFolderCSVIncreasingOrder(const std::string &folder_path);

    double fetchCurrentPrice(const std::string &symbol);

} // namespace nmDataFetcher

#endif // DATA_FETCH_HPP
