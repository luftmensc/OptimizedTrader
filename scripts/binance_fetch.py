"""
Fetch historical 1 minute candles (and funding rates) of Binance USDT-M perpetual futures.

Only public endpoints are used, so no API key is needed. Only the standard library of Python is used.

Usage (from the repository root):
    python3 scripts/binance_fetch.py                                  # BTC, ETH, SOL, BNB - last 730 days
    python3 scripts/binance_fetch.py --symbols BTCUSDT --days 30
    python3 scripts/binance_fetch.py --update                         # append new candles to existing files

Output:
    input/<SYMBOL>.csv            Open time,Open,High,Low,Close,Volume   (UTC, one row per minute, no gaps)
    input/funding/<SYMBOL>.csv    Funding time,Funding rate              (UTC)
"""
import argparse
import concurrent.futures
import csv
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

BASE_URL = "https://fapi.binance.com"
INTERVAL_MS = 60 * 1000
KLINES_LIMIT = 1000          # 1000 candles cost 5 weight, the cheapest per candle
FUNDING_LIMIT = 1000
WEIGHT_LIMIT_PER_MINUTE = 2400
WEIGHT_SAFETY_THRESHOLD = int(WEIGHT_LIMIT_PER_MINUTE * 0.75)
TIME_FORMAT = "%Y-%m-%d %H:%M:%S"

throttle_lock = threading.Lock()
resume_time = 0.0  # No thread sends a request before this time


def pause_requests(seconds):
    global resume_time
    with throttle_lock:
        resume_time = max(resume_time, time.time() + seconds)


def wait_for_permission():
    while True:
        with throttle_lock:
            wait = resume_time - time.time()
        if wait <= 0:
            return
        time.sleep(wait)


def format_ts(ts_ms):
    return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime(TIME_FORMAT)


def parse_ts(text):
    return int(datetime.strptime(text, TIME_FORMAT).replace(tzinfo=timezone.utc).timestamp() * 1000)


def get(path, params, retries=8):
    """GET with retries, which also respects the request weight limit shared by all threads."""
    for attempt in range(retries):
        wait_for_permission()
        url = BASE_URL + path + "?" + urllib.parse.urlencode(params)
        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                body = response.read()
                headers = response.headers
        except urllib.error.HTTPError as error:
            if error.code in (418, 429):
                # Too many requests
                pause_requests(int(error.headers.get("Retry-After", 60)))
                continue
            if error.code >= 500:
                time.sleep(min(2 ** attempt, 30))
                continue
            raise RuntimeError(f"Request failed with {error.code}: {path} {params} {error.read()[:200]}")
        except (urllib.error.URLError, OSError) as error:
            # No connection
            time.sleep(min(2 ** attempt, 30))
            continue

        used_weight = int(headers.get("x-mbx-used-weight-1m", 0))
        if used_weight > WEIGHT_SAFETY_THRESHOLD:
            # The weight counter resets at the start of each minute
            pause_requests(60 - time.time() % 60 + 1)
        return json.loads(body)
    raise RuntimeError(f"Request failed after {retries} retries: {path} {params}")


def read_last_row(path):
    """Returns (open time in ms, close) of the last complete row of an existing candle file, or None.

    The last row is removed from the file, because it can be incomplete after an interrupted
    download. It is fetched again.
    """
    if not os.path.exists(path):
        return None
    with open(path, "rb+") as file:
        file.seek(0, os.SEEK_END)
        size = file.tell()
        tail_start = max(0, size - 4096)
        file.seek(tail_start)
        tail = file.read()
        lines = tail.split(b"\n")
        # lines[-1] is empty if the file ends with a line break, lines[-2] is the last row
        if len(lines) < 4:
            return None
        removed = len(lines[-1]) + len(lines[-2]) + 1
        file.truncate(size - removed)
        last_row = lines[-3].decode().split(",")
    return parse_ts(last_row[0]), last_row[4]


