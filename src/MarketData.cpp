#include "MarketData.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>

namespace TimeUtils
{
    // Days since 1970-01-01 of a civil date.
    static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int64_t>(doe) - 719468;
    }

    int64_t parseUtc(const std::string &text)
    {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        int count = std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (count != 3 && count != 6)
            return -1;
        if (month < 1 || month > 12 || day < 1 || day > 31)
            return -1;
        return daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    }

    std::string formatUtc(int64_t time)
    {
        int64_t days = time / 86400;
        int64_t seconds = time % 86400;
        if (seconds < 0)
        {
            seconds += 86400;
            days -= 1;
        }

        // Civil date from the days since 1970-01-01
        days += 719468;
        const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(days - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t year = static_cast<int64_t>(yoe) + era * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned day = doy - (153 * mp + 2) / 5 + 1;
        const unsigned month = mp < 10 ? mp + 3 : mp - 9;
        year += month <= 2;

        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02d:%02d:%02d",
                      static_cast<int>(year), month, day,
                      static_cast<int>(seconds / 3600), static_cast<int>(seconds % 3600 / 60),
                      static_cast<int>(seconds % 60));
        return buffer;
    }
}

bool MarketData::load(const std::string &symbol_, const std::string &candle_file, const std::string &funding_file)
{
    symbol = symbol_;
    candles = CandleSeries{};
    m_Bars.clear();

    std::ifstream file(candle_file);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open " << candle_file << std::endl;
        return false;
    }

    std::string line;
    std::getline(file, line); // Skip the header line
    size_t line_number = 1;

    while (std::getline(file, line))
    {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        size_t comma = line.find(',');
        if (comma == std::string::npos)
        {
            std::cerr << "Error: " << candle_file << " line " << line_number << " is malformed." << std::endl;
            return false;
        }

        int64_t time = TimeUtils::parseUtc(line.substr(0, comma));
        double values[4];
        const char *cursor = line.c_str() + comma + 1;
        bool valid = time >= 0;
        for (int i = 0; i < 4 && valid; ++i)
        {
            char *end = nullptr;
            values[i] = std::strtod(cursor, &end);
            valid = (end != cursor) && values[i] > 0.0;
            cursor = (*end == ',') ? end + 1 : end;
        }
        if (!valid)
        {
            std::cerr << "Error: " << candle_file << " line " << line_number << " is malformed." << std::endl;
            return false;
        }

        if (!candles.time.empty() && time != candles.time.back() + 60)
        {
            std::cerr << "Error: " << candle_file << " line " << line_number << ": expected "
                      << TimeUtils::formatUtc(candles.time.back() + 60) << " but got "
                      << TimeUtils::formatUtc(time)
                      << ". The candles must be 1 minute apart without gaps (see scripts/binance_fetch.py)."
                      << std::endl;
            return false;
        }

        candles.time.push_back(time);
        candles.open.push_back(values[0]);
        candles.high.push_back(values[1]);
        candles.low.push_back(values[2]);
        candles.close.push_back(values[3]);
    }

    if (candles.size() == 0)
    {
        std::cerr << "Error: No candles found in " << candle_file << std::endl;
        return false;
    }

    // Funding is optional. Without the file the backtest runs without funding payments.
    funding_rate.assign(candles.size(), 0.0);
    funding_count = 0;
    std::ifstream funding(funding_file);
    if (funding.is_open())
    {
        std::getline(funding, line); // Skip the header line
        while (std::getline(funding, line))
        {
            size_t comma = line.find(',');
            if (comma == std::string::npos)
                continue;
            int64_t time = TimeUtils::parseUtc(line.substr(0, comma));
            if (time < candles.time.front() || time > candles.time.back())
                continue;
            // The funding time can be a few milliseconds late, so it belongs to the candle of that minute
            size_t index = static_cast<size_t>((time - candles.time.front()) / 60);
            funding_rate[index] = std::strtod(line.c_str() + comma + 1, nullptr);
            ++funding_count;
        }
    }
    else
    {
        std::cerr << "Warning: No funding file for " << symbol << ", funding payments are ignored." << std::endl;
    }

    return true;
}

size_t MarketData::indexOfTime(int64_t time) const
{
    return std::lower_bound(candles.time.begin(), candles.time.end(), time) - candles.time.begin();
}

