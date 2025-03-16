#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <ctime>

#include "HelperFunctions.hpp"

namespace HelperFunctions {
    namespace fs = std::filesystem;

    std::string getFormattedDate() {
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm* localTime = std::localtime(&nowTime);
        std::ostringstream dateStream;
        dateStream << std::put_time(localTime, "%m_%d_%Y_%H_%M");
        return dateStream.str();
    }
    // Helper function to parse a date-time string in the format "YYYY-MM-DD HH:MM:SS".
    std::time_t parseDateTime(const std::string &dateTimeStr)
    {
        std::tm tm = {};
        std::istringstream ss(dateTimeStr);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ss.fail())
        {
            throw std::runtime_error("Failed to parse date time: " + dateTimeStr);
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
                // Remove the ".csv" suffix (assumes all files end with ".csv")
                if (file_name.size() > 4)
                {
                    std::string symbol = file_name.substr(0, file_name.size() - 4);
                    symbols.push_back(symbol);
                }
            }
        }
        return symbols;
    }

    // Checks if the CSV file timestamps are in increasing order and exactly separated by the specified time frame (in minutes).
    bool checkCSVIncreasingOrder(const std::string &file_path, int time_frame_minutes)
    {
        int expected_interval = time_frame_minutes * 60; // convert minutes to seconds

        std::ifstream file(file_path);
        if (!file.is_open())
        {
            std::cerr << "Could not open file: " << file_path << std::endl;
            return false;
        }

        std::string line;
        std::getline(file, line); // Skip header

        std::time_t previous_timestamp = 0;
        int line_number = 2; // First data line (line 1 is header)

        while (std::getline(file, line))
        {
            std::istringstream ss(line);
            std::string current_time;
            std::getline(ss, current_time, ','); // Get the "Open time" column

            try
            {
                std::time_t current_timestamp = parseDateTime(current_time);
                if (previous_timestamp != 0)
                {
                    if (current_timestamp != previous_timestamp + expected_interval)
                    {
                        std::time_t expectedTimestamp = previous_timestamp + expected_interval;
                        std::cerr << "Error: At line " << line_number 
                                  << ", expected timestamp " 
                                  << std::asctime(std::localtime(&expectedTimestamp))
                                  << " but got " << current_time << std::endl;
                        return false;
                    }
                }
                previous_timestamp = current_timestamp;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error processing line " << line_number << ": " << e.what() << std::endl;
                return false;
            }
            ++line_number;
        }
        return true;
    }

    // Checks all CSV files in a folder using the provided time frame in minutes.
    bool checkFolderCSVIncreasingOrder(const std::string &folder_path, int time_frame_minutes)
    {
        std::vector<std::string> symbols = get_symbols_from_directory(folder_path);

        std::cout << "Found symbols:" << std::endl;
        for (const auto &symbol : symbols)
        {
            std::cout << symbol << std::endl;
        }

        bool all_files_ordered = true;
        for (const auto &symbol : symbols)
        {
            std::string file_path = folder_path + "/" + symbol + ".csv";
            if (!checkCSVIncreasingOrder(file_path, time_frame_minutes))
            {
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
}
