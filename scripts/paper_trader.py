"""
Paper trading: runs the strategy of the backtest on the live market data of Binance with fake money.

Every minute the new closed candles are downloaded and the backtest program is executed in the live mode
from the start of the paper trading until now. The positions, stops and the balance are always calculated
from the data, so a restart or a crash does not lose anything and the decisions are the same as in a backtest.
No order is sent to the exchange and no API key is used.

Usage (from the repository root, the backtest program must be built):
    python3 scripts/paper_trader.py init                      # Create paper/, download the history
    python3 scripts/paper_trader.py init --capital 5000 --option risk=0.02
    python3 scripts/paper_trader.py run                       # Run until it is stopped (see deploy/)
    python3 scripts/paper_trader.py status                    # Positions, balance and the last events
    python3 scripts/paper_trader.py once                      # One cycle, for example for cron

Files in paper/:
    config.json          Settings, written by init
    data/                Candles and funding rates
    output/paper_*       Output of the backtest program: state, trades, stops, equity, summary
    events.csv           Every entry, stop move and exit with the time when it happened
    paper.log            Log of the program

Chart of the trades:
    python3 scripts/plot_trades.py --tag paper --input-dir paper/data --output-dir paper/output
"""
import argparse
import concurrent.futures
import csv
import fcntl
import json
import logging
import logging.handlers
import os
import signal
import subprocess
import sys
import time

import binance_fetch

INTERVAL_MS = binance_fetch.INTERVAL_MS
TAG = "paper"
# Options of the backtest program which are set by this program
RESERVED_OPTIONS = {"live", "start", "end", "tag", "input-dir", "output-dir", "symbols", "capital", "quiet"}
EVENT_COLUMNS = ["Time", "Symbol", "Event", "Side", "Price", "Quantity", "Stop", "Net", "R", "Equity"]

log = logging.getLogger("paper")
stop_requested = False


class Paths:
    def __init__(self, directory):
        self.directory = directory
        self.config = os.path.join(directory, "config.json")
        self.data = os.path.join(directory, "data")
        self.output = os.path.join(directory, "output")
        self.known = os.path.join(directory, "known.json")
        self.events = os.path.join(directory, "events.csv")
        self.log = os.path.join(directory, "paper.log")
        self.lock = os.path.join(directory, ".lock")

    def candles(self, symbol):
        return os.path.join(self.data, f"{symbol}.csv")

    def funding(self, symbol):
        return os.path.join(self.data, "funding", f"{symbol}.csv")

    def result(self, name):
        return os.path.join(self.output, f"{TAG}_{name}")


def write_json(path, value):
    # The file is replaced with one step, a reader never sees a half written file
    with open(path + ".tmp", "w") as file:
        json.dump(value, file, indent=2)
    os.replace(path + ".tmp", path)


def read_json(path, default=None):
    if not os.path.exists(path):
        return default
    with open(path) as file:
        return json.load(file)


def load_config(paths):
    config = read_json(paths.config)
    if config is None:
        sys.exit(f"Error: {paths.config} does not exist. Run first: python3 scripts/paper_trader.py init")
    return config


def setup_logging(paths, to_file):
    log.setLevel(logging.INFO)
    formatter = logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    formatter.converter = time.gmtime  # All the times are UTC
    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(formatter)
    log.addHandler(console)
    if to_file:
        handler = logging.handlers.RotatingFileHandler(paths.log, maxBytes=2_000_000, backupCount=3)
        handler.setFormatter(formatter)
        log.addHandler(handler)


# ---------- Market data ----------
def closed_candles_end():
    """Open time (ms) of the candle which is not closed yet, from the clock of the exchange."""
    server_time = binance_fetch.get("/fapi/v1/time", {})["serverTime"]
    # 2 seconds for the exchange to finish the candle
    return (server_time - 2000) // INTERVAL_MS * INTERVAL_MS


