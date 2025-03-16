// DataFetch.cpp
#include "DataFetch.hpp"
#include <cpprest/http_client.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <mutex>
namespace nmDataFetcher
{
    namespace fs = std::filesystem;

    std::unordered_map<std::string, std::shared_ptr<std::mutex>> mutex_map;
    std::mutex map_mutex;

    // Helper function to convert timeframe to milliseconds
    long long get_timeframe_in_milliseconds(const std::string &interval)
    {
        std::unordered_map<std::string, long long> timeframe_to_ms = {
            {"1m", 60000LL},     // 1 minute
            {"3m", 180000LL},    // 3 minutes
            {"5m", 300000LL},    // 5 minutes
            {"15m", 900000LL},   // 15 minutes
            {"30m", 1800000LL},  // 30 minutes
            {"1h", 3600000LL},   // 1 hour
            {"2h", 7200000LL},   // 2 hours
            {"4h", 14400000LL},  // 4 hours
            {"6h", 21600000LL},  // 6 hours
            {"8h", 28800000LL},  // 8 hours
            {"12h", 43200000LL}, // 12 hours
            {"1d", 86400000LL},  // 1 day
            {"3d", 259200000LL}, // 3 days
            {"1w", 604800000LL}, // 1 week
        };

        if (timeframe_to_ms.find(interval) == timeframe_to_ms.end())
        {
            throw std::invalid_argument("Invalid timeframe interval provided");
        }

        return timeframe_to_ms[interval];
    }

    // Function to fetch candles from Binance Futures
    std::vector<nlohmann::json> fetch_klines_data(const std::string &symbol, const std::string &interval, int num_candles)
    {
        using namespace web::http;
        using namespace web::http::client;

        std::vector<nlohmann::json> all_klines;
        int remaining_candles = num_candles;
        int candles_per_request = 1000; // Binance limit per request

        long long candle_duration_ms = get_timeframe_in_milliseconds(interval);

        auto now = std::chrono::system_clock::now();
        long long end_time = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() - candle_duration_ms;

        const std::string base_url = "https://fapi.binance.com";

        while (remaining_candles > 0)
        {
            int fetch_count = std::min(candles_per_request, remaining_candles);
            long long start_time = end_time - (fetch_count * candle_duration_ms);

            std::string url = base_url + "/fapi/v1/klines?symbol=" + symbol +
                              "&interval=" + interval +
                              "&startTime=" + std::to_string(start_time) +
                              "&endTime=" + std::to_string(end_time) +
                              "&limit=" + std::to_string(fetch_count);

            http_client client(U(url));
            uri_builder builder(U("/"));

            auto response = client.request(methods::GET, builder.to_string()).get();
            if (response.status_code() != status_codes::OK)
            {
                throw std::runtime_error("Failed to fetch data from Binance Futures");
            }

            auto klines_value = response.extract_json().get();
            std::string klines_str = klines_value.serialize();
            auto klines = nlohmann::json::parse(klines_str);

            if (!klines.empty())
            {
                all_klines.insert(all_klines.end(), klines.begin(), klines.end());
            }

            remaining_candles -= fetch_count;
            end_time = start_time - candle_duration_ms;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "Fetched: " << all_klines.size() << " candles out of " << num_candles << "For symbol: " << symbol << std::endl;
        }

        // Sort the candles by open time (first element in each JSON array)
        std::sort(all_klines.begin(), all_klines.end(), [](const nlohmann::json &a, const nlohmann::json &b)
                  { return a[0].get<long long>() < b[0].get<long long>(); });

        return all_klines;
    }

    // Function to save the data to CSV file (append or new save)
void save_to_csv(const std::string &filename, const std::vector<nlohmann::json> &klines, bool append)
{
    std::shared_ptr<std::mutex> file_mutex;

    // Access or create the mutex for the file
    {
        std::lock_guard<std::mutex> lock(map_mutex);
        auto it = mutex_map.find(filename);
        if (it == mutex_map.end())
        {
            // Create a new mutex for this file
            file_mutex = std::make_shared<std::mutex>();
            mutex_map[filename] = file_mutex;
        }
        else
        {
            file_mutex = it->second;
        }
    }

    // Lock the mutex for this file
    std::lock_guard<std::mutex> file_lock(*file_mutex);

    // Proceed to write to the file safely
    std::ofstream file;

    // Open file in append or overwrite mode based on the append flag
    if (append)
    {
        file.open(filename, std::ios::out | std::ios::app);
    }
    else
    {
        file.open(filename, std::ios::out | std::ios::trunc);
        file << "Open time,Open,High,Low,Close\n"; // Write header if new file
    }

    if (!file.is_open())
    {
        throw std::runtime_error("Could not open file for writing");
    }

    // Write each row of candle data to CSV
    for (const auto &candle : klines)
    {
        long long open_time = candle[0]; // This is a number (timestamp in ms)

        // Convert these string values to numbers
        double open = std::stod(candle[1].get<std::string>());
        double high = std::stod(candle[2].get<std::string>());
        double low = std::stod(candle[3].get<std::string>());
        double close = std::stod(candle[4].get<std::string>());

        // Convert open_time from milliseconds to readable format
        std::time_t open_time_t = open_time / 1000;
        char time_str[20];
        std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::gmtime(&open_time_t));

        // Write row
        file << time_str << "," << open << "," << high << "," << low << "," << close << "\n";
    }

