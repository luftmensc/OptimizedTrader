import os
import pandas as pd
import matplotlib.pyplot as plt

def visualize_all_trading_logs(output_dir):
    # Find CSV files that start with "all_trading_logs_"
    csv_files = [f for f in os.listdir(output_dir) if f.startswith("all_trading_logs_") and f.endswith(".csv")]
    
    if not csv_files:
        print("No trading log CSV files found in", output_dir)
        return

    # Process each CSV file
    for csv_file in csv_files:
        file_path = os.path.join(output_dir, csv_file)
        try:
            data = pd.read_csv(file_path)
        except Exception as e:
            print(f"Error reading {file_path}: {e}")
            continue

        # Convert 'Open time' column to datetime
        try:
            data['Open time'] = pd.to_datetime(data['Open time'])
        except Exception as e:
            print(f"Error parsing dates in {file_path}: {e}")
            continue

        # Optionally filter out rows if you only want to plot when the balance changes:
        # filtered_data = data[data['Balance'].diff() != 0]
        filtered_data = data  # plotting all rows

        # Extract header for the figure by removing the prefix and suffix
        header = csv_file.replace("all_trading_logs_", "").replace(".csv", "")
        
        # Create and display the plot
        plt.figure(figsize=(12, 6))
        plt.plot(filtered_data['Open time'], filtered_data['Balance'], label=header)
        
        # Add horizontal dashed line at the starting point (balance=1000)
        plt.axhline(y=1000, linestyle='--', color='blue', label='Starting Point (1000)')
        
        # Add horizontal dashed line at the ending balance (last value in 'Balance' column)
        ending_balance = filtered_data['Balance'].iloc[-1]
        plt.axhline(y=ending_balance, linestyle='--', color='red', label=f'Ending Point ({ending_balance})')
        
        plt.title(header)
        plt.xlabel('Open time')
        plt.ylabel('Balance')
        plt.xticks(rotation=45)
        plt.legend()
        plt.tight_layout()
        
        # Display the plot on the screen
        plt.show()
        plt.close()
        print(f"Displayed plot for {header}")

def visualize_backtest_equity(output_dir):
    # Find the equity files of the backtest program
    csv_files = [f for f in os.listdir(output_dir) if f.endswith("_equity.csv")]

    for csv_file in csv_files:
        file_path = os.path.join(output_dir, csv_file)
        try:
            data = pd.read_csv(file_path, parse_dates=['Time'])
        except Exception as e:
            print(f"Error reading {file_path}: {e}")
            continue

        header = csv_file.replace("_equity.csv", "")
        starting_balance = data['Portfolio'].iloc[0]

        plt.figure(figsize=(12, 6))
        plt.plot(data['Time'], data['Portfolio'], label='Strategy')
        plt.plot(data['Time'], data['BuyAndHold'], label='Buy and hold', alpha=0.6)
        plt.axhline(y=starting_balance, linestyle='--', color='gray',
                    label=f'Starting Point ({starting_balance:.0f})')

        plt.title(header)
        plt.xlabel('Time')
        plt.ylabel('Balance')
        plt.xticks(rotation=45)
        plt.legend()
        plt.tight_layout()

        plt.show()
        plt.close()
        print(f"Displayed plot for {header}")

if __name__ == "__main__":
    output_directory = "output"
    visualize_all_trading_logs(output_directory)
    visualize_backtest_equity(output_directory)
