import pandas as pd
import matplotlib.pyplot as plt
import os

# Directory containing CSV files
csv_directory = 'csv_output/'

# Function to find the best CSV file for each coin based on the final balance
def find_best_csv_files(csv_directory):
    coin_files = {}
    for csv_file in os.listdir(csv_directory):
        print(csv_file + " is being processed, processed/total: " + str(len(coin_files)) + "/" + str(len(os.listdir(csv_directory))))
        if csv_file.endswith('.csv'):
            parts = csv_file.split('_')
            if len(parts) >= 4:
                coin = parts[2]
                file_path = os.path.join(csv_directory, csv_file)
                data = pd.read_csv(file_path)
                final_balance = data['Balance'].iloc[-1]
                
                if coin not in coin_files or final_balance > coin_files[coin][1]:
                    coin_files[coin] = (csv_file, final_balance)
    return [value[0] for value in coin_files.values()]

# Function to visualize the balance of multiple CSV files in a single figure
def visualize_balances(csv_files, plot_index):
    plt.figure(figsize=(18, 12))
    for i, csv_file in enumerate(csv_files):
        file_path = os.path.join(csv_directory, csv_file)
        data = pd.read_csv(file_path)
        data['Open Time'] = pd.to_datetime(data['Open Time'])

        # Filter rows where balance changes
        filtered_data = data[data['Balance'].diff() != 0]

        open_time = filtered_data['Open Time'].values
        balance = filtered_data['Balance'].values

        ax = plt.subplot(3, 2, i + 1)
        ax.plot(open_time, balance, label=csv_file)
        ax.set_title(f'Balance over Time for {csv_file}')
        ax.set_xlabel('Open Time')
        ax.set_ylabel('Balance')
        ax.tick_params(axis='x', rotation=45)
        ax.legend()

    plt.tight_layout()
    plt.savefig(f'balance_plot_{plot_index}.png')
    plt.close()

# Find the best CSV files
best_csv_files = find_best_csv_files(csv_directory)

# Plot balance for 6 coins at a time and save each plot as an image
for i in range(0, len(best_csv_files), 6):
    visualize_balances(best_csv_files[i:i+6], i//6)