    file.close();
    std::cout << "Data saved to " << filename << std::endl;
}

    // Main function to fetch candles and save them to CSV
    void fetch_and_save_candles(const std::string &symbol, const std::string &timeframe, int num_candles, bool append_or_new_save, const std::string &csv_file_to_be_saved)
    {
        try
        {
            // Fetch candle data
            auto klines = fetch_klines_data(symbol, timeframe, num_candles);

            // Save to CSV file (append or overwrite based on flag)
            save_to_csv(csv_file_to_be_saved, klines, append_or_new_save);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }

    void fetch_symbol_data(const std::string &symbol, const std::string &csv_file, const std::string &timeframe, int initial_data_number, bool append_or_new_save)
    {
        nmDataFetcher::fetch_and_save_candles(symbol, timeframe, initial_data_number, append_or_new_save, csv_file);
    }

    void fetch_data_for_symbols(const std::vector<std::string> &symbols, int initial_data_number, const std::string &timeframe, bool append_or_new_save)
    {
        // Vector to hold CSV file paths
        std::vector<std::string> csv_files;

        // Generate CSV file names based on the symbol names
        for (const auto &symbol : symbols)
        {
            csv_files.push_back("./input/1min_oldbacktest/" + symbol + ".csv");
        }

        // Vector to hold threads
        std::vector<std::thread> threads;

        // Create a thread for each symbol and CSV file
        for (size_t i = 0; i < symbols.size(); ++i)
        {
            threads.emplace_back(fetch_symbol_data, symbols[i], csv_files[i], timeframe, initial_data_number, append_or_new_save);
        }

        // Wait for all threads to complete
        for (auto &th : threads)
        {
            if (th.joinable())
            {
                th.join();
            }
        }

        std::cout << "Data fetching completed for all symbols." << std::endl;
    }

    // Helper function to convert date string to time_t
    std::time_t parseDateTime(const std::string &datetime)
    {
        std::tm tm = {};
        std::istringstream ss(datetime);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ss.fail())
        {
            throw std::runtime_error("Failed to parse datetime: " + datetime);
        }
        return std::mktime(&tm);
    }

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

    // Function to check if the CSV file is in increasing order by "Open time"
    bool checkCSVIncreasingOrder(const std::string &file_path)
    {
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            std::cerr << "Could not open file: " << file_path << std::endl;
            return false;
        }

        std::string line;
        std::getline(file, line); // Skip header

        std::string previous_time;
        std::time_t previous_timestamp = 0;
        int line_number = 2; // Start with 2 because the first line is the header

        while (std::getline(file, line))
        {
            std::istringstream ss(line);
            std::string current_time;
            std::getline(ss, current_time, ','); // Get the "Open time" column

            try
            {
                std::time_t current_timestamp = parseDateTime(current_time);

                // Check if current timestamp is greater than the previous one
                if (previous_timestamp != 0 && current_timestamp <= previous_timestamp)
                {
                    std::cerr << "Error: Timestamps not in increasing order at line " << line_number
                              << " (" << current_time << ")" << std::endl;
                    return false;
                }

                previous_timestamp = current_timestamp;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error processing line " << line_number << ": " << e.what() << std::endl;
                return false;
            }

            line_number++;
        }

        return true;
    }

    bool checkFolderCSVIncreasingOrder(const std::string &folder_path)
    {
        std::vector<std::string> symbols = get_symbols_from_directory(folder_path);

        std::cout << "Found symbols are: " << std::endl;
        for (const auto &symbol : symbols)
        {
            std::cout << symbol << std::endl;
        }

        bool all_files_ordered = true;

        for (const auto &symbol : symbols)
        {
            std::string file_path = folder_path + symbol + ".csv";    // Correct file path without unnecessary subst
            if (checkCSVIncreasingOrder(file_path))
            {
                std::cout << ".";
            }
            else
            {
                std::cout << std::endl;
                std::cerr << "CSV data is not correctly ordered for " << symbol << "." << std::endl;
                all_files_ordered = false;
            }
            
        }
        if (all_files_ordered)
        {
            std::cout << "All CSV files are correctly ordered." << std::endl;
        }
        return all_files_ordered;
    }

    double fetchCurrentPrice(const std::string &symbol)
    {
        using namespace web::http;
        using namespace web::http::client;

        try
        {
            const std::string base_url = "https://fapi.binance.com";

            // Construct the API endpoint URL
            std::string endpoint = "/fapi/v1/ticker/price";

            // Create HTTP client with the base URL
            http_client client(U(base_url));

            // Build the URI with query parameters
            uri_builder builder(U(endpoint));
            builder.append_query(U("symbol"), U(symbol));

            // Send the GET request
            auto response = client.request(methods::GET, builder.to_string()).get();

            // Check the response status code
            if (response.status_code() != status_codes::OK)
            {
                throw std::runtime_error("Failed to fetch current price from Binance Futures");
            }

            // Extract the response as a JSON value
            auto json_value = response.extract_json().get();

            // Convert the JSON value to a string and parse it using nlohmann::json
            std::string json_str = json_value.serialize();
            auto json_data = nlohmann::json::parse(json_str);

            // Extract the "price" field from the JSON data
            if (json_data.contains("price"))
            {
                std::string price_str = json_data["price"];
                double current_price = std::stod(price_str);
                return current_price;
            }
            else
            {
                throw std::runtime_error("Price not found in the response");
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error fetching current price for " << symbol << ": " << e.what() << std::endl;
            return 0.0; // Return 0.0 or handle the error as appropriate
        }
    }

} // namespace nmDataFetcher
