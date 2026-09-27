"""
Creates an interactive candlestick chart (html file) with the trades of a backtest:
entry and exit points, stop loss (also the moves of a trailing stop), take profit and the broken channel level.

Usage (from the repository root, after running build/bin/backtest):
    python3 scripts/plot_trades.py                                   # BTCUSDT of the tag breakout_4h
    python3 scripts/plot_trades.py --symbol ETHUSDT --tag backtest
    python3 scripts/plot_trades.py --timeframes 240,60,15,5 --open   # More zoom levels, open the browser

Output:
    output/<tag>_<SYMBOL>_chart.html    Open it with a browser.

Mouse: drag = move, wheel = zoom, click on a trade (chart or list) = details. Keys: left / right = previous / next trade.
"""
import argparse
import calendar
import csv
import json
import os
import sys
import time
import urllib.request
import webbrowser

LIBRARY_VERSION = "4.2.3"
LIBRARY_URL = (f"https://cdn.jsdelivr.net/npm/lightweight-charts@{LIBRARY_VERSION}"
               "/dist/lightweight-charts.standalone.production.js")
TIME_FORMAT = "%Y-%m-%d %H:%M:%S"
WARMUP_DAYS = 30  # Candles before the first trading day which are also drawn


def parse_time(text):
    return calendar.timegm(time.strptime(text, TIME_FORMAT))


def read_candles(path):
    """Returns the 1 minute candles as lists: time, open, high, low, close."""
    times, opens, highs, lows, closes = [], [], [], [], []
    with open(path) as file:
        next(file)  # Skip the header line
        previous = ""
        day_start = 0
        for line in file:
            parts = line.split(",")
            if len(parts) < 5:
                continue
            # Parsing every time is slow, only the day is parsed
            day = parts[0][:10]
            if day != previous:
                day_start = parse_time(day + " 00:00:00")
                previous = day
            text = parts[0]
            times.append(day_start + int(text[11:13]) * 3600 + int(text[14:16]) * 60)
            opens.append(float(parts[1]))
            highs.append(float(parts[2]))
            lows.append(float(parts[3]))
            closes.append(float(parts[4]))
    return times, opens, highs, lows, closes


def aggregate(candles, minutes):
    """Returns the complete bars of the time frame as list of [time, open, high, low, close]."""
    times, opens, highs, lows, closes = candles
    seconds = minutes * 60
    bars = []
    i = 0
    count = len(times)
    while i < count and times[i] % seconds != 0:
        i += 1
    while i + minutes <= count:
        end = i + minutes
        bars.append([times[i], opens[i], max(highs[i:end]), min(lows[i:end]), closes[end - 1]])
        i = end
    return bars


def calculate_ema(bars, length):
    """Same calculation as the backtest: [time, value] of the bars which have enough history."""
    values = []
    alpha = 2.0 / (length + 1.0)
    value = 0.0
    for index, bar in enumerate(bars):
        value = bar[4] if index == 0 else value + alpha * (bar[4] - value)
        if index + 1 >= length:
            values.append([bar[0], value])
    return values


def read_trades(path, symbol):
    trades = []
    with open(path) as file:
        for row in csv.DictReader(file):
            if row["Symbol"] != symbol:
                continue
            if "Initial stop price" not in row:
                sys.exit(f"Error: {path} is from an old version of the backtest, run the backtest again.")
            trades.append({
                "side": row["Side"],
                "entryTime": parse_time(row["Entry time"]),
                "exitTime": parse_time(row["Exit time"]),
                "entryPrice": float(row["Entry price"]),
                "exitPrice": float(row["Exit price"]),
                "quantity": float(row["Quantity"]),
                "gross": float(row["Gross"]),
                "fees": float(row["Fees"]),
                "funding": float(row["Funding"]),
                "net": float(row["Net"]),
                "return": float(row["Return"]),
                "r": float(row["R"]),
                "reason": row["Exit reason"],
                "channel": int(row["Channel"]),
                "stopValue": float(row["Stop"]),
                "initialStop": float(row["Initial stop price"]),
                "takeProfit": float(row["Take profit price"]),
                "stops": [],
                "level": None,
            })
    return trades


def read_stops(path, symbol, trades):
    by_entry = {trade["entryTime"]: trade for trade in trades}
    if not os.path.exists(path):
        return
    with open(path) as file:
        for row in csv.DictReader(file):
            if row["Symbol"] != symbol:
                continue
            trade = by_entry.get(parse_time(row["Entry time"]))
            if trade is not None:
                trade["stops"].append([parse_time(row["Time"]), float(row["Stop price"])])


def add_channel_levels(trades, strategy_bars, minutes):
    """The level which was broken by the close of the signal bar, and the first bar of the channel."""
    index_of_time = {bar[0]: index for index, bar in enumerate(strategy_bars)}
    for trade in trades:
        # The position is opened on the first candle after the signal bar
        signal = index_of_time.get(trade["entryTime"] - minutes * 60)
        length = trade["channel"]
        if signal is None or signal < length:
            continue
        closes = [bar[4] for bar in strategy_bars[signal - length:signal]]
        trade["level"] = {
            "from": strategy_bars[signal - length][0],
            "price": max(closes) if trade["side"] == "Long" else min(closes),
        }


