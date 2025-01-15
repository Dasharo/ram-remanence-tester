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
    ax2.set_ylabel("Percentage of Total Memory\n (Applies to average only) ", fontsize=14, labelpad=10)

    ax2.set_ylim(0, ax1.get_ylim()[1]/(total_bits/128) * 100) # 128, not 64, to include the average in calculations

    ax2.yaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:.0f}%"))
    ax2.tick_params(axis='y', labelsize=10)

    ax1.set_xticks(x_positions)
    ax1.set_xticklabels(bits, rotation=45, ha="right")
    ax1.set_xlim([-0.5, len(bits) - 0.5])

    plt.tight_layout()

    chart_path = os.path.join(temp_dir, f"{file_stem}.png")
    plt.savefig(chart_path)

    if save_pngs:
        png_output_folder = os.path.join(output_folder, "chart_pngs")
        os.makedirs(png_output_folder, exist_ok=True)
        png_output_path = os.path.join(png_output_folder, f"{file_stem}.png")
        plt.savefig(png_output_path)
        print(f"Standalone PNG saved to: {png_output_path}")

    plt.close()
    return chart_path


def write_to_ods(ods_doc, sheet_name, data, chart_path, total_flipped_bits, total_bits):
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
            table_cell.addElement(P(text=str(cell)))
            table_row.addElement(table_cell)
        table.addElement(table_row)

    # Ensure there are at least 69 rows
    while len(table.childNodes) < 69:
        table.addElement(TableRow())

    # Add "Average percentage" to C68
    row_68 = table.childNodes[67]  # Row 68 (0-indexed)
    c68_cell = TableCell()
    c68_cell.addElement(P(text="Average percentage"))
    row_68.addElement(c68_cell)

    # Add the average percentage to C69
    row_69 = table.childNodes[68]  # Row 69 (0-indexed)
    c69_cell = TableCell()
    if total_bits > 0:
        avg_percentage = (total_flipped_bits / total_bits) * 100
        c69_cell.addElement(P(text=f"{avg_percentage:.2f}%"))
    row_69.addElement(c69_cell)
    # Embed the chart as an image in the top row, new column
    if os.path.exists(chart_path):
        relative_path = ods_doc.addPicture(chart_path)
        with PIL.Image.open(chart_path) as img:
            img_width, img_height = img.size

        scale_factor = 0.7
        scaled_width_cm = (img_width * scale_factor) / 96 * 2.54
        scaled_height_cm = (img_height * scale_factor) / 96 * 2.54

        frame = Frame(
            width=f"{scaled_width_cm:.2f}cm",
            height=f"{scaled_height_cm:.2f}cm",
            x="0cm",
            y="0cm",
        )
        image = Image(href=relative_path, type="simple")
        frame.addElement(image)

        chart_row = table.firstChild
        if not chart_row:
            chart_row = TableRow()
            table.addElement(chart_row)

        chart_cell = TableCell()
        chart_cell.addElement(frame)
        chart_row.addElement(chart_cell)

    ods_doc.spreadsheet.addElement(table)


def process_csv(input_csv, ods_doc, temp_dir, save_pngs, output_folder):
    file_stem = os.path.splitext(os.path.basename(input_csv))[0]
    with open(input_csv, 'r', encoding='utf-8') as f_in:
        reader = csv.reader(f_in, delimiter=',')
        rows = list(reader)

    product_name, temperature, time = None, None, None
    for row in rows:
        if len(row) > 1:
            if row[0].strip() == "ProductName":
                product_name = row[1].strip()
            elif row[0].strip() == "Temperature":
                temperature = float(row[1].strip())
            elif row[0].strip() == "Time":
                time = float(row[1].strip())

    if product_name and temperature is not None and time is not None:
        sheet_name = f"temp_{temperature}_time_{time}"
    else:
        sheet_name = file_stem

    try:
        total_flipped_bits = int(rows[68][0].strip().lstrip("'"))
        total_bits = int(rows[68][1].strip().lstrip("'"))
    except (IndexError, ValueError):
        print(f"Warning: Missing or invalid 'Different bits'/'Total compared bits' in {input_csv}.")
        total_flipped_bits = 0
        total_bits = 1

    processed_rows = []
    header_found = False
    for row in rows:
        if not row:
            processed_rows.append(row)
            continue
        if not header_found and len(row) >= 3 and row[0].strip() == "Bit":
            row.append("average")
            header_found = True
        else:
            try:
                avg_val = (int(row[1].strip()) + int(row[2].strip())) / 2
                row.append(f"{avg_val:.1f}")
            except (ValueError, IndexError):
                pass
        processed_rows.append(row)

    numeric_data = [row for row in processed_rows if len(row) >= 4 and row[0].isdigit()]
    chart_path = generate_bar_chart(numeric_data, temp_dir, sheet_name, save_pngs, output_folder, total_bits)
    write_to_ods(ods_doc, sheet_name, processed_rows, chart_path, total_flipped_bits, total_bits)
    return product_name, temperature, time


def main():
    parser = argparse.ArgumentParser(description="Process CSV files and generate ODS file with optional PNGs.")
    parser.add_argument("input_folder", help="Folder containing the CSV files to process.")
    parser.add_argument("--output-folder", required=True, help="Folder to save the ODS file and PNGs.")
    parser.add_argument("--save-pngs", action="store_true", help="Save standalone PNG files.")
    args = parser.parse_args()

    input_folder = args.input_folder
    output_folder = args.output_folder
    save_pngs = args.save_pngs

    os.makedirs(output_folder, exist_ok=True)

    with tempfile.TemporaryDirectory() as temp_dir:
        ods_name = "processed_data"
        product_name_found = False

        ods_doc = OpenDocumentSpreadsheet()
        for file_name in os.listdir(input_folder):
            input_path = os.path.join(input_folder, file_name)
            if os.path.isfile(input_path) and file_name.endswith(".csv"):
                product_name, temperature, time = process_csv(input_path, ods_doc, temp_dir, save_pngs, output_folder)
                if product_name and not product_name_found:
                    ods_name = product_name
                    product_name_found = True

        ods_file_path = os.path.join(output_folder, f"{ods_name}.ods")
        ods_doc.save(ods_file_path)
        print(f"All processed data saved to: {ods_file_path}")


if __name__ == "__main__":
    main()
