# RAM data remanence tester

This UEFI application was made to help with testing RAM data remanence after
powering off the platform.

## Building

Prerequisites:

- `gcc`, `make` & friends
- `gnu-efi`
- `gnu-efi-devel`

It is known to work with GCC 14.2.1 and both `gnu-efi` packages in version
3.0.18, as found in Fedora 40. Older versions of `gnu-efi` may require different
build options than those defined in Makefile.

To build, simply run:

```shell
make
```

This will produce `BOOTx64.EFI` file that should be copied to USB drive
formatted as FAT32 (not exFAT) to `/EFI/BOOT/` directory.

## Use

Plug in the drive and boot the tested platform from it. It will show simple menu
with 3 options to choose:

```text
Application for testing RAM data decay
Available RAM [         2000000 -          2FFFFFF]
Available RAM [         4000000 -          5FFFFFF]
Found 12288 pages of available RAM (48 MB)


Choose the mode:
1. Pattern write
2. Exclude modified by firmware
3. Pattern compare
```

They must be run in order, and with specific types of reboot (or shutdown) in
between.

> This particular output comes from QEMU OVMF. Usually, there will be more RAM
> available for testing.

### Step 1

In this step, detected available memory is written with predictable pattern,
based on physical memory address. The pattern is generated using linear-feedback
shift register (LFSR) algorithm to generate in relatively short time
random-like, yet predictable patterns of uniformly distributed bit streams. In
other words, the pattern doesn't contain long streams of the same bit, isn't
constant and isn't (for the lengths required here) repetitive.

The progress is reported, and after all memory is written, the memory map is
obtained again (to check whether it has changed significantly by calling UEFI
services), and the app asks whether to do a warm reboot or shut down the
platform:

```text
Pattern write was selected
... 100%
Pattern write done
Available RAM [         2000000 -          2FFFFFF]
Available RAM [         4000000 -          5FFFFFF]
Found 12288 pages of available RAM (48 MB)

Press R to reboot, S to shut down
```

After this step, always choose to reboot.

> Q: Why doesn't the app reboot automatically?
>
> A: Few reasons. Firstly, it gives time to compare whether the memory map has
> changed. It happened a lot during development, and some decisions were made to
> counteract it, like discarding small regions and aligning the rest to 16 MB.
> Secondly, this gives time for RAM to cool down. If DRAM temperature is
> externally monitored, it is up to the user to decide when the temperature is
> low enough to continue. As to why shutdown is even an option, it is to made
> application simpler, and further reduce the possibility of different memory
> maps between different steps, caused by different code flow.

### Step 2

This step runs after a warm reboot, so memory contents are (usually) preserved.
However, firmware may allocate memory in regions marked as available, overwrite
it, and then free it before passing control to the application. In order to
find such regions, the application generates identical pattern as in step 1, but
instead of writing it, it is compared against existing memory content. If it
doesn't match, such region is excluded from memory map, and modified map is
stored in UEFI variable to be consumed by next step.

> There are (rather uncommon) security features that may make the old memory
> content unreadable, like full memory encryption with forced key change even on
> warm reboot. Such cases aren't supported by this code.

As before, the progress is printed (it may or may not be intertwined with lines
about excluded ranges), and the app asks about reboot type. Once again, this may
be used as a pause to let RAM cool down. In this case, reboot is not a good
option (except testing and debugging, or running in QEMU). Depending on test
case, there are few available options like:

- pressing `S` to let the platform perform a graceful shutdown,
- using the power button,
- removing the power cord.

After that, the power must be restored and platform must be powered back on.
The amount of time during which the RAM content can be preserved without power
depends on many factors, but it usually isn't long, sometimes even below a
second.

### Step 3

This step will do the actual comparison of memory against expected pattern,
taking the modified memory map from step 2 into account. The comparison is
similar to that of previous step, except this time some per-bit statistics are
saved for later analysis. It may take significantly longer to complete,
especially for larger amount of flipped bits. After the test finishes, the
application will show results and ask for few environmental factors, as well as
an optional free-form comment to be added to the results:

