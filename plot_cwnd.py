#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CWND Curve Plotting Script
Reads cwnd data from CSV file and generates a plot.
Usage: python plot_cwnd.py [input_csv] [output_image]
"""

import sys
import os

def plot_cwnd(input_file="cwnd_log.csv", output_file="cwnd_curve.png"):
    """
    Read cwnd data from CSV and plot the curve.
    
    CSV format: time_ms,cwnd
    """
    try:
        import matplotlib
        matplotlib.use('Agg')  # Use non-interactive backend for saving to file
        import matplotlib.pyplot as plt
    except ImportError:
        print("Error: matplotlib is required. Install with: pip install matplotlib")
        return False
    
    if not os.path.exists(input_file):
        print(f"Error: Input file '{input_file}' not found.")
        return False
    
    times = []
    cwnds = []
    
    try:
        with open(input_file, 'r') as f:
            # Skip header line
            header = f.readline()
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(',')
                if len(parts) >= 2:
                    time_ms = float(parts[0])
                    cwnd = int(parts[1])
                    times.append(time_ms)
                    cwnds.append(cwnd)
    except Exception as e:
        print(f"Error reading file: {e}")
        return False
    
    if not times:
        print("Error: No data found in input file.")
        return False
    
    # Convert to relative time in seconds
    start_time = times[0]
    times_sec = [(t - start_time) / 1000.0 for t in times]
    
    # Create the plot
    plt.figure(figsize=(12, 6))
    plt.plot(times_sec, cwnds, 'b-', linewidth=1.5, marker='o', markersize=3)
    plt.xlabel('Time (seconds)', fontsize=12)
    plt.ylabel('Congestion Window (segments)', fontsize=12)
    plt.title('TCP Reno Congestion Window (cwnd) Over Time', fontsize=14)
    plt.grid(True, alpha=0.3)
    plt.xlim(left=0)
    plt.ylim(bottom=0)
    
    # Add some statistics as text
    max_cwnd = max(cwnds)
    avg_cwnd = sum(cwnds) / len(cwnds)
    plt.text(0.02, 0.98, f'Max cwnd: {max_cwnd}\nAvg cwnd: {avg_cwnd:.2f}\nSamples: {len(cwnds)}',
             transform=plt.gca().transAxes, verticalalignment='top',
             fontsize=10, bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
    
    plt.tight_layout()
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    plt.close()
    
    print(f"CWND curve saved to: {output_file}")
    print(f"  - Total samples: {len(cwnds)}")
    print(f"  - Time range: 0 - {times_sec[-1]:.3f} seconds")
    print(f"  - CWND range: {min(cwnds)} - {max_cwnd} segments")
    
    return True

if __name__ == "__main__":
    input_csv = sys.argv[1] if len(sys.argv) > 1 else "cwnd_log.csv"
    output_img = sys.argv[2] if len(sys.argv) > 2 else "cwnd_curve.png"
    
    success = plot_cwnd(input_csv, output_img)
    sys.exit(0 if success else 1)
