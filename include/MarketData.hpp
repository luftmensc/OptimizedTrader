#ifndef MARKET_DATA_HPP
#define MARKET_DATA_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 1 minute candles. Time is the open time of the candle in epoch seconds (UTC).
struct CandleSeries
{
    std::vector<int64_t> time;
    std::vector<double> open;
    std::vector<double> high;
    std::vector<double> low;
    std::vector<double> close;

    size_t size() const { return time.size(); }
};

// Highest and lowest close of the previous `length` bars (the current bar is excluded).
// The values are NaN until enough bars exist.
struct Channel
{
    std::vector<double> high;
    std::vector<double> low;
};

// Candles aggregated to a higher time frame together with the indicators calculated on them.
// Every indicator value of a bar only uses the data up to the close of that bar.
struct TimeframeBars
{
    int minutes = 1;
    std::vector<int64_t> time;
    std::vector<double> open;
    std::vector<double> high;
    std::vector<double> low;
    std::vector<double> close;
    std::vector<size_t> end_index;      // Index of the last 1 minute candle of the bar
    std::vector<int32_t> bar_of_candle; // For each 1 minute candle: the bar that closes with it, otherwise -1
    std::vector<double> atr;
    std::map<int, Channel> channels;         // Key: channel length
    std::map<int, std::vector<double>> emas; // Key: ema length
};

class MarketData
{
public:
    // Reads the candle csv (Open time,Open,High,Low,Close,...) and the optional funding csv.
    // Returns false if the candles can not be used (missing file, gaps, wrong order).
    bool load(const std::string &symbol, const std::string &candle_file, const std::string &funding_file);

    // Builds the bars and the indicators. Must be called before the simulations start,
    // so that the simulation threads only read.
    void prepare(int timeframe_minutes, const std::vector<int> &channel_lengths,
                 const std::vector<int> &ema_lengths, int atr_period);

    const TimeframeBars &bars(int timeframe_minutes) const { return m_Bars.at(timeframe_minutes); }

    // Index of the first candle with time >= the given time.
    size_t indexOfTime(int64_t time) const;

    std::string symbol;
    CandleSeries candles;
    // Funding rate paid by the longs to the shorts at the open of the candle, 0 if there is no funding.
    std::vector<double> funding_rate;
    size_t funding_count = 0;

private:
    std::map<int, TimeframeBars> m_Bars;
};

namespace TimeUtils
{
    // Parses "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DD" as UTC. Returns -1 if the text is not valid.
    int64_t parseUtc(const std::string &text);

    std::string formatUtc(int64_t time);
}

#endif // MARKET_DATA_HPP