def update_candles(paths, config, end_ms, repair):
    """Appends the new closed candles. repair removes a last row which is not complete (after a crash)."""
    added = 0
    for symbol in config["symbols"]:
        path = paths.candles(symbol)
        last_row = binance_fetch.read_last_row(path) if repair else read_last_row_only(path)
        if last_row is None:
            raise RuntimeError(f"{path} has no candles, run init again")
        start_ms = last_row[0] + INTERVAL_MS
        if start_ms < end_ms:
            added += binance_fetch.fetch_klines(symbol, start_ms, end_ms, path, True, last_row[1], quiet=True)
    return added


def read_last_row_only(path):
    """(open time in ms, close) of the last row, the file is not changed."""
    if not os.path.exists(path):
        return None
    with open(path, "rb") as file:
        file.seek(0, os.SEEK_END)
        size = file.tell()
        file.seek(max(0, size - 4096))
        lines = file.read().split(b"\n")
    if len(lines) < 3 or lines[-1] != b"":
        # The last row is not complete, the next cycle repairs it
        raise RuntimeError(f"{path} does not end with a complete row")
    last_row = lines[-2].decode().split(",")
    return binance_fetch.parse_ts(last_row[0]), last_row[4]


def first_candle_time(path):
    with open(path) as file:
        file.readline()
        return binance_fetch.parse_ts(file.readline().split(",")[0])


def update_funding(paths, config, end_ms):
    for symbol in config["symbols"]:
        start_ms = first_candle_time(paths.candles(symbol))
        binance_fetch.fetch_funding(symbol, start_ms, end_ms, paths.funding(symbol), quiet=True)


# ---------- Engine ----------
def run_engine(paths, config, binary):
    command = [binary, "--live", "1", "--quiet", "--tag", TAG,
               "--input-dir", paths.data, "--output-dir", paths.output,
               "--symbols", ",".join(config["symbols"]),
               "--start", config["start"], "--capital", str(config["capital"])]
    for option, value in config.get("options", {}).items():
        command += [f"--{option}", str(value)]

    completed = subprocess.run(command, capture_output=True, text=True, timeout=300)
    warnings = completed.stderr.strip()
    if completed.returncode != 0:
        raise RuntimeError(f"backtest program failed ({completed.returncode}): {warnings}")
    if warnings:
        log.warning("backtest program: %s", warnings)
    return read_json(paths.result("state.json"))


def read_trades(paths):
    stops = {}
    with open(paths.result("stops.csv")) as file:
        for row in csv.DictReader(file):
            stops[(row["Symbol"], row["Entry time"])] = row
    with open(paths.result("trades.csv")) as file:
        trades = list(csv.DictReader(file))
    for trade in trades:
        # The last row of a trade is its current stop
        stop = stops.get((trade["Symbol"], trade["Entry time"]))
        trade["Stop price"] = stop["Stop price"] if stop else trade["Initial stop price"]
        trade["Stop time"] = stop["Time"] if stop else trade["Entry time"]
    return trades


# ---------- Events ----------
def write_event(paths, state, time_text, trade, event, price, net="", r=""):
    is_new = not os.path.exists(paths.events)
    with open(paths.events, "a", newline="") as file:
        writer = csv.writer(file)
        if is_new:
            writer.writerow(EVENT_COLUMNS)
        writer.writerow([time_text, trade["Symbol"], event, trade["Side"], price, trade["Quantity"],
                         trade["Stop price"], net, r, f"{state['equity']:.2f}"])


