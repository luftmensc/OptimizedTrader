"""
Runs the backtest for a list of strategy variants and prints a comparison of the portfolio results.

Usage (from the repository root, after ./buildrun.sh or a cmake build):
    python3 scripts/research.py variants.json --end 2026-04-01
    python3 scripts/research.py variants.json --start 2026-04-01 --only trend_4h

variants.json is an object: {"name": {"option": value, ...}, ...}. The options are the options
of build/bin/backtest without the leading "--", for example {"timeframe": 240, "channels": "20,40"}.
"""
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys

COLUMNS = [
    ("total_return", "Return", "pct"),
    ("cagr", "CAGR", "pct"),
    ("sharpe", "Sharpe", "num"),
    ("sortino", "Sortino", "num"),
    ("calmar", "Calmar", "num"),
    ("max_drawdown", "MaxDD", "pct"),
    ("trades", "Trades", "int"),
    ("win_rate", "Win", "pct"),
    ("profit_factor", "PF", "num"),
    ("expectancy_r", "Exp(R)", "num3"),
    ("positive_months", "Months+", "pct0"),
    ("exposure", "Expo", "pct0"),
]


def format_value(value, kind):
    if kind == "pct":
        return f"{value * 100:.1f}%"
    if kind == "pct0":
        return f"{value * 100:.0f}%"
    if kind == "int":
        return str(int(value))
    if kind == "num3":
        return f"{value:.3f}"
    return f"{value:.2f}"


def run_variant(name, options, args):
    command = [args.binary, "--quiet", "--tag", name, "--output-dir", args.output_dir,
               "--input-dir", args.input_dir]
    if args.start:
        command += ["--start", args.start]
    if args.end:
        command += ["--end", args.end]
    if args.symbols:
        command += ["--symbols", args.symbols]
    for option, value in options.items():
        command += [f"--{option}", str(value)]

    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        return name, None, completed.stderr.strip()
    with open(os.path.join(args.output_dir, f"{name}_summary.json")) as file:
        return name, json.load(file), completed.stderr.strip()


def main():
    repository_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    parser = argparse.ArgumentParser(description="Compare strategy variants.")
    parser.add_argument("variants", help="Json file with the variants")
    parser.add_argument("--binary", default=os.path.join(repository_root, "build", "bin", "backtest"))
    parser.add_argument("--input-dir", default=os.path.join(repository_root, "input"))
    parser.add_argument("--output-dir", default=os.path.join(repository_root, "output", "research"))
    parser.add_argument("--start", default="")
    parser.add_argument("--end", default="")
    parser.add_argument("--symbols", default="")
    parser.add_argument("--only", nargs="*", help="Names of the variants to run")
    parser.add_argument("--per-symbol", action="store_true", help="Also print the return of each symbol")
    parser.add_argument("--sort", default="", help="Column to sort by, for example sortino")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()

    with open(args.variants) as file:
        variants = json.load(file)
    if args.only:
        variants = {name: variants[name] for name in args.only}

    os.makedirs(args.output_dir, exist_ok=True)

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run_variant, name, options, args) for name, options in variants.items()]
        for future in concurrent.futures.as_completed(futures):
            name, summary, errors = future.result()
            if summary is None:
                print(f"{name} failed: {errors}", file=sys.stderr)
            else:
                results[name] = summary

    names = [name for name in variants if name in results]
    if args.sort:
        names.sort(key=lambda name: results[name]["portfolio"][args.sort], reverse=True)
    if not names:
        return

    period = results[names[0]]["period"]
    hold = results[names[0]]["buy_and_hold"]
    print(f"Period: {period['start']} - {period['end']}")
    print(f"Buy and hold of the symbols: return {hold['total_return'] * 100:.1f}%, "
          f"sharpe {hold['sharpe']:.2f}, sortino {hold['sortino']:.2f}, "
          f"max drawdown {hold['max_drawdown'] * 100:.1f}%\n")

    name_width = max(len(name) for name in names) + 2
    header = "".join(f"{title:>9}" for _, title, _ in COLUMNS)
    symbols = list(results[names[0]]["symbols"].keys()) if args.per_symbol else []
    header += "".join(f"{symbol[:-4]:>9}" for symbol in symbols)
    print(f"{'':<{name_width}}{header}")

    for name in names:
        portfolio = results[name]["portfolio"]
        row = "".join(f"{format_value(portfolio[key], kind):>9}" for key, _, kind in COLUMNS)
        row += "".join(f"{results[name]['symbols'][symbol]['total_return'] * 100:>8.1f}%" for symbol in symbols)
        print(f"{name:<{name_width}}{row}")


if __name__ == "__main__":
    main()
