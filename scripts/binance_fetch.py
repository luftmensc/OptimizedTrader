from binance.client import Client
import pandas as pd
from datetime import datetime, timedelta
import concurrent.futures
import time

def fetch_and_save_klines(symbol, interval, days, api_key, api_secret, output_file, multiply=False):
    # Connect to Binance Futures
    client = Client(api_key, api_secret)

    # Calculate the start and end time for the data
    end_time = datetime.utcnow()
    start_time = end_time - timedelta(days=days)

    all_klines = []  # List to accumulate data

    # Convert times to milliseconds
    start_ts = int(start_time.timestamp() * 1000)
    end_ts = int(end_time.timestamp() * 1000)

    # Define the interval in milliseconds
    interval_ms_dict = {
        Client.KLINE_INTERVAL_1MINUTE: 60 * 1000,
    }
    interval_ms = interval_ms_dict.get(interval, 60 * 1000)

    # Fetch data in batches
    while start_ts < end_ts:
        limit = 1000
        klines = client.futures_klines(
            symbol=symbol,
            interval=interval,
            startTime=start_ts,
            limit=limit
        )

        if not klines:
            break  # No more data available

        all_klines.extend(klines)
        last_kline_open_time = klines[-1][0]
        start_ts = last_kline_open_time + interval_ms

        time.sleep(0.1)  # Pause to avoid rate limiting

    # Define DataFrame columns as returned by Binance
    columns = ['Open time', 'Open', 'High', 'Low', 'Close', 'Volume', 'Close time',
               'Quote asset volume', 'Number of trades', 'Taker buy base asset volume',
               'Taker buy quote asset volume', 'Ignore']

    # Create DataFrame and convert timestamps
    df = pd.DataFrame(all_klines, columns=columns)
    df['Open time'] = pd.to_datetime(df['Open time'], unit='ms')
    df['Close time'] = pd.to_datetime(df['Close time'], unit='ms')

    # Select only the 'Open time', 'High', 'Low', and 'Close' columns
    df_filtered = df[['Open time', 'Open', 'High', 'Low', 'Close']].copy()

    # If multiply is True, multiply 'Close' by 1000 (adjust as needed)
    if multiply:
        df_filtered['Close'] = df_filtered['Close'].astype(float) * 1000

    # Ensure 'Close' is float and round to 5 decimal places
    df_filtered['Close'] = df_filtered['Close'].astype(float).round(5)

    # Save the DataFrame to a CSV file
    df_filtered.to_csv(output_file, index=False, float_format='%.5f')
    print(f"Data saved to {output_file}")

# Example usage
API_KEY = 'your_api_key'
API_SECRET = 'your_api_secret'
days = 5

# Define the coin symbols you want to fetch
coin_symbols = ["ETHUSDT"]

# Create tasks for parallel execution
tasks = [
    (
        symbol,
        Client.KLINE_INTERVAL_1MINUTE,
        days,
        API_KEY,
        API_SECRET,
        f'../input/{symbol}.csv',
        False  # Set to True if you want to multiply the 'Close' values by 1000
    )
    for symbol in coin_symbols
]

# Run tasks in parallel
with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
    futures = [executor.submit(fetch_and_save_klines, *task) for task in tasks]
    concurrent.futures.wait(futures)
