#!/usr/bin/env python3

import csv
import os
import sys
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter
from odf.opendocument import OpenDocumentSpreadsheet
from odf.table import Table, TableRow, TableCell
from odf.text import P
from odf.draw import Frame, Image
from odf.style import Style, GraphicProperties


def generate_bar_chart(output_folder, data, file_stem):
    """
    Create a bar chart based on the provided data.
    """
    bits = [int(row[0]) for row in data]
    values_0to1 = [int(row[1]) for row in data]
    values_1to0 = [int(row[2]) for row in data]
    averages = [float(row[3]) for row in data]

    # Width of each group of bars
    bar_width = 0.2

    # Position of the bars on the X axis
    x_positions = np.arange(len(bits))

    # Create the bar chart
    plt.figure(figsize=(12, 8))
    plt.bar(x_positions - bar_width, values_0to1, bar_width, label="0to1")
    plt.bar(x_positions, values_1to0, bar_width, label="1to0")
    plt.bar(x_positions + bar_width, averages, bar_width, label="average")

    # Add legend only
    plt.legend()

    # Format the vertical axis numbers to use plain notation
    def plain_formatter(x, pos):
        return f"{int(x):,}"  # Adds thousands separator
    plt.gca().yaxis.set_major_formatter(FuncFormatter(plain_formatter))

    # Remove extra margins on the X-axis
    plt.gca().set_xlim([-0.5, len(bits) - 0.5])  # Align bars exactly with edges

    # Replace default X-tick labels with the Bit values
    plt.xticks(x_positions, bits, rotation=45)

    # Remove titles from the chart and axes
    plt.xlabel("")
    plt.ylabel("")
    plt.title("")

    plt.tight_layout()

    # Save the plot to a file
    plot_path = os.path.join(output_folder, f"{file_stem}.png")
    plt.savefig(plot_path)
    plt.close()
    print(f"Bar chart saved to: {plot_path}")


def write_to_ods(ods_doc, sheet_name, data, chart_path):
    """
    Add a sheet to the ODS file with the given data and embed the chart.
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

    # Embed the chart as an image in a new row
    if os.path.exists(chart_path):
        # Add a new row for the chart
        image_row = TableRow()

        # Create a cell to contain the image frame
        image_cell = TableCell()

        # Add the image to the ODS package
        relative_path = ods_doc.addPicture(chart_path)

        # Create a frame to hold the image
        frame = Frame(width="15cm", height="10cm", x="0cm", y="0cm")
        image = Image(href=relative_path, type="simple")
        frame.addElement(image)

        # Add the frame to the cell
        image_cell.addElement(frame)

        # Add the cell with the frame to the row
        image_row.addElement(image_cell)

        # Add the row to the table
        table.addElement(image_row)

    # Add the table to the spreadsheet
    ods_doc.spreadsheet.addElement(table)


def process_csv(input_csv, ods_doc, output_folder):
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
            row.append(f"{avg_val:.2f}")
        except (ValueError, IndexError):
            # If parsing fails, keep the row unchanged
            pass

        # Add the processed row
        processed_rows.append(row)

    # Extract numeric data (excluding the header)
    numeric_data = [row for row in processed_rows if len(row) >= 4 and row[0].isdigit()]

    # Generate the bar chart
    chart_path = os.path.join(output_folder, f"{file_stem}.png")
    generate_bar_chart(output_folder, numeric_data, file_stem)

    # Add processed data and chart to ODS file
    write_to_ods(ods_doc, file_stem, processed_rows, chart_path)


def main():
    if len(sys.argv) < 2:
        print("Usage: python plotter.py <folder_with_csvs>")
        sys.exit(1)

    input_folder = sys.argv[1]

    # Prepare output folder
    output_folder = "plotter_output"
    os.makedirs(output_folder, exist_ok=True)

    # Create an ODS file
    ods_file_path = os.path.join(output_folder, "processed_data.ods")
    ods_doc = OpenDocumentSpreadsheet()

    # Process all CSV files in the input folder
    for file_name in os.listdir(input_folder):
        input_path = os.path.join(input_folder, file_name)
        if os.path.isfile(input_path) and file_name.endswith(".csv"):
            print(f"Processing: {input_path}")
            process_csv(input_path, ods_doc, output_folder)

    # Save the ODS file
    ods_doc.save(ods_file_path)
    print(f"All processed data saved to: {ods_file_path}")


if __name__ == "__main__":
    main()
