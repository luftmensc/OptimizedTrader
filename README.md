# Optimization Based Backtesting with Modern C++

Gathering data from Binance and backtesting trading strategies with C++.

## Getting Started

Only thing that needed is C++20 and Python installed on your machine.,

## Before Execute

Make sure that you have coin data as csv with following format


| Open time          |Open |High |Low  |Close|
|-|-|-|-|-|
| 2024-11-18 11:30:00 |4.348|4.502|3.342|3.941|
| 2024-11-18 11:31:00 |3.934|4.0  |3.82 |3.867|
| 2024-11-18 11:32:00 |3.866|3.871|3.757|3.807|

Then, modify example.cpp main function with giving the path of the csv data.

### Executing the program

If everything is ready, run the program with `./buildrun.sh`

### What to Expect

With rolling window optimization technique, a csv file as an output that includes coin candle data and balance will be placed at csv_output/ folder. It can be visualized by visualize.py

## Authors

Luftmenschh\
[@Github](https://github.com/luftmensc)\
[@LinkedIn](https://www.linkedin.com/in/omer-faruk-okuyan/)