static Channel calculateChannel(const std::vector<double> &close, int length)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Channel channel;
    channel.high.assign(close.size(), nan);
    channel.low.assign(close.size(), nan);

    // Monotonic queues hold the candidates for the maximum and the minimum of the sliding window
    std::deque<size_t> max_queue;
    std::deque<size_t> min_queue;
    const size_t window = static_cast<size_t>(length);

    for (size_t i = 0; i < close.size(); ++i)
    {
        // At this point the queues contain the bars [i - length, i)
        if (i >= window)
        {
            channel.high[i] = close[max_queue.front()];
            channel.low[i] = close[min_queue.front()];
        }

        while (!max_queue.empty() && close[max_queue.back()] <= close[i])
            max_queue.pop_back();
        max_queue.push_back(i);
        while (!min_queue.empty() && close[min_queue.back()] >= close[i])
            min_queue.pop_back();
        min_queue.push_back(i);

        if (i >= window)
        {
            if (max_queue.front() == i - window)
                max_queue.pop_front();
            if (min_queue.front() == i - window)
                min_queue.pop_front();
        }
    }
    return channel;
}

static std::vector<double> calculateAtr(const TimeframeBars &bars, int period)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> atr(bars.close.size(), nan);
    double sum = 0.0;
    double value = 0.0;

    for (size_t i = 1; i < bars.close.size(); ++i)
    {
        double true_range = std::max({bars.high[i] - bars.low[i],
                                      std::fabs(bars.high[i] - bars.close[i - 1]),
                                      std::fabs(bars.low[i] - bars.close[i - 1])});
        if (i < static_cast<size_t>(period))
        {
            sum += true_range;
        }
        else if (i == static_cast<size_t>(period))
        {
            sum += true_range;
            value = sum / period;
            atr[i] = value;
        }
        else
        {
            // Wilder smoothing
            value = (value * (period - 1) + true_range) / period;
            atr[i] = value;
        }
    }
    return atr;
}

static std::vector<double> calculateEma(const std::vector<double> &close, int length)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> ema(close.size(), nan);
    const double alpha = 2.0 / (length + 1.0);
    double value = 0.0;

    for (size_t i = 0; i < close.size(); ++i)
    {
        value = (i == 0) ? close[i] : value + alpha * (close[i] - value);
        // The first values depend too much on the starting point
        if (i + 1 >= static_cast<size_t>(length))
            ema[i] = value;
    }
    return ema;
}

void MarketData::prepare(int timeframe_minutes, const std::vector<int> &channel_lengths,
                         const std::vector<int> &ema_lengths, int atr_period)
{
    TimeframeBars &bars = m_Bars[timeframe_minutes];

    if (bars.time.empty())
    {
        bars.minutes = timeframe_minutes;
        bars.bar_of_candle.assign(candles.size(), -1);
        const int64_t seconds = static_cast<int64_t>(timeframe_minutes) * 60;
        const size_t count = static_cast<size_t>(timeframe_minutes);

        // The first bar starts on the first candle which is aligned with the time frame
        size_t i = 0;
        while (i < candles.size() && candles.time[i] % seconds != 0)
            ++i;

        // Only complete bars are created
        for (; i + count <= candles.size(); i += count)
        {
            double high = candles.high[i];
            double low = candles.low[i];
            for (size_t j = i + 1; j < i + count; ++j)
            {
                high = std::max(high, candles.high[j]);
                low = std::min(low, candles.low[j]);
            }
            bars.bar_of_candle[i + count - 1] = static_cast<int32_t>(bars.time.size());
            bars.time.push_back(candles.time[i]);
            bars.open.push_back(candles.open[i]);
            bars.high.push_back(high);
            bars.low.push_back(low);
            bars.close.push_back(candles.close[i + count - 1]);
            bars.end_index.push_back(i + count - 1);
        }

        bars.atr = calculateAtr(bars, atr_period);
    }

    for (int length : channel_lengths)
    {
        if (bars.channels.find(length) == bars.channels.end())
            bars.channels[length] = calculateChannel(bars.close, length);
    }
    for (int length : ema_lengths)
    {
        if (length > 0 && bars.emas.find(length) == bars.emas.end())
            bars.emas[length] = calculateEma(bars.close, length);
    }
}
