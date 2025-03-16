#ifndef HELPER_FUNCTIONS_HPP
#define HELPER_FUNCTIONS_HPP

#include <string>
#include <vector>
#include <chrono>

namespace HelperFunctions {

    std::string getFormattedDate();

    // Extracts symbols from files in the given directory.
    std::vector<std::string> get_symbols_from_directory(const std::string &directory_path);

    // Checks if a CSV file is in increasing order by "Open time" with each entry separated by time_frame_minutes.
    bool checkCSVIncreasingOrder(const std::string &file_path, int time_frame_minutes);

    // Checks all CSV files in the folder for increasing order.
    bool checkFolderCSVIncreasingOrder(const std::string &folder_path, int time_frame_minutes);

}

#endif
