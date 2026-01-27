"""
Plot TensorStore Write Performance Data from CSV
This script reads the CSV performance logs and creates multiple plots showing write speeds and throughput.
"""

import pandas as pd
import matplotlib.pyplot as plt
import argparse
import glob
import os

def plot_write_performance(csv_files, separate_cores=False, compare_fps=False):
    """
    Create performance plots from CSV log files.
    
    Args:
        csv_files: List of CSV file paths to process
        separate_cores: If True, plot each core separately; if False, plot combined data
        compare_fps: If True, group data by FPS ranges for comparison
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
            
            if 'frames_per_second' in df.columns and len(df) > 0:
                df['fps_value'] = df['frames_per_second'].iloc[0]
            
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
    
    if compare_fps and 'fps_value' in data.columns:
        data['label'] = data.apply(
            lambda row: f"{int(row['fps_value'])} FPS ({row['kvstore_from_filename']})" 
            if pd.notna(row['fps_value']) else f"Unknown FPS ({row['kvstore_from_filename']})",
            axis=1
        )
    elif separate_cores:
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

    if compare_fps and 'fps_value' in data_success.columns:
        num_stat_boxes = len(data_success['fps_value'].dropna().unique())
    else:
        num_stat_boxes = len(data_success['kvstore_driver'].unique())
    
    num_stat_rows = (num_stat_boxes + 3) // 4
    total_rows = 1 + num_stat_rows
    
    fig = plt.figure(figsize=(20, 6 + 4 * num_stat_rows))
    gs = fig.add_gridspec(total_rows, 4, hspace=0.3, wspace=0.3)
    
    fig.suptitle('TensorStore Write Performance Analysis', fontsize=16, fontweight='bold')
    
    ax1 = fig.add_subplot(gs[0, 0:2])
    ax2 = fig.add_subplot(gs[0, 2:4])
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
    for label in data_success['label'].unique():
        label_data = data_success[data_success['label'] == label]
        ax2.plot(label_data['timestamp_seconds'], label_data['cumulative_frames'], 
                label=label, marker='o', markersize=3, alpha=0.7)
    ax2.set_xlabel('Time (seconds)')
    ax2.set_ylabel('Cumulative Frames Written')
    ax2.set_title('Total Frames Written Over Time')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    if compare_fps and 'fps_value' in data_success.columns:
        groups = sorted(data_success['fps_value'].dropna().unique())
        group_column = 'fps_value'
    else:
        groups = sorted(data_success['kvstore_driver'].unique())
        group_column = 'kvstore_driver'
    
    for idx, group in enumerate(groups):
        row = 1 + (idx // 4) 
        col = idx % 4
        ax = fig.add_subplot(gs[row, col])
        ax.axis('off')
        
        group_data = data_success[data_success[group_column] == group]
        
        if compare_fps and 'fps_value' in data_success.columns:
            drivers = sorted(group_data['kvstore_driver'].unique())
            driver_list = ', '.join(drivers)
            group_name = f"{int(group)} FPS ({driver_list})"
        else:
            group_name = str(group)
        
        stats_text = f"Performance - {group_name}:\n\n"
        
        if separate_cores and not compare_fps:
            for label in sorted(group_data['label'].unique()):
                label_data = group_data[group_data['label'] == label]
                stats_text += f"{label}:\n"
                stats_text += f"  Writes: {len(label_data)}\n"
                stats_text += f"  Avg: {label_data['write_time_us'].mean()/1000:.2f} ms\n"
                stats_text += f"  Median: {label_data['write_time_us'].median()/1000:.2f} ms\n"
                stats_text += f"  Min: {label_data['write_time_us'].min()/1000:.2f} ms\n"
                stats_text += f"  Max: {label_data['write_time_us'].max()/1000:.2f} ms\n"
                stats_text += f"  Total Frames: {label_data['num_frames'].sum()}\n\n"
        else:
            stats_text += f"  Writes: {len(group_data)}\n"
            stats_text += f"  Avg: {group_data['write_time_us'].mean()/1000:.2f} ms\n"
            stats_text += f"  Median: {group_data['write_time_us'].median()/1000:.2f} ms\n"
            stats_text += f"  Min: {group_data['write_time_us'].min()/1000:.2f} ms\n"
            stats_text += f"  Max: {group_data['write_time_us'].max()/1000:.2f} ms\n"
            stats_text += f"  Total Frames: {group_data['num_frames'].sum()}\n"
            
            if 'core_id' in group_data.columns:
                core_ids = sorted(group_data['core_id'].unique())
                if len(core_ids) > 1:
                    stats_text += f"\n  Cores: {', '.join(map(str, core_ids))}\n"
            
            if 'frames_per_second' in group_data.columns and not compare_fps:
                avg_fps = group_data['frames_per_second'].mean()
                stats_text += f"  Avg FPS: {avg_fps:.1f}\n"
        
        ax.text(0.05, 0.95, stats_text, transform=ax.transAxes, 
                fontsize=9, verticalalignment='top', fontfamily='monospace',
                bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
    
    plt.tight_layout()
    plt.show()


def main():
    parser = argparse.ArgumentParser(description='Plot TensorStore write performance data')
    parser.add_argument('csv_files', nargs='+', help='CSV file(s) to plot ')
    parser.add_argument('--separate-cores', action='store_true', 
                       help='Plot each core separately')
    parser.add_argument('--compare-fps', action='store_true',
                       help='Compare performance grouped by frames per second ranges')
    
    args = parser.parse_args()
    
    csv_files = []
    for pattern in args.csv_files:
        csv_files.extend(glob.glob(pattern))
    
    if not csv_files:
        print("No CSV files found!")
        return
    
    print(f"Processing {len(csv_files)} file(s)...")
    plot_write_performance(csv_files, args.separate_cores, args.compare_fps)


if __name__ == '__main__':
    main()
