#ifndef ORDERMANAGER_HPP
#define ORDERMANAGER_HPP

#include <string>
#include <utility>

class OrderManager {
public:
    OrderManager(const std::string &server_ip = "127.0.0.1", int server_port = 65432);
    ~OrderManager();

    // Place a market order with optional SL and TP
    bool marketOrder(const std::string &symbol, const std::string &side, double qty,
                     double stop_loss = 0.0, double take_profit = 0.0);

    // Place a limit order with optional SL and TP
    bool limitOrder(const std::string &symbol, const std::string &side, double qty, double price,
                    double stop_loss = 0.0, double take_profit = 0.0);

    // Get position status (open/closed) and side (long/short)
    std::pair<std::string, std::string> getPositionStatus(const std::string &symbol);

    // Cancel all pending orders for a given symbol
    bool cancelPendingOrders(const std::string &symbol);

private:
    std::string server_ip;
    int server_port;

    // Helper function to send commands to the Python server and receive responses
    std::string sendOrderCommand(const std::string &command);

    // Helper function to format float numbers with fixed decimal places
    std::string formatFloat(double number, int precision);
};

#endif // ORDERMANAGER_HPP