```text
62:         27046685 0to1,         27569254 1to0,         54615939 total
63:         27137124 0to1,         26885265 1to0,         54022389 total

3427191393/8724152320 different bits (39.28%)
Ambient temperature: 18.2
Time (in seconds) without power: 0
Comments (max 96 characters, leave empty to skip): no bat, PSU connected, power button immediately, boot after ~1s
```

Don't worry about counting the characters to the limit, any character above that
won't be echoed back. Apart from answers to those question, metadata describing
platform name and DIMM part number coming from SMBIOS will be stored.

Results are saved as CSV file on USB drive. Current date and time is used as a
file name. The format of file:

```text
Bit, 0to1, 1to0
0, 25585400, 25736273
1, 25987054, 25836528
(...)
62, 27046685, 27569254
63, 27137124, 26885265


Different bits, Total compared bits
3427191393, 8724152320


ProductName,"NV4xPZ"


DIMM info
Locator, Bank Locator, Part Number
"Channel-0-DIMM-0","BANK 0","78.D2GG7.4010B"

Temperature,"18.2"
Time,"0"
"no bat, PSU connected, power button immediately, boot after ~1s"
```

Once again, the application will ask whether to reboot or shut down. This time
use whatever suits you best, probably depending on whether further tests are to
be run or not.

### Post-test analysis

CSV by itself is hard to analyze. It may be imported to a spreadsheet
application in order to calculate real percentage of changed bits, or to draw
a chart of number of changes per bit location.

## Tips and quirks

To make testing faster, set the drive as first boot entry. Starting anything
else between steps (e.g. accidentally letting OS to boot) overwrites the memory,
and all steps must be repeated from the beginning.

The larger the size of installed memory, the longer it takes to run the tests.
Start small to get some basic idea about the time the platform can be powered
off before running it will all memory populated. For even faster testing, some
additional constraints may be added to `InitMemmap()` to further reduce amount
of tested memory.

The more swapped bits, the longer step 3 takes to complete. There is some room
for optimization, but it will always be longer than previous steps.

This application asserts on first sight of trouble. Some of most common issues:

- `MmapEntries < MEMORY_DESC_MAX` or `MmapEntries > 1` in step 2: a lot of
  memory has changed on warm reboot. Either some protection mechanism is used,
  or something very wrong with the firmware, or step 2 was started without
  running step 1.
- `*Csv != NULL` in step 3: read-only filesystem. Make sure your USB drive is
  formatted as FAT32.
- `Status == EFI_SUCCESS`: this check is so generic that exact line number must
  be compared, check in the code what is done before that. One of the usual
  suspects is running steps out of order.

## `plotter.py` - Automated Data Analysis and Visualization

The `plotter.py` script streamlines the analysis of CSV results generated by 
the RAM data remanence tester. It automatically parses test result files to 
generate corresponding graph visualizations and combine them into a single 
`.ods` spreadsheet for convenience.

### Usage

#### Prerequisites

Ensure the following tools and libraries are set up:

1. **Python Virtual Environment**:
   Create and activate a Python virtual environment before installing dependencies:
   ```bash
   python3 -m venv venv
   source venv/bin/activate
   ```

2. **Install Required Python Packages**:
   Install dependencies using the provided `requirements.txt` file:
   ```bash
   pip install -r requirements.txt
   ```

#### Running the Script

```bash
python plotter.py <input_folder> --output-folder <output_folder> [--save-pngs]
```

- `<input_folder>`: Folder containing CSV files to process.
- `--output-folder <output_folder>`: **Mandatory.** Folder where results (ODS and optionally PNGs) will be saved.
- `--save-pngs`: **Optional.** Save standalone PNG charts in `output_folder/chart_pngs/`.

#### Examples

1. **Basic Usage** (generate only the ODS file):
   ```bash
   python plotter.py ../csv/ --output-folder results
   ```

2. **Generate ODS + Standalone PNGs**:
   ```bash
   python plotter.py ../csv/ --output-folder results --save-pngs
   ```

## References

- [Report from initial testing](https://blog.3mdeb.com/2024/2024-12-13-ram-data-decay-research/)
- [Report from part 2 of research](https://blog.3mdeb.com/2025/2025-01-24-ram-data-decay-research-part2/)

## Credits

This research has been supported by Power Up Privacy, a privacy advocacy group
that seeks to supercharge privacy projects with resources so they can complete
their mission of making our world a better place.
