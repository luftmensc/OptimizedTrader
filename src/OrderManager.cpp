#include "OrderManager.hpp"
#include <iostream>
#include <sstream>
#include <arpa/inet.h>
#include <unistd.h>
#include <regex>

OrderManager::OrderManager(const std::string &server_ip, int server_port)
    : server_ip(server_ip), server_port(server_port) {}

OrderManager::~OrderManager() {}

bool OrderManager::marketOrder(const std::string &symbol, const std::string &side, double qty,
                               double stop_loss, double take_profit) {
    // Create command in the format: symbol/market/qty/side/0/stop_loss/take_profit
    std::string command = symbol + "/market/" + formatFloat(qty, 8) + "/" + side + "/0";
    command += "/" + (stop_loss > 0.0 ? formatFloat(stop_loss, 8) : "None");
    command += "/" + (take_profit > 0.0 ? formatFloat(take_profit, 8) : "None");

    std::string response = sendOrderCommand(command);
    if (response.find("Error") == std::string::npos) {
        std::cout << "Market order placed successfully." << std::endl;
        return true;
    } else {
        std::cerr << "Failed to place market order: " << response << std::endl;
        return false;
    }
}

bool OrderManager::limitOrder(const std::string &symbol, const std::string &side, double qty, double price,
                              double stop_loss, double take_profit) {
    // Create command in the format: symbol/limit/qty/side/price/stop_loss/take_profit
    std::string command = symbol + "/limit/" + formatFloat(qty, 8) + "/" + side + "/" + formatFloat(price, 8);
    command += "/" + (stop_loss > 0.0 ? formatFloat(stop_loss, 8) : "None");
    command += "/" + (take_profit > 0.0 ? formatFloat(take_profit, 8) : "None");

    std::string response = sendOrderCommand(command);
    if (response.find("Error") == std::string::npos) {
        std::cout << "Limit order placed successfully." << std::endl;
        return true;
    } else {
        std::cerr << "Failed to place limit order: " << response << std::endl;
        return false;
    }
}

std::string OrderManager::sendOrderCommand(const std::string &command) {
    int sock = 0;
    struct sockaddr_in server_addr;

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Error creating socket!" << std::endl;
        return "";
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    // Convert IPv4 address from text to binary
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address / Address not supported!" << std::endl;
        close(sock);
        return "";
    }

    // Connect to the Python server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Connection failed!" << std::endl;
        close(sock);
        return "";
    }

    // Send the message to the server
    send(sock, command.c_str(), command.size(), 0);

    // Receive the feedback from the server
    char buffer[4096] = {0};
    int bytes_received = read(sock, buffer, sizeof(buffer));
    std::string response;
    if (bytes_received > 0) {
        response = std::string(buffer, bytes_received);
        std::cout << "Feedback from server: " << response << std::endl;
    } else {
        std::cerr << "No response received from server." << std::endl;
    }

    // Close the socket
    close(sock);

    return response;
}

std::pair<std::string, std::string> OrderManager::getPositionStatus(const std::string &symbol) {
    // Construct the command for checking position status
    std::string command = "position_status/" + symbol;
    std::string response = sendOrderCommand(command);

    // Initialize status and side
    std::string status = "closed";
    std::string side = "";

    // Check if response contains "Position status: open"
    if (response.find("Position status: open") != std::string::npos) {
        status = "open";
        // If open, try to find the side line: e.g. "Side: long" or "Side: short"
        std::regex side_regex("Side:\\s+([a-zA-Z]+)");
        std::smatch match;
        if (std::regex_search(response, match, side_regex) && match.size() > 1) {
            side = match[1];
        }
    } 
    // If not found, status remains "closed" and side remains empty

    return std::make_pair(status, side);
}

bool OrderManager::cancelPendingOrders(const std::string &symbol) {
    // Create cancel command in the format: cancel/symbol
    std::string command = "cancel/" + symbol;
    std::string response = sendOrderCommand(command);

    if (response.find("canceled successfully") != std::string::npos ||
        response.find("No open orders") != std::string::npos ||
        response.find("All open orders") != std::string::npos) {
        std::cout << "All pending orders for " << symbol << " have been canceled successfully." << std::endl;
        return true;
    } else {
        std::cerr << "Failed to cancel pending orders for " << symbol << ": " << response << std::endl;
        return false;
    }
}

std::string OrderManager::formatFloat(double number, int precision) {
    std::ostringstream out;
    out.precision(precision);
    out << std::fixed << number;
    return out.str();
}
