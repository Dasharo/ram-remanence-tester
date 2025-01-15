#!/usr/bin/env python3

import csv
import os
import sys
import tempfile
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter
from odf.opendocument import OpenDocumentSpreadsheet
from odf.table import Table, TableRow, TableCell
from odf.text import P
from odf.draw import Frame, Image
from odf.manifest import FileEntry
import PIL.Image


def generate_bar_chart(data, temp_dir, file_stem):
    """
    Create a bar chart with a secondary Y-axis (percentage scale) on the right and save it temporarily.
    """
    bits = [int(row[0]) for row in data]
    values_0to1 = [int(row[1]) for row in data]
    values_1to0 = [int(row[2]) for row in data]
    averages = [float(row[3]) for row in data]

    # Calculate the total memory as the sum of all bars
    total_memory = sum(values_0to1) + sum(values_1to0)

    # Width of each group of bars
    bar_width = 0.2

    # Position of the bars on the X axis
    x_positions = np.arange(len(bits))

    # Dynamically adjust figure width based on number of bits
    fig_width = max(10, len(bits) * 0.2)
    fig, ax1 = plt.subplots(figsize=(fig_width, 8))  # Keep height consistent

    # Create the bar chart on the primary axis
    ax1.bar(x_positions - bar_width, values_0to1, bar_width, label="0to1", color="tab:blue")
    ax1.bar(x_positions, values_1to0, bar_width, label="1to0", color="tab:orange")
    ax1.bar(x_positions + bar_width, averages, bar_width, label="average", color="tab:green")

    # Add legend
    ax1.legend(loc="upper left")

    # Format the primary vertical axis (absolute values)
    ax1.set_ylabel("Absolute Value (bits switched)", fontsize=14, labelpad=10)
    ax1.yaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{int(x):,}"))

    # Add labels and adjust tick label font sizes
    ax1.set_xlabel("Bit number in data bus", fontsize=16, labelpad=10)
    ax1.tick_params(axis='x', labelsize=10)
    ax1.tick_params(axis='y', labelsize=10)

    # Add a secondary Y-axis for percentage
    ax2 = ax1.twinx()  # Create a secondary axis sharing the same x-axis
    ax2.set_ylabel("Percentage of Total Memory", fontsize=14, labelpad=10)

    # Calculate and set the percentage scale
    ax2.set_ylim(0, 100)  # Percentage goes from 0 to 100%
    ax2.yaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:.0f}%"))
    ax2.tick_params(axis='y', labelsize=10)

    # Replace default X-tick labels with the Bit values
    ax1.set_xticks(x_positions)
    ax1.set_xticklabels(bits, rotation=45, ha="right")

    # Adjust x-axis limits to match the bar spacing
    ax1.set_xlim([-0.5, len(bits) - 0.5])

    # Use tight layout to prevent label clipping
    plt.tight_layout()

    # Save the plot to a temporary file
    chart_path = os.path.join(temp_dir, f"{file_stem}.png")
    plt.savefig(chart_path)
    plt.close()
    return chart_path

def write_to_ods(ods_doc, sheet_name, data, chart_path):
    """
    Add a sheet to the ODS file with the given data and embed the chart at the top in a new column.
    The chart is resized to 80% of its original size while preserving the aspect ratio.
    """
    table = Table(name=sheet_name)

    # Add the table with data
    for row in data:
        table_row = TableRow()
        for cell in row:
            table_cell = TableCell()
            table_cell.addElement(P(text=str(cell)))  # Properly wrap the text
            table_row.addElement(table_cell)
        table.addElement(table_row)

    # Embed the chart as an image in the top row, new column
    if os.path.exists(chart_path):
        # Add the image to the ODS package
        relative_path = ods_doc.addPicture(chart_path)

        # Get the original dimensions of the image
        import PIL.Image
        with PIL.Image.open(chart_path) as img:
            img_width, img_height = img.size

        # Scale the image to 80% of its original size
        scale_factor = 0.7
        scaled_width_cm = (img_width * scale_factor) / 96 * 2.54  # Convert pixels to cm
        scaled_height_cm = (img_height * scale_factor) / 96 * 2.54  # Convert pixels to cm

        # Create a frame to hold the image with scaled dimensions
        frame = Frame(
            width=f"{scaled_width_cm:.2f}cm",
            height=f"{scaled_height_cm:.2f}cm",
            x="0cm",
            y="0cm",
        )
        image = Image(href=relative_path, type="simple")
        frame.addElement(image)

        # Add the image in the first row, next to the data
        chart_row = table.firstChild  # Get the first row of the table
        if not chart_row:
            chart_row = TableRow()
            table.addElement(chart_row)

        # Create a new cell in the first row for the chart
        chart_cell = TableCell()
        chart_cell.addElement(frame)
        chart_row.addElement(chart_cell)

    # Add the table to the spreadsheet
    ods_doc.spreadsheet.addElement(table)


def process_csv(input_csv, ods_doc, temp_dir):
    file_stem = os.path.splitext(os.path.basename(input_csv))[0]  # File name without extension

    # Read and process the CSV file
    with open(input_csv, 'r', encoding='utf-8') as f_in:
        reader = csv.reader(f_in, delimiter=',')
        rows = list(reader)

    # Processed rows to store in ODS
    processed_rows = []
    header_found = False

    for row in rows:
        # Handle empty rows
        if not row:
            processed_rows.append(row)
            continue

        # Detect header row
        if not header_found:
            if len(row) >= 3 and row[0].strip() == "Bit" and row[1].strip() == "0to1" and row[2].strip() == "1to0":
                # Add "average" to the header
                row.append("average")
                header_found = True
            processed_rows.append(row)
            continue

        # Process numeric rows after the header
        try:
            # Parse the first three columns
            bit_val = int(row[0].strip())
            val_0to1 = int(row[1].strip())
            val_1to0 = int(row[2].strip())

            # Compute the average
            avg_val = (val_0to1 + val_1to0) / 2.0

            # Append the average to the row
            row.append(f"{avg_val:.1f}")
        except (ValueError, IndexError):
            # If parsing fails, keep the row unchanged
            pass

        # Add the processed row
        processed_rows.append(row)

    # Extract numeric data (excluding the header)
    numeric_data = [row for row in processed_rows if len(row) >= 4 and row[0].isdigit()]

    # Generate the bar chart
    chart_path = generate_bar_chart(numeric_data, temp_dir, file_stem)

    # Add processed data and chart to ODS file
    write_to_ods(ods_doc, file_stem, processed_rows, chart_path)


def main():
    if len(sys.argv) < 2:
        print("Usage: python plotter.py <folder_with_csvs>")
        sys.exit(1)

    input_folder = sys.argv[1]

    # Prepare a temporary directory for charts
    with tempfile.TemporaryDirectory() as temp_dir:
        # Create an ODS file
        ods_file_path = os.path.join(input_folder, "processed_data.ods")
        ods_doc = OpenDocumentSpreadsheet()

        # Process all CSV files in the input folder
        for file_name in os.listdir(input_folder):
            input_path = os.path.join(input_folder, file_name)
            if os.path.isfile(input_path) and file_name.endswith(".csv"):
                print(f"Processing: {input_path}")
                process_csv(input_path, ods_doc, temp_dir)

        # Save the ODS file
        ods_doc.save(ods_file_path)
        print(f"All processed data saved to: {ods_file_path}")


if __name__ == "__main__":
    main()