def process_events(paths, state, announce):
    """Compares the trades with the trades of the last cycle and writes what is new.

    announce is False for the first cycle after init: trades of the history are saved without events.
    """
    known = read_json(paths.known, {"closed": [], "open": {}})
    closed = set(known["closed"])
    was_open = known["open"]
    now_open = {}
    trades = read_trades(paths)
    seen = set()

    for trade in trades:
        symbol = trade["Symbol"]
        key = f"{symbol}|{trade['Entry time']}"
        seen.add(key)
        previous = was_open.get(symbol)
        is_known_entry = previous is not None and previous["entry_time"] == trade["Entry time"]
        value = float(trade["Quantity"]) * float(trade["Entry price"])

        if trade["Exit reason"] == "Open":
            now_open[symbol] = {"entry_time": trade["Entry time"], "stop": trade["Stop price"]}
            if not announce:
                continue
            if not is_known_entry:
                log.info("ENTRY %s %s %s @ %s (%.0f USDT), stop loss %s",
                         symbol, trade["Side"], trade["Quantity"], trade["Entry price"], value,
                         trade["Stop price"])
                write_event(paths, state, trade["Entry time"], trade, "ENTRY", trade["Entry price"])
            elif previous["stop"] != trade["Stop price"]:
                log.info("STOP MOVED %s %s: %s -> %s", symbol, trade["Side"], previous["stop"],
                         trade["Stop price"])
                write_event(paths, state, trade["Stop time"], trade, "STOP_MOVED", trade["Stop price"])
        elif key not in closed:
            closed.add(key)
            if not announce:
                continue
            if not is_known_entry:
                # Entry and exit between two cycles
                log.info("ENTRY %s %s %s @ %s (%.0f USDT)", symbol, trade["Side"], trade["Quantity"],
                         trade["Entry price"], value)
                write_event(paths, state, trade["Entry time"], trade, "ENTRY", trade["Entry price"])
            log.info("EXIT %s %s @ %s (%s): net %+.2f USDT, %+.2f R", symbol, trade["Side"],
                     trade["Exit price"], trade["Exit reason"], float(trade["Net"]), float(trade["R"]))
            write_event(paths, state, trade["Exit time"], trade, "EXIT_" + trade["Exit reason"],
                        trade["Exit price"], trade["Net"], trade["R"])

    # The history must never change: everything which was reported must still be there
    for symbol, previous in was_open.items():
        if f"{symbol}|{previous['entry_time']}" not in seen:
            log.error("INCONSISTENT: the position of %s from %s is not in the result anymore",
                      symbol, previous["entry_time"])
    for key in known["closed"]:
        if key not in seen:
            log.error("INCONSISTENT: the closed trade %s is not in the result anymore", key)

    write_json(paths.known, {"closed": sorted(closed), "open": now_open})


