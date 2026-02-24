"""
Plot TensorStore Write Performance Data from CSV
This script reads the CSV performance logs and creates multiple plots showing write speeds and throughput.
"""

import pandas as pd
import matplotlib
matplotlib.use('Agg')  # Use non-interactive backend for faster plotting
import matplotlib.pyplot as plt
from matplotlib.widgets import TextBox, Button
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
import argparse
import glob
import os
import tkinter as tk
from tkinter import filedialog, ttk

# Disable interactive mode for performance
plt.ioff()

def select_csv_files():
    """
    Open a file dialog to select CSV files.
    
    Returns:
        List of selected file paths, or None if cancelled
    """
    root = tk.Tk()
    root.withdraw()  # Hide the main window
    
    file_paths = filedialog.askopenfilenames(
        title='Select CSV Performance Log Files',
        filetypes=[('CSV files', '*.csv'), ('All files', '*.*')],
        initialdir=os.getcwd()
    )
    
    root.destroy()
    
    if file_paths:
        return list(file_paths)
    return None

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
            df['dataset_label'] = os.path.basename(csv_file).replace('.csv', '')
            
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
    
    # Create labels for plotting
    if separate_cores:
        if 'core_id' in data.columns:
            data['label'] = data.apply(lambda row: f"{row['dataset_label']} - Core {row['core_id']}", axis=1)
        else:
            data['label'] = data['dataset_label']
    else:
        data['label'] = data['dataset_label']
    
    # Filters successful writes
    data_success = data[data['success'] == 1]
    
    print(f"\nTotal writes: {len(data)}")
    print(f"Successful writes: {len(data_success)}")
    print(f"Failed writes: {len(data[data['success'] == 0])}")

    # One stat box per dataset label, arranged in grid (2 per row)
    num_datasets = len(data_success['dataset_label'].unique())
    num_stat_rows = (num_datasets + 1) // 2  # 2 boxes per row
    total_rows = 2 + num_stat_rows  # 2 rows for graphs, rest for stats
    
    # Larger figure height to ensure all content is visible and scrollable
    fig = plt.figure(figsize=(24, 14 + 6 * num_stat_rows))
    # Reduced spacing to bring stats closer to graphs
    gs = fig.add_gridspec(total_rows, 4, hspace=0.3, wspace=0.4, top=0.98, bottom=0.10,
                         height_ratios=[1, 1] + [0.8] * num_stat_rows)
    
    # Position textboxes at the bottom, below all content
    label_y = 0.055
    textbox_y = 0.025
    button_y = 0.01
    
    # Toggle button for text boxes
    ax_button = plt.axes([0.01, button_y, 0.08, 0.015])
    toggle_button = Button(ax_button, 'Edit Titles')
    
    # Labels above textboxes (initially hidden)
    ax_label1 = plt.axes([0.12, label_y, 0.25, 0.015])
    ax_label1.axis('off')
    label1_text = ax_label1.text(0.0, 0.5, 'Graph 1: Write Latency Over Time', 
                                 fontsize=9, verticalalignment='center')
    ax_label1.set_visible(False)
    
    ax_label2 = plt.axes([0.38, label_y, 0.25, 0.015])
    ax_label2.axis('off')
    label2_text = ax_label2.text(0.0, 0.5, 'Graph 2: Total Frames Written Over Time', 
                                 fontsize=9, verticalalignment='center')
    ax_label2.set_visible(False)
    
    ax_label3 = plt.axes([0.64, label_y, 0.25, 0.015])
    ax_label3.axis('off')
    label3_text = ax_label3.text(0.0, 0.5, 'Graph 3: Write Throughput Over Time', 
                                 fontsize=9, verticalalignment='center')
    ax_label3.set_visible(False)
    
    # Text boxes in a row at the bottom (initially hidden)
    ax_textbox1 = plt.axes([0.12, textbox_y, 0.25, 0.02])
    text_box1 = TextBox(ax_textbox1, '', initial='Write Latency Over Time')
    ax_textbox1.set_visible(False)
    
    ax_textbox2 = plt.axes([0.38, textbox_y, 0.25, 0.02])
    text_box2 = TextBox(ax_textbox2, '', initial='Total Frames Written Over Time')
    ax_textbox2.set_visible(False)
    
    ax_textbox3 = plt.axes([0.64, textbox_y, 0.25, 0.02])
    text_box3 = TextBox(ax_textbox3, '', initial='Write Throughput Over Time')
    ax_textbox3.set_visible(False)
    
    # Track visibility state
    textboxes_visible = [False]
    
    def toggle_textboxes(event):
        textboxes_visible[0] = not textboxes_visible[0]
        ax_label1.set_visible(textboxes_visible[0])
        ax_label2.set_visible(textboxes_visible[0])
        ax_label3.set_visible(textboxes_visible[0])
        ax_textbox1.set_visible(textboxes_visible[0])
        ax_textbox2.set_visible(textboxes_visible[0])
        ax_textbox3.set_visible(textboxes_visible[0])
        toggle_button.label.set_text('Hide Titles' if textboxes_visible[0] else 'Edit Titles')
        plt.draw()
    
    toggle_button.on_clicked(toggle_textboxes)
    
    def update_title1(text):
        title1.set_text(text)
        plt.draw()
    
    def update_title2(text):
        title2.set_text(text)
        plt.draw()
    
    def update_title3(text):
        title3.set_text(text)
        plt.draw()
    
    text_box1.on_submit(update_title1)
    text_box2.on_submit(update_title2)
    text_box3.on_submit(update_title3)
    
    # Graph layout: 2 graphs in row 0, 1 graph in row 1
    ax1 = fig.add_subplot(gs[0, 0:2])
    ax2 = fig.add_subplot(gs[0, 2:4])
    ax3 = fig.add_subplot(gs[1, 1:3])  # Centered in second row
    
    # Frame size for throughput calculation
    FRAME_SIZE_MB = 18
    
    # Plot 1: Write latency
    for label in data_success['label'].unique():
        label_data = data_success[data_success['label'] == label]
        ax1.plot(label_data['timestamp_seconds'], label_data['write_time_us'] / 1000, 
                label=label, linewidth=1.5, alpha=0.8)
    ax1.set_xlabel('Time (seconds)')
    ax1.set_ylabel('Write Time (ms)')
    title1 = ax1.set_title('Write Latency Over Time')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: Cumulative frames written
    for label in data_success['label'].unique():
        label_data = data_success[data_success['label'] == label]
        ax2.plot(label_data['timestamp_seconds'], label_data['cumulative_frames'], 
                label=label, linewidth=1.5, alpha=0.8)
    ax2.set_xlabel('Time (seconds)')
    ax2.set_ylabel('Cumulative Frames Written')
    title2 = ax2.set_title('Total Frames Written Over Time')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    # Plot 3: Instantaneous throughput
    for label in data_success['label'].unique():
        label_data = data_success[data_success['label'] == label].copy()
        label_data = label_data.sort_values('timestamp_seconds')
        
        # Calculate instantaneous throughput between consecutive writes
        if len(label_data) > 1:
            timestamps = label_data['timestamp_seconds'].values
            frames = label_data['num_frames'].values
            
            # Calculate throughput for each interval
            throughputs = []
            plot_timestamps = []
            
            for i in range(1, len(timestamps)):
                time_diff = timestamps[i] - timestamps[i-1]
                if time_diff > 0:
                    frames_written = frames[i-1]  # frames written in this interval
                    # Throughput: (MB * 8 / 1000) / seconds = Gb/s
                    throughput_gbps = (frames_written * FRAME_SIZE_MB * 8 / 1000) / time_diff
                    throughputs.append(throughput_gbps)
                    plot_timestamps.append(timestamps[i])
            
            if throughputs:
                ax3.plot(plot_timestamps, throughputs, 
                        label=label, linewidth=1.5, alpha=0.8)
    
    ax3.set_xlabel('Time (seconds)')
    ax3.set_ylabel('Throughput (Gb/s)')
    title3 = ax3.set_title('Write Throughput Over Time')
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    
    # Group by dataset label for stats boxes
    groups = sorted(data_success['dataset_label'].unique())
    
    # Frame size in megabytes
    FRAME_SIZE_MB = 18
    
    for idx, group in enumerate(groups):
        row = 2 + (idx // 2)  # 2 boxes per row, starting from row 2
        col_start = 0 if idx % 2 == 0 else 2  # Left side (0:2) or right side (2:4)
        col_end = col_start + 2
        ax = fig.add_subplot(gs[row, col_start:col_end])
        ax.axis('off')
        
        group_data = data_success[data_success['dataset_label'] == group]
        
        stats_text = f"{group}\n"
        stats_text += "═" * 40 + "\n\n"
        
        # Overview
        stats_text += f"Frame Size: {FRAME_SIZE_MB} MB\n"
        if not separate_cores:
            if 'kvstore_driver' in group_data.columns:
                drivers = sorted(group_data['kvstore_driver'].unique())
                if len(drivers) == 1:
                    stats_text += f"Storage: {drivers[0]}\n"
        stats_text += "\n"
        
        # Write Operations Section
        stats_text += "Write Operations\n"
        stats_text += "─" * 20 + "\n"
        
        if separate_cores:
            for label in sorted(group_data['label'].unique()):
                label_data = group_data[group_data['label'] == label]
                stats_text += f"{label}:\n"
                stats_text += f"  Count: {len(label_data)}\n"
                stats_text += f"  Avg: {label_data['write_time_us'].mean()/1000:.2f} ms\n"
                stats_text += f"  Median: {label_data['write_time_us'].median()/1000:.2f} ms\n"
                stats_text += f"  Min: {label_data['write_time_us'].min()/1000:.2f} ms\n"
                stats_text += f"  Max: {label_data['write_time_us'].max()/1000:.2f} ms\n\n"
        else:
            stats_text += f"  Count: {len(group_data)}\n"
            stats_text += f"  Avg: {group_data['write_time_us'].mean()/1000:.2f} ms\n"
            stats_text += f"  Median: {group_data['write_time_us'].median()/1000:.2f} ms\n"
            stats_text += f"  Min: {group_data['write_time_us'].min()/1000:.2f} ms\n"
            stats_text += f"  Max: {group_data['write_time_us'].max()/1000:.2f} ms\n"
        
        stats_text += "\n"
        
        # Performance Metrics Section
        stats_text += "Performance Metrics\n"
        stats_text += "─" * 20 + "\n"
        
        if separate_cores:
            for label in sorted(group_data['label'].unique()):
                label_data = group_data[group_data['label'] == label]
                stats_text += f"{label}:\n"
                stats_text += f"  Total Frames: {label_data['num_frames'].sum()}\n"
                if 'timestamp_seconds' in label_data.columns and len(label_data) > 1:
                    time_span = label_data['timestamp_seconds'].max() - label_data['timestamp_seconds'].min()
                    if time_span > 0:
                        total_frames = label_data['num_frames'].sum()
                        actual_fps = total_frames / time_span
                        aggregate_gbps = (total_frames * FRAME_SIZE_MB * 8 / 1000) / time_span
                        stats_text += f"  Write FPS: {actual_fps:.1f}\n"
                        stats_text += f"  Throughput: {aggregate_gbps:.2f} Gb/s\n"
                stats_text += "\n"
        else:
            stats_text += f"  Total Frames: {group_data['num_frames'].sum()}\n"
            if 'timestamp_seconds' in group_data.columns and len(group_data) > 1:
                time_span = group_data['timestamp_seconds'].max() - group_data['timestamp_seconds'].min()
                if time_span > 0:
                    total_frames = group_data['num_frames'].sum()
                    actual_fps = total_frames / time_span
                    aggregate_gbps = (total_frames * FRAME_SIZE_MB * 8 / 1000) / time_span
                    total_data_gb = (total_frames * FRAME_SIZE_MB) / 1000
                    stats_text += f"  Total Data: {total_data_gb:.2f} GB\n"
                    stats_text += f"  Time Span: {time_span:.2f} s\n"
                    stats_text += f"  Write FPS: {actual_fps:.1f}\n"
                    stats_text += f"  Throughput: {aggregate_gbps:.2f} Gb/s\n"
        
        stats_text += "\n"
        
        # Configuration Section
        if not separate_cores:
            stats_text += "Configuration\n"
            stats_text += "─" * 20 + "\n"
            
            if 'frames_per_second' in group_data.columns:
                camera_fps = group_data['frames_per_second'].mean()
                stats_text += f"  Camera FPS: {camera_fps:.1f}\n"
            
            if 'core_id' in group_data.columns:
                core_ids = sorted(group_data['core_id'].unique())
                if len(core_ids) > 1:
                    stats_text += f"  Cores: {', '.join(map(str, core_ids))}\n"
                elif len(core_ids) == 1:
                    stats_text += f"  Core: {core_ids[0]}\n"
        
        ax.text(0.05, 0.95, stats_text, transform=ax.transAxes,
                fontsize=9, verticalalignment='top', fontfamily='monospace',
                bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
    
    # Create scrollable window
    root = tk.Tk()
    root.title("TensorStore Write Performance Analysis")
    
    # Set window size
    window_width = 1600
    window_height = 900
    root.geometry(f"{window_width}x{window_height}")
    
    # Create main frame with vertical scrollbar only
    main_frame = tk.Frame(root)
    main_frame.pack(fill=tk.BOTH, expand=True)
    
    # Create canvas for scrolling (vertical only)
    canvas = tk.Canvas(main_frame, bg='white')
    canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    
    # Add vertical scrollbar only
    v_scrollbar = ttk.Scrollbar(main_frame, orient=tk.VERTICAL, command=canvas.yview)
    v_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
    
    canvas.configure(yscrollcommand=v_scrollbar.set)
    
    # Create frame inside canvas
    scrollable_frame = tk.Frame(canvas, bg='white')
    canvas_window = canvas.create_window((0, 0), window=scrollable_frame, anchor='nw')
    
    # Embed matplotlib figure
    canvas_widget = FigureCanvasTkAgg(fig, master=scrollable_frame)
    canvas_widget.draw()
    canvas_widget.get_tk_widget().pack(side=tk.TOP, fill=tk.X, expand=True)
    
    # Add matplotlib toolbar
    toolbar = NavigationToolbar2Tk(canvas_widget, scrollable_frame)
    toolbar.update()
    toolbar.pack(side=tk.TOP, fill=tk.X)
    
    # Update canvas window width to match canvas width and configure scroll region
    def configure_scroll_region(event=None):
        # Make the scrollable frame width match the canvas width
        canvas_width = canvas.winfo_width()
        if canvas_width > 1:
            canvas.itemconfig(canvas_window, width=canvas_width)
        
        # Only update scroll region if needed (reduce redundant updates)
        canvas.update_idletasks()
        # Only set scroll region for vertical scrolling
        bbox = canvas.bbox('all')
        if bbox:
            canvas.configure(scrollregion=(0, bbox[1], canvas_width, bbox[3]))
    
    # Only bind to canvas, not scrollable_frame (reduce redundant updates)
    canvas.bind('<Configure>', configure_scroll_region)
    root.after(200, configure_scroll_region)  # Initial update with longer delay
    
    # Enable mousewheel scrolling (vertical only)
    def on_mousewheel(event):
        canvas.yview_scroll(int(-1*(event.delta/120)), "units")
    
    canvas.bind_all("<MouseWheel>", on_mousewheel)  # Windows
    canvas.bind_all("<Button-4>", lambda e: canvas.yview_scroll(-1, "units"))  # Linux scroll up
    canvas.bind_all("<Button-5>", lambda e: canvas.yview_scroll(1, "units"))  # Linux scroll down
    
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description='Plot TensorStore write performance data')
    parser.add_argument('csv_files', nargs='*', help='CSV file(s) to plot (optional - will open file dialog if not provided)')
    parser.add_argument('--separate-cores', action='store_true', 
                       help='Plot each core separately')
    
    args = parser.parse_args()
    
    # If no files provided via command line, open file dialog
    if not args.csv_files:
        print("Opening file selection dialog...")
        csv_files = select_csv_files()
        if not csv_files:
            print("No files selected. Exiting.")
            return
    else:
        # Expand glob patterns from command line
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
