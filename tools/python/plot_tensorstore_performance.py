#!/usr/bin/env python3
"""
Plot TensorStore Write Performance Data from CSV
This script reads the CSV performance logs and creates multiple plots showing write speeds and throughput.
"""

import pandas as pd
import matplotlib.pyplot as plt
import argparse
import glob
import os

def plot_write_performance(csv_files, output_dir=None):
    """
    Create performance plots from CSV log files.
    
    Args:
        csv_files: List of CSV file paths to process
        output_dir: Directory to save plots (optional)
    """
    
    # Read all CSV files
    dfs = []
    for csv_file in csv_files:
        try:
            df = pd.read_csv(csv_file)
            core_id = df['core_id'].iloc[0] if 'core_id' in df.columns else 'unknown'
            df['source_file'] = os.path.basename(csv_file)
            
            # Create a label combining core and driver for plotting
            if 'driver' in df.columns:
                # Group by driver to handle driver changes
                for driver in df['driver'].unique():
                    driver_df = df[df['driver'] == driver].copy()
                    driver_df['label'] = f"Core {core_id} ({driver})"
                    dfs.append(driver_df)
            else:
                df['label'] = f"Core {core_id}"
                dfs.append(df)
            print(f"Loaded {len(df)} records from {csv_file}")
        except Exception as e:
            print(f"Error reading {csv_file}: {e}")
    
    if not dfs:
        print("No data to plot!")
        return
    
    data = pd.concat(dfs, ignore_index=True)
    
    # Filter only successful writes
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
    
    # Plot 3: Statistics summary
    ax3 = axes[1, 0]
    ax3.axis('off')
    
    # Calculate summary statistics
    stats_text = "Performance Statistics:\n\n"
    for label in data_success['label'].unique():
        label_data = data_success[data_success['label'] == label]
        stats_text += f"{label}:\n"
        stats_text += f"  Writes: {len(label_data)}\n"
        stats_text += f"  Avg Write Time: {label_data['write_time_us'].mean()/1000:.2f} ms\n"
        stats_text += f"  Min Write Time: {label_data['write_time_us'].min()/1000:.2f} ms\n"
        stats_text += f"  Max Write Time: {label_data['write_time_us'].max()/1000:.2f} ms\n"
        stats_text += f"  Total Frames: {label_data['num_frames'].sum()}\n\n"
    
    ax3.text(0.1, 0.95, stats_text, transform=ax3.transAxes, 
            fontsize=10, verticalalignment='top', fontfamily='monospace',
            bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
    
    #Empty plot for layout
    ax4 = axes[1, 1]
    ax4.axis('off')
    
    plt.tight_layout()
    
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
        output_path = os.path.join(output_dir, 'tensorstore_performance.png')
        plt.savefig(output_path, dpi=300, bbox_inches='tight')
        print(f"\nPlot saved to: {output_path}")
        
        # Save summary statistics to text file
        stats_path = os.path.join(output_dir, 'tensorstore_statistics.txt')
        with open(stats_path, 'w') as f:
            f.write(stats_text)
        print(f"Statistics saved to: {stats_path}")
    else:
        plt.show()


def main():
    parser = argparse.ArgumentParser(description='Plot TensorStore write performance data')
    parser.add_argument('csv_files', nargs='+', help='CSV file(s) to plot (supports wildcards)')
    parser.add_argument('--output-dir', '-o', help='Directory to save plots')
    
    args = parser.parse_args()
    
    csv_files = []
    for pattern in args.csv_files:
        csv_files.extend(glob.glob(pattern))
    
    if not csv_files:
        print("No CSV files found!")
        return
    
    print(f"Processing {len(csv_files)} file(s)...")
    plot_write_performance(csv_files, args.output_dir)


if __name__ == '__main__':
    main()
