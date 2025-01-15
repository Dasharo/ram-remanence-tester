#!/usr/bin/env python3

import csv
import os
import argparse
import tempfile
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter
from odf.opendocument import OpenDocumentSpreadsheet
from odf.table import Table, TableRow, TableCell
from odf.text import P
from odf.draw import Frame, Image
import PIL.Image



def generate_bar_chart(data, temp_dir, file_stem, save_pngs, output_folder, total_bits):
    """
    Create a bar chart with a secondary Y-axis (percentage scale) on the right.
    Save it temporarily and optionally save as a standalone .png file in output_folder/chart_pngs/.
    """
    bits = [int(row[0]) for row in data]
    values_0to1 = [int(row[1]) for row in data]
    values_1to0 = [int(row[2]) for row in data]
    averages = [float(row[3]) for row in data]

    # Width of each group of bars
    bar_width = 0.2
    x_positions = np.arange(len(bits))
    fig_width = max(10, len(bits) * 0.2)

    fig, ax1 = plt.subplots(figsize=(fig_width, 8))

    ax1.bar(x_positions - bar_width, values_0to1, bar_width, label="0to1", color="tab:blue")
    ax1.bar(x_positions, values_1to0, bar_width, label="1to0", color="tab:orange")
    ax1.bar(x_positions + bar_width, averages, bar_width, label="average", color="tab:green")

    ax1.legend(loc="upper left")
    ax1.set_ylabel("Absolute Value (bits switched)", fontsize=14, labelpad=10)
    ax1.yaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{int(x):,}"))
    ax1.set_xlabel("Bit number in data bus", fontsize=16, labelpad=10)
    ax1.tick_params(axis='x', labelsize=10)
    ax1.tick_params(axis='y', labelsize=10)

    ax2 = ax1.twinx()
    ax2.set_ylabel("Percentage of Total Memory", fontsize=14, labelpad=10)
    ax2.set_ylim(0, 100)
    ax2.yaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:.0f}%"))
    ax2.tick_params(axis='y', labelsize=10)

    ax1.set_xticks(x_positions)
    ax1.set_xticklabels(bits, rotation=45, ha="right")
    ax1.set_xlim([-0.5, len(bits) - 0.5])

    plt.tight_layout()

    # Save to a temporary file
    chart_path = os.path.join(temp_dir, f"{file_stem}.png")
    plt.savefig(chart_path)

    # Optionally save a separate PNG
    if save_pngs:
        png_output_folder = os.path.join(output_folder, "chart_pngs")
        os.makedirs(png_output_folder, exist_ok=True)
        png_output_path = os.path.join(png_output_folder, f"{file_stem}.png")
        plt.savefig(png_output_path)
        print(f"Standalone PNG saved to: {png_output_path}")

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


def process_csv(input_csv, ods_doc, temp_dir, save_pngs, output_folder):
    file_stem = os.path.splitext(os.path.basename(input_csv))[0]  # Default file name without extension

    # Read and process the CSV file
    with open(input_csv, 'r', encoding='utf-8') as f_in:
        reader = csv.reader(f_in, delimiter=',')
        rows = list(reader)

    # Extract metadata: ProductName, Temperature, Time
    product_name = None
    temperature = None
    time = None

    for row in rows:
        if len(row) > 1:
            if row[0].strip() == "ProductName":
                product_name = row[1].strip()
            elif row[0].strip() == "Temperature":
                try:
                    temperature = float(row[1].strip())
                except ValueError:
                    temperature = None
            elif row[0].strip() == "Time":
                try:
                    time = float(row[1].strip())
                except ValueError:
                    time = None

    # Determine the sheet and PNG names
    if product_name and temperature is not None and time is not None:
        sheet_name = f"temp_{temperature}_time_{time}"
    else:
        sheet_name = file_stem

    # Extract "Total compared bits" from the CSV
    total_bits = None
    for i, row in enumerate(rows):
        if row and len(row) > 1 and row[1].strip() == "Total compared bits":
            try:
                # Extract the value from the second column of the next row
                raw_value = rows[i + 1][1].strip()
                # Remove leading ' if present and convert to int
                total_bits = int(raw_value.lstrip("'"))
                break
            except (IndexError, ValueError):
                print(f"Error: Invalid 'Total compared bits' value in {input_csv}")
                total_bits = None
                break

    # Default to 1 if "Total compared bits" is missing or invalid
    if total_bits is None or total_bits <= 0:
        print(f"Warning: 'Total compared bits' not found or invalid in {input_csv}. Defaulting to 1.")
        total_bits = 1

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

    # Generate the bar chart, passing total_bits
    chart_path = generate_bar_chart(numeric_data, temp_dir, sheet_name, save_pngs, output_folder, total_bits)

    # Add processed data and chart to ODS file
    write_to_ods(ods_doc, sheet_name, processed_rows, chart_path)

    # Return product name, temperature, and time for ODS naming
    return product_name, temperature, time

def main():
    parser = argparse.ArgumentParser(description="Process CSV files and generate ODS file with optional PNGs.")
    parser.add_argument("input_folder", help="Folder containing the CSV files to process.")
    parser.add_argument("--output-folder", required=True, help="Folder to save the ODS file and PNGs. Must be explicitly specified.")
    parser.add_argument("--save-pngs", action="store_true", help="Save standalone PNG files in addition to embedding in ODS.")
    args = parser.parse_args()

    input_folder = args.input_folder
    output_folder = args.output_folder
    save_pngs = args.save_pngs

    # Ensure the output folder exists
    os.makedirs(output_folder, exist_ok=True)

    # Prepare a temporary directory for charts
    with tempfile.TemporaryDirectory() as temp_dir:
        # Placeholder for the ODS file name
        ods_name = "processed_data"
        product_name_found = False

        # Create an ODS file
        ods_doc = OpenDocumentSpreadsheet()

        # Process all CSV files in the input folder
        for file_name in os.listdir(input_folder):
            input_path = os.path.join(input_folder, file_name)
            if os.path.isfile(input_path) and file_name.endswith(".csv"):
                print(f"Processing: {input_path}")
                product_name, temperature, time = process_csv(input_path, ods_doc, temp_dir, save_pngs, output_folder)

                # Update ODS name if metadata is found
                if product_name and not product_name_found:
                    ods_name = product_name
                    product_name_found = True

        # Save the ODS file
        ods_file_path = os.path.join(output_folder, f"{ods_name}.ods")
        ods_doc.save(ods_file_path)
        print(f"All processed data saved to: {ods_file_path}")

if __name__ == "__main__":
    main()
