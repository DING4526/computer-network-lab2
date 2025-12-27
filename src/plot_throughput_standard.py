# plot_throughput_standard.py
import pandas as pd
import matplotlib.pyplot as plt
from io import StringIO

# ===== Input table (tab-separated) =====
raw = """吞吐\t16\t8\t4\t2
0 0\t0.06\t0.062\t0.067\t0.046
1 2\t0.04\t0.047\t0.045\t0.027
3 5\t0.023\t0.03\t0.031\t0.021
5 10\t0.021\t0.025\t0.026\t0.018
"""

# ===== Read data =====
df = pd.read_csv(StringIO(raw), sep=r"\t", engine="python")

# First column: "loss delay" (e.g., "3 5")
pairs = df.iloc[:, 0].astype(str).str.split(r"\s+", expand=True)
df["loss_pct"] = pairs[0].astype(int)
df["delay_ms"] = pairs[1].astype(int)

# X labels: "(loss%, delayms)"
x_labels = [f"({l}%, {d}ms)" for l, d in zip(df["loss_pct"], df["delay_ms"])]

# Columns for fixed windows
wnd_cols = ["16", "8", "4", "2"]

# ===== Plot style (report-like) =====
plt.rcParams.update({
    "font.size": 12,
    "axes.titlesize": 14,
    "axes.labelsize": 13,
    "legend.fontsize": 11,
    "xtick.labelsize": 11,
    "ytick.labelsize": 11,
})

fig, ax = plt.subplots(figsize=(9.5, 5.2))

for wnd in wnd_cols:
    ax.plot(
        x_labels,
        df[wnd].astype(float),
        marker="o",
        linewidth=2.0,
        label=f"fixed_wnd = {wnd}"
    )

# ===== Axes labels, title, limits =====
ax.set_title("Throughput under Combined Loss/Delay Conditions", pad=10)
ax.set_xlabel("Network condition (loss, delay)")
ax.set_ylabel("Throughput (MB/s)") 

ax.set_ylim(bottom=0)  # y-axis starts at 0 for fair visual comparison

# ===== Grid & legend =====
ax.grid(True, which="major", linestyle="--", linewidth=0.8, alpha=0.6)
ax.legend(loc="best", frameon=True)

fig.tight_layout()

# ===== Save outputs =====
png_path = "throughput_vs_condition.png"
pdf_path = "throughput_vs_condition.pdf"
fig.savefig(png_path, dpi=300)
fig.savefig(pdf_path)

plt.show()

print(f"Saved: {png_path}")
print(f"Saved: {pdf_path}")