def load_library(cache_dir):
    """Returns the script tag of the chart library. The library is put inside of the html file,
    so that the chart also works without internet. Without a download the CDN is used."""
    cache_file = os.path.join(cache_dir, f".lightweight-charts-{LIBRARY_VERSION}.js")
    source = None
    if os.path.exists(cache_file):
        with open(cache_file) as file:
            source = file.read()
    else:
        try:
            with urllib.request.urlopen(LIBRARY_URL, timeout=30) as response:
                source = response.read().decode()
            with open(cache_file, "w") as file:
                file.write(source)
        except Exception as error:
            print(f"Warning: Could not download the chart library ({error}). "
                  "The chart needs internet when it is opened.")
    if source is None:
        return f'<script src="{LIBRARY_URL}"></script>'
    return "<script>" + source.replace("</script", "<\\/script") + "</script>"


def round_price(value):
    return float(f"{value:.8g}")


def main():
    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    parser = argparse.ArgumentParser(description="Interactive chart of the trades of a backtest.")
    parser.add_argument("--symbol", default="BTCUSDT")
    parser.add_argument("--tag", default="breakout_4h", help="Tag of the backtest (prefix of the output files)")
    parser.add_argument("--input-dir", default=os.path.join(repository_root, "input"))
    parser.add_argument("--output-dir", default=os.path.join(repository_root, "output"))
    parser.add_argument("--timeframes", default="",
                        help="Minutes of the bars which can be chosen in the chart (default: the time frame "
                             "of the strategy, 60 and 15). Small time frames make a big file.")
    parser.add_argument("--open", action="store_true", help="Open the chart with the browser")
    args = parser.parse_args()

    prefix = os.path.join(args.output_dir, args.tag)
    for path in (prefix + "_summary.json", prefix + "_trades.csv"):
        if not os.path.exists(path):
            sys.exit(f"Error: {path} does not exist. Run the backtest first: build/bin/backtest --tag {args.tag}")

    with open(prefix + "_summary.json") as file:
        summary = json.load(file)
    if "config" not in summary:
        sys.exit(f"Error: {prefix}_summary.json is from an old version of the backtest, run the backtest again.")
    if args.symbol not in summary["symbols"]:
        sys.exit(f"Error: {args.symbol} is not a symbol of the backtest {args.tag}.")

    config = summary["config"]
    strategy_minutes = config["timeframe_minutes"]
    if args.timeframes:
        timeframes = sorted({int(value) for value in args.timeframes.split(",")}, reverse=True)
    else:
        timeframes = sorted({strategy_minutes, 60, 15}, reverse=True)

    print(f"Reading the candles of {args.symbol}...")
    candles = read_candles(os.path.join(args.input_dir, f"{args.symbol}.csv"))
    if not candles[0]:
        sys.exit("Error: No candles found.")

    trades = read_trades(prefix + "_trades.csv", args.symbol)
    read_stops(prefix + "_stops.csv", args.symbol, trades)

    strategy_bars = aggregate(candles, strategy_minutes)
    add_channel_levels(trades, strategy_bars, strategy_minutes)

    first_time = parse_time(summary["period"]["start"]) - WARMUP_DAYS * 86400
    last_time = parse_time(summary["period"]["end"]) + 3600

    bars = {}
    for minutes in timeframes:
        bars[str(minutes)] = [
            [bar[0]] + [round_price(value) for value in bar[1:]]
            for bar in aggregate(candles, minutes)
            if first_time <= bar[0] < last_time
        ]

    ema = []
    if config["trend_ema_length"] > 0:
        ema = [[point[0], round_price(point[1])]
               for point in calculate_ema(strategy_bars, config["trend_ema_length"])
               if first_time <= point[0] < last_time]

    data = {
        "symbol": args.symbol,
        "tag": args.tag,
        "config": config,
        "period": summary["period"],
        "report": summary["symbols"][args.symbol],
        "timeframes": timeframes,
        "strategyMinutes": strategy_minutes,
        "bars": bars,
        "ema": ema,
        "trades": trades,
    }

    template_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "plot_trades_template.html")
    with open(template_file) as file:
        html = file.read()
    html = html.replace("<!--LIBRARY-->", load_library(args.output_dir))
    html = html.replace("/*DATA*/null", json.dumps(data, separators=(",", ":")).replace("</", "<\\/"))
    html = html.replace("<!--TITLE-->", f"{args.symbol} trades - {args.tag}")

    output_file = f"{prefix}_{args.symbol}_chart.html"
    with open(output_file, "w") as file:
        file.write(html)

    size = os.path.getsize(output_file) / 1e6
    print(f"{len(trades)} trades, time frames {timeframes} minutes")
    print(f"Chart saved to {output_file} ({size:.1f} MB). Open it with a browser.")

    if args.open:
        webbrowser.open("file://" + os.path.abspath(output_file))


if __name__ == "__main__":
    main()
