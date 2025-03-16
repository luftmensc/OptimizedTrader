import ccxt
import datetime
import csv
import time

# Initialize Binance Futures
exchange = ccxt.binance({
    'options': {
        'defaultType': 'future',  # Use Binance Futures
    }
})

def fetch_and_save_candles(symbol, timeframe, num_candles):
    """
    Fetches the specified number of candles for a given symbol and timeframe and saves the data to a CSV file.

    :param symbol: The trading symbol, e.g., 'RUNE/USDT'
    :param timeframe: The desired timeframe, e.g., '15m', '1h'
    :param num_candles: The total number of candles to fetch
    """
    limit = 1000  # Binance limits to 1000 data points per request

    # Get the current time in milliseconds
    now = exchange.milliseconds()

    # Calculate how far back we need to go to get the requested number of candles
    all_candles = []
    fetched_candles = 0

    while fetched_candles < num_candles:
        # Calculate how many candles to fetch in this iteration
        candles_to_fetch = min(limit, num_candles - fetched_candles)

        # Fetch data from Binance Futures
        candles = exchange.fetch_ohlcv(symbol, timeframe, since=now - candles_to_fetch * exchange.parse_timeframe(timeframe) * 1000, limit=candles_to_fetch)
        
        if len(candles) == 0:
            break  # Exit loop if no more data is returned

        # Add fetched candles to the list
        all_candles += candles
        fetched_candles += len(candles)

        # Move the current time to the time of the last fetched candle
        now = candles[-1][0] - 1 

        # Be respectful of Binance's rate limits
        time.sleep(1)

    # Save the data to CSV
    csv_filename = f'{symbol.replace("/", "")}_{timeframe}_candles.csv'
    with open(csv_filename, mode='w', newline='') as file:
        writer = csv.writer(file)

        writer.writerow(['Open time', 'Open', 'High', 'Low', 'Close'])

        for candle in all_candles:
            timestamp = candle[0]
            date_time = datetime.datetime.utcfromtimestamp(timestamp / 1000).strftime('%Y-%m-%d %H:%M:%S')
            open_price = candle[1]
            high_price = candle[2]
            low_price = candle[3]
            close_price = candle[4]

            writer.writerow([date_time, open_price, high_price, low_price, close_price])

    print(f"Data has been saved to {csv_filename}. Total candles fetched: {len(all_candles)}")


# Example usage:
fetch_and_save_candles('RUNE/USDT', '15m', 672)