def fetch_klines(symbol, start_ms, end_ms, output_file, append, previous_close=None, quiet=False):
    """Fetches closed candles with open time in [start_ms, end_ms) and writes them without gaps.

    previous_close is the close of the candle before start_ms, it is used if the first candles are missing.
    Returns the number of the written rows.
    """
    total_rows = 0
    filled_rows = 0
    expected_ts = start_ms

    with open(output_file, "a" if append else "w", newline="") as file:
        writer = csv.writer(file)
        if not append:
            writer.writerow(["Open time", "Open", "High", "Low", "Close", "Volume"])

        cursor = start_ms
        while cursor < end_ms:
            klines = get("/fapi/v1/klines", {
                "symbol": symbol,
                "interval": "1m",
                "startTime": cursor,
                "endTime": end_ms - 1,
                "limit": KLINES_LIMIT,
            })
            if not klines:
                break

            for kline in klines:
                open_time = int(kline[0])
                if open_time < expected_ts:
                    continue  # Duplicate candle
                # Fill the minutes the exchange did not report (outage) with flat candles
                while expected_ts < open_time and previous_close is not None:
                    writer.writerow([format_ts(expected_ts), previous_close, previous_close,
                                     previous_close, previous_close, "0"])
                    expected_ts += INTERVAL_MS
                    total_rows += 1
                    filled_rows += 1
                writer.writerow([format_ts(open_time), kline[1], kline[2], kline[3], kline[4], kline[5]])
                previous_close = kline[4]
                expected_ts = open_time + INTERVAL_MS
                total_rows += 1

            cursor = int(klines[-1][0]) + INTERVAL_MS

    if not quiet:
        print(f"{symbol}: {total_rows} candles written to {output_file} "
              f"({filled_rows} missing minutes filled)")
    return total_rows


def fetch_funding(symbol, start_ms, end_ms, output_file, quiet=False):
    rows = []
    cursor = start_ms
    while cursor < end_ms:
        funding = get("/fapi/v1/fundingRate", {
            "symbol": symbol,
            "startTime": cursor,
            "endTime": end_ms,
            "limit": FUNDING_LIMIT,
        })
        if not funding:
            break
        rows.extend(funding)
        cursor = int(funding[-1]["fundingTime"]) + 1
        if len(funding) < FUNDING_LIMIT:
            break

    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    # The file is replaced with one step, a reader never sees a half written file
    with open(output_file + ".tmp", "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["Funding time", "Funding rate"])
        for row in rows:
            writer.writerow([format_ts(int(row["fundingTime"])), row["fundingRate"]])
    os.replace(output_file + ".tmp", output_file)
    if not quiet:
        print(f"{symbol}: {len(rows)} funding rates written to {output_file}")
    return len(rows)


def fetch_symbol(symbol, days, output_dir, update):
    # Only closed candles: the end is the start of the current minute
    end_ms = int(time.time() * 1000) // INTERVAL_MS * INTERVAL_MS
    start_ms = end_ms - days * 24 * 60 * INTERVAL_MS

    candle_file = os.path.join(output_dir, f"{symbol}.csv")
    funding_file = os.path.join(output_dir, "funding", f"{symbol}.csv")

    append = False
    previous_close = None
    if update:
        last_row = read_last_row(candle_file)
        if last_row is not None:
            start_ms = last_row[0] + INTERVAL_MS
            previous_close = last_row[1]
            append = True

    fetch_klines(symbol, start_ms, end_ms, candle_file, append, previous_close)
    # Funding history is small, so it is always fetched for the whole period
    fetch_funding(symbol, end_ms - days * 24 * 60 * INTERVAL_MS, end_ms, funding_file)


def main():
    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    parser = argparse.ArgumentParser(description="Fetch Binance USDT-M futures 1m candles and funding rates.")
    parser.add_argument("--symbols", nargs="+", default=["BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT"])
    parser.add_argument("--days", type=int, default=730)
    parser.add_argument("--output-dir", default=os.path.join(repository_root, "input"))
    parser.add_argument("--update", action="store_true",
                        help="Append the candles after the last row of the existing files")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        futures = [executor.submit(fetch_symbol, symbol, args.days, args.output_dir, args.update)
                   for symbol in args.symbols]
        for future in concurrent.futures.as_completed(futures):
            future.result()  # Raise the exceptions of the worker threads


if __name__ == "__main__":
    main()
