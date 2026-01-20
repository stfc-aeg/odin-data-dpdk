"""
Plot TensorStore Write Performance Data from CSV
This script reads the CSV performance logs and creates multiple plots showing write speeds and throughput.
"""

import pandas as pd
import matplotlib.pyplot as plt
import argparse
import glob
import os

def plot_write_performance(csv_files, separate_cores=False):
    """
    Create performance plots from CSV log files.
    
    Args:
        csv_files: List of CSV file paths to process
        separate_cores: If True, plot each core separately; if False, plot combined data
    """
    
    # Read all CSV files
    dfs = []
    for csv_file in csv_files:
        try:
            df = pd.read_csv(csv_file)
            df['source_file'] = os.path.basename(csv_file)
            
            filename = os.path.basename(csv_file)
            if '_file.csv' in filename:
                df['kvstore_from_filename'] = 'file'
            elif '_s3.csv' in filename:
                df['kvstore_from_filename'] = 's3'
            else:
                df['kvstore_from_filename'] = 'unknown'
            
            dfs.append(df)
            print(f"Loaded {len(df)} records from {csv_file}")
        except Exception as e:
            print(f"Error reading {csv_file}: {e}")
    
    if not dfs:
        print("No data to plot!")
        return
    
    data = pd.concat(dfs, ignore_index=True)
    
    # Identify kvstore driver from filename
    data['kvstore_driver'] = data['kvstore_from_filename']
    
    if separate_cores:
        if 'core_id' in data.columns:
            data['label'] = data.apply(lambda row: f"Core {row['core_id']} ({row['kvstore_from_filename']})", axis=1)
        else:
            data['label'] = data['kvstore_from_filename']
    else:
        data['label'] = data['kvstore_from_filename']
    
    # Filters successful writes
    data_success = data[data['success'] == 1]
    
    print(f"\nTotal writes: {len(data)}")
    print(f"Successful writes: {len(data_success)}")
    print(f"Failed writes: {len(data[data['success'] == 0])}")
    
 
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle('TensorStore Write Performance Analysis', fontsize=16, fontweight='bold')
    
    # Plot 1: Write time over time
    ax1 = axes[0, 0]
    for label in data_success['label'].unique():
        label_data = data_success[data_success['label'] == label]
        ax1.plot(label_data['timestamp_seconds'], label_data['write_time_us'] / 1000, 
                label=label, marker='o', markersize=3, alpha=0.7)
    ax1.set_xlabel('Time (seconds)')
    ax1.set_ylabel('Write Time (ms)')
    ax1.set_title('Write Latency Over Time')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    
    # Plot 2: Cumulative frames written
    ax2 = axes[0, 1]
    for label in data_success['label'].unique():
        label_data = data_success[data_success['label'] == label]
        ax2.plot(label_data['timestamp_seconds'], label_data['cumulative_frames'], 
                label=label, marker='o', markersize=3, alpha=0.7)
    ax2.set_xlabel('Time (seconds)')
    ax2.set_ylabel('Cumulative Frames Written')
    ax2.set_title('Total Frames Written Over Time')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    # Plot 3 & 4: Statistics summary boxes - one per kvstore driver
    ax3 = axes[1, 0]
    ax3.axis('off')
    ax4 = axes[1, 1]
    ax4.axis('off')
    
    # Get unique kvstore drivers
    kvstore_drivers = sorted(data_success['kvstore_driver'].unique())
    
    # Distribute drivers between the two bottom subplots
    axes_list = [ax3, ax4]
    
    for idx, driver in enumerate(kvstore_drivers):
        if idx >= len(axes_list):
            break  
            
        ax = axes_list[idx]
        
        driver_data = data_success[data_success['kvstore_driver'] == driver]
        
        stats_text = f"Performance Statistics - {driver.upper()} Driver:\n\n"
        
        if separate_cores:

            for label in sorted(driver_data['label'].unique()):
                label_data = driver_data[driver_data['label'] == label]
                stats_text += f"{label}:\n"
                stats_text += f"  Writes: {len(label_data)}\n"
                stats_text += f"  Avg: {label_data['write_time_us'].mean()/1000:.2f} ms\n"
                stats_text += f"  Median: {label_data['write_time_us'].median()/1000:.2f} ms\n"
                stats_text += f"  Min: {label_data['write_time_us'].min()/1000:.2f} ms\n"
                stats_text += f"  Max: {label_data['write_time_us'].max()/1000:.2f} ms\n"
                stats_text += f"  Total Frames: {label_data['num_frames'].sum()}\n\n"
        else:
            stats_text += f"  Writes: {len(driver_data)}\n"
            stats_text += f"  Avg: {driver_data['write_time_us'].mean()/1000:.2f} ms\n"
            stats_text += f"  Median: {driver_data['write_time_us'].median()/1000:.2f} ms\n"
            stats_text += f"  Min: {driver_data['write_time_us'].min()/1000:.2f} ms\n"
            stats_text += f"  Max: {driver_data['write_time_us'].max()/1000:.2f} ms\n"
            stats_text += f"  Total Frames: {driver_data['num_frames'].sum()}\n"
            
            if 'core_id' in driver_data.columns:
                core_ids = sorted(driver_data['core_id'].unique())
                if len(core_ids) > 1:
                    stats_text += f"\n  Cores: {', '.join(map(str, core_ids))}\n"
        
        ax.text(0.05, 0.95, stats_text, transform=ax.transAxes, 
                fontsize=9, verticalalignment='top', fontfamily='monospace',
                bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
    
    plt.tight_layout()
    



def main():
    parser = argparse.ArgumentParser(description='Plot TensorStore write performance data')
    parser.add_argument('csv_files', nargs='+', help='CSV file(s) to plot ')
    parser.add_argument('--separate-cores', action='store_true', 
                       help='Plot each core separately')
    
    args = parser.parse_args()
    
    csv_files = []
    for pattern in args.csv_files:
        csv_files.extend(glob.glob(pattern))
    
    if not csv_files:
        print("No CSV files found!")
        return
    
    print(f"Processing {len(csv_files)} file(s)...")
    plot_write_performance(csv_files, args.separate_cores)


if __name__ == '__main__':
    main()