# ---------- Commands ----------
class Trader:
    def __init__(self, paths, config, binary):
        self.paths = paths
        self.config = config
        self.binary = binary
        self.first_cycle = True
        self.funding_slot = None

    def cycle(self):
        end_ms = closed_candles_end()
        added = update_candles(self.paths, self.config, end_ms, repair=self.first_cycle)

        # Funding is paid on full hours. It is downloaded right after every full hour and one more
        # time 5 minutes later, if the exchange was late.
        minute = end_ms // INTERVAL_MS
        slot = (minute // 60, minute % 60 >= 5)
        if slot != self.funding_slot:
            update_funding(self.paths, self.config, end_ms)
            self.funding_slot = slot

        if added == 0 and not self.first_cycle:
            return

        state = run_engine(self.paths, self.config, self.binary)
        announce = os.path.exists(self.paths.known)
        process_events(self.paths, state, announce)
        if self.first_cycle:
            positions = [symbol for symbol, value in state["symbols"].items() if value["position"]]
            log.info("Data until %s, equity %.2f, open positions: %s", state["time"], state["equity"],
                     ", ".join(positions) if positions else "none")
        self.first_cycle = False


def lock_directory(paths):
    """Only one program can trade in a directory."""
    lock = open(paths.lock, "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit(f"Error: Another paper trader is running in {paths.directory}")
    return lock


def command_init(args, paths, binary):
    if os.path.exists(paths.config) and not args.force:
        sys.exit(f"Error: {paths.config} exists already. A new init deletes the paper trading history, "
                 "use --force if this is what you want.")
    if not os.path.exists(binary):
        sys.exit(f"Error: {binary} does not exist, build it first (see README).")

    options = {}
    for text in args.option or []:
        name, separator, value = text.partition("=")
        name = name.lstrip("-")
        if not separator or name in RESERVED_OPTIONS:
            sys.exit(f"Error: Invalid option {text}. Use NAME=VALUE with an option of build/bin/backtest --help")
        options[name] = value

    lookback_days = int(options.get("lookback-days", 90))
    # Days for the indicators before the first lookback day (200 bars of 4 hours are 34 days)
    minimum_history = lookback_days + 45
    if args.history_days < minimum_history:
        sys.exit(f"Error: --history-days must be at least {minimum_history} for a lookback of {lookback_days} days.")

    now_ms = int(time.time() * 1000)
    if args.start:
        start_ms = binance_fetch.parse_ts(args.start if len(args.start) > 10 else args.start + " 00:00:00")
    else:
        # The next full hour
        start_ms = (now_ms // 3_600_000 + 1) * 3_600_000

    os.makedirs(paths.directory, exist_ok=True)
    lock = lock_directory(paths)
    setup_logging(paths, to_file=True)

    # Remove the results of an old run
    for path in (paths.known, paths.events):
        if os.path.exists(path):
            os.remove(path)
    os.makedirs(paths.output, exist_ok=True)
    for name in os.listdir(paths.output):
        if name.startswith(TAG + "_"):
            os.remove(os.path.join(paths.output, name))

    config = {
        "symbols": args.symbols,
        "capital": args.capital,
        "start": binance_fetch.format_ts(start_ms),
        "history_days": args.history_days,
        "options": options,
    }

    log.info("Downloading %d days of history for %s", args.history_days, ", ".join(args.symbols))
    os.makedirs(os.path.join(paths.data, "funding"), exist_ok=True)
    end_ms = closed_candles_end()
    # The history starts on a full day. It must never change later, the indicators depend on it.
    history_start = (min(start_ms, end_ms) // 86_400_000 - args.history_days) * 86_400_000

    def download(symbol):
        binance_fetch.fetch_klines(symbol, history_start, end_ms, paths.candles(symbol), False, quiet=True)
        log.info("%s: history downloaded", symbol)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        for future in [executor.submit(download, symbol) for symbol in args.symbols]:
            future.result()

    write_json(paths.config, config)
    trader = Trader(paths, config, binary)
    trader.cycle()
    log.info("Paper trading starts at %s UTC with %.2f USDT. Now start: python3 scripts/paper_trader.py run",
             config["start"], config["capital"])
    lock.close()


def command_run(args, paths, binary, once):
    config = load_config(paths)
    lock = lock_directory(paths)
    setup_logging(paths, to_file=True)

    def request_stop(signal_number, frame):
        global stop_requested
        stop_requested = True

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    try:
        os.nice(10)  # Other programs of the computer are more important
    except OSError:
        pass

    trader = Trader(paths, config, binary)
    log.info("Paper trader started: %s, start %s, capital %.2f", ", ".join(config["symbols"]),
             config["start"], config["capital"])

    failures = 0
    while not stop_requested:
        try:
            trader.cycle()
            if failures > 0:
                log.info("Working again after %d failed cycles", failures)
            failures = 0
        except Exception as error:
            if stop_requested:
                break  # The cycle was interrupted by the stop
            failures += 1
            # One line for an error which repeats, for example without internet
            if failures <= 3 or failures % 30 == 0:
                log.error("Cycle failed (%d): %s", failures, error)
            trader.first_cycle = True  # Check the files again
        if once:
            break

        # The next cycle starts 4 seconds after the next full minute
        wait = 60 - time.time() % 60 + 4
        while wait > 0 and not stop_requested:
            time.sleep(min(wait, 1))
            wait -= 1

    log.info("Paper trader stopped")
    lock.close()
    return 1 if (once and failures > 0) else 0


def format_price(value):
    return f"{value:,.2f}" if value >= 100 else f"{value:,.4f}"


def command_status(args, paths):
    config = load_config(paths)
    state = read_json(paths.result("state.json"))
    if state is None:
        sys.exit("No result yet.")

    age = time.time() - binance_fetch.parse_ts(state["time"]) / 1000
    print(f"Paper trading since {config['start']} UTC, data until {state['time']} UTC")
    if age > 300:
        print(f"WARNING: The data is {age / 60:.0f} minutes old, the paper trader is not running.")

    profit = state["equity"] - state["capital"]
    print(f"Equity {state['equity']:,.2f} USDT ({profit:+,.2f}, {profit / state['capital'] * 100:+.2f}%), "
          f"capital {state['capital']:,.2f}\n")

    print(f"{'Symbol':<10}{'Price':>12}{'Equity':>11}{'Trades':>8}  Position")
    for symbol, value in state["symbols"].items():
        position = value["position"]
        if position:
            text = (f"{position['side']} {position['quantity']:.5f} @ {format_price(position['entry_price'])} "
                    f"since {position['entry_time']}, stop {format_price(position['stop_price'])}, "
                    f"now {position['unrealized']:+.2f} USDT ({position['r']:+.2f} R)")
        elif value["levels"]:
            levels = value["levels"]
            parts = []
            if levels["long_above"] is not None:
                parts.append(f"long above {format_price(levels['long_above'])}")
            if levels["short_below"] is not None:
                parts.append(f"short below {format_price(levels['short_below'])}")
            text = f"flat, at the bar close {levels['bar_close'][5:16]}: " + ", ".join(parts)
        else:
            text = "flat"
        print(f"{symbol:<10}{format_price(value['last_price']):>12}{value['equity']:>11,.2f}"
              f"{value['closed_trades']:>8}  {text}")

    summary = read_json(paths.result("summary.json"))
    if summary and summary["portfolio"]["days"] >= 30:
        report = summary["portfolio"]
        print(f"\n{report['days']:.0f} days: sharpe {report['sharpe']:.2f}, sortino {report['sortino']:.2f}, "
              f"max drawdown {report['max_drawdown'] * 100:.1f}%, win rate {report['win_rate'] * 100:.0f}%, "
              f"profit factor {report['profit_factor']:.2f}")

    if os.path.exists(paths.events):
        with open(paths.events) as file:
            events = list(csv.DictReader(file))
        print(f"\nLast events ({len(events)} total):")
        for event in events[-args.events:]:
            result = f", net {float(event['Net']):+.2f} USDT ({float(event['R']):+.2f} R)" if event["Net"] else ""
            print(f"  {event['Time']}  {event['Symbol']:<9} {event['Event']:<18} {event['Side']:<5} "
                  f"@ {event['Price']}{result}")
    else:
        print("\nNo events yet.")


def main():
    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    parser = argparse.ArgumentParser(description="Paper trading of the breakout strategy.")
    parser.add_argument("--dir", default=os.path.join(repository_root, "paper"), help="Paper trading directory")
    parser.add_argument("--binary", default=os.path.join(repository_root, "build", "bin", "backtest"))
    commands = parser.add_subparsers(dest="command", required=True)

    init = commands.add_parser("init", help="Create the directory and download the history")
    init.add_argument("--symbols", nargs="+", default=["BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT"])
    init.add_argument("--capital", type=float, default=10000.0)
    init.add_argument("--start", default="", help='Start of the trading in UTC, "YYYY-MM-DD HH:MM:SS" '
                                                  "(default: the next full hour)")
    init.add_argument("--history-days", type=int, default=180)
    init.add_argument("--option", action="append",
                      help="Option of the backtest program as NAME=VALUE, for example risk=0.02")
    init.add_argument("--force", action="store_true", help="Delete an existing paper trading history")

    commands.add_parser("run", help="Trade until the program is stopped")
    commands.add_parser("once", help="One cycle")
    status = commands.add_parser("status", help="Show the positions and the last events")
    status.add_argument("--events", type=int, default=10, help="Number of events to show")

    args = parser.parse_args()
    paths = Paths(os.path.abspath(args.dir))
    binary = os.path.abspath(args.binary)

    if args.command == "init":
        command_init(args, paths, binary)
    elif args.command == "status":
        command_status(args, paths)
    else:
        sys.exit(command_run(args, paths, binary, once=args.command == "once"))


if __name__ == "__main__":
    main()
