#include <efi.h>
#include <efilib.h>

/* As defined per SMBIOS 2.3, we don't care about further fields */
#pragma pack(1)
typedef struct {
	SMBIOS_HEADER   Hdr;
	UINT16          PhysicalMemoryArrayHandle;
	UINT16          MemoryErrorInformationHandle;
	UINT16          TotalWidth;
	UINT16          DataWidth;
	UINT16          Size;
	UINT8           FormFactor;
	UINT8           DeviceSet;
	SMBIOS_STRING   DeviceLocator;
	SMBIOS_STRING   BankLocator;
	UINT8           MemoryType;
	UINT16          TypeDetail;
	UINT16          Speed;
	SMBIOS_STRING   Manufacturer;
	SMBIOS_STRING   SerialNumber;
	SMBIOS_STRING   AssetTag;
	SMBIOS_STRING   PartNumber;
} SMBIOS_TYPE17;
#pragma pack()

/* This isn't part of gnu-efi, and AsciiVSPrint doesn't handle '\n' properly. */
static UINTN AsciiSPrint (
	OUT CHAR8         *Str,
	IN UINTN          StrSize,
	IN CONST CHAR8    *fmt,
	...
	)
{
	va_list       args;
	UINTN         len;
	va_start (args, fmt);
	len = AsciiVSPrint(Str, StrSize, fmt, args);
	va_end (args);

	/*
	 * Every '\n' is changed to '\r\r\n' by AsciiVSPrint. Code below fixes the
	 * trailing one, but internal or multiple newline characters aren't handled.
	 */
	if (len >= 3 &&
	    Str[len-3] == '\r' && Str[len-2] == '\r' && Str[len-1] == '\n') {
		Str[len-3] = '\n';
		Str[len-2] = '\0';
		len -= 2;
	}

	return len;
}

#define PAGE_SIZE 0x1000
#define ADDR_4G 0x100000000ULL
#define ADDR_16M 0x1000000ULL
#define PAGES_16M 0x1000

static VOID Halt()
{
	while (1) asm volatile("cli; hlt" ::: "memory");
}

#define __L(x)	L##x
#define _L(x)	__L(x)
#define LFILE	_L(__FILE__)

#define Assert(exp)                                                    \
     ((exp)                                                            \
         ? ((VOID) 0)                                                  \
         : (Print(L"Assertion failed: %s:%d: %s\n",                    \
                  LFILE, __LINE__, _L(#exp)),                          \
            Halt()))

static UINTN lfsr;

static UINT64 Pattern (VOID)
{
	/* Taps: 64,63,61,60; feedback polynomial: x^64 + x^63 + x^61 + x^60 + 1. */
	UINT64 bit = ((lfsr >> 0) ^ (lfsr >> 1) ^ (lfsr >> 3) ^ (lfsr >> 4)) & 1u;

	lfsr ^= (lfsr >> 1) | (bit << 63);

	return lfsr;
}

static VOID StirPattern (UINT64 Seed)
{
	/* Random mask, breaks the pattern and makes at least one bit set. */
	lfsr = Seed ^ 0x7DEF56A18BC1A1E5ULL;

	for (UINTN I=0; I<50; I++)
		Pattern();

	lfsr = Pattern();
}

/*
 * WARNING: sizeof(EFI_MEMORY_DESCRIPTOR) isn't the same as DescSize.
 * In efiapi.h there is a macro: NextMemoryDescriptor(Ptr,Size), use it
 * instead. Because of that, mmap for N entries isn't actually big enough
 * for N entries.
 *
 * https://forum.osdev.org/viewtopic.php?f=1&t=32953
 * https://edk2-devel.narkive.com/BMqVNNak/efi-memory-descriptor-8-byte-padding-on-x86-64
 */
#define MEMORY_DESC_MAX		200

static EFI_MEMORY_DESCRIPTOR Mmap[MEMORY_DESC_MAX];
static UINTN MmapEntries = 0;
static UINTN TotalPages = 0;
static UINTN PagesDone = 0;

static VOID UpdateTotalPages(VOID)
{
	TotalPages = 0;
	for (UINTN I = 0; I < MmapEntries; I++)
		TotalPages += Mmap[I].NumberOfPages;
}

static VOID InitMemmap (VOID)
{
	UINTN MMSize = sizeof(Mmap);
	UINTN MapKey;
	UINTN DescSize;
	UINT32 DescVer;
	EFI_MEMORY_DESCRIPTOR *Desc;
	EFI_STATUS Status;

	Status = uefi_call_wrapper(gBS->GetMemoryMap, 5, &MMSize, Mmap, &MapKey,
							   &DescSize, &DescVer);
	if (Status != EFI_SUCCESS) {
		Print(L"Error obtaining the memory map: %r\n", Status);
		return;
	}

	Assert(DescVer == EFI_MEMORY_DESCRIPTOR_VERSION);
	Assert(DescSize >= sizeof(EFI_MEMORY_DESCRIPTOR));
	Assert(MMSize <= MEMORY_DESC_MAX * sizeof(EFI_MEMORY_DESCRIPTOR));
	Assert((MMSize % DescSize) == 0);

	/*
	 * Defragment memory map for easier iteration later on, leaving only
	 * unavailable RAM regions that are unlikely to change between boots.
	 */
	for (Desc = Mmap; (UINT8 *)Desc < (UINT8 *)Mmap + MMSize;
	     Desc = NextMemoryDescriptor(Desc, DescSize)) {
		if (Desc->Type == EfiConventionalMemory) {
			/* Skip regions smaller than 16MB, they tend to change. */
			if (Desc->NumberOfPages < PAGES_16M)
				continue;
			/*
			 * Skip regions between this application and 4GB, this is where
			 * firmware usually operates, and edk2 is unpredictable.
			 */
			if (Desc->PhysicalStart < ADDR_4G &&
			    Desc->PhysicalStart > (UINTN)&InitMemmap)
				continue;
			/*
			 * Align base (up) and size (down) to multiple of 16MB, just in
			 * case. Check if aligned size is still big enough.
			 */
			Desc->NumberOfPages -= PAGES_16M;
			Desc->NumberOfPages += (Desc->PhysicalStart & (ADDR_16M - 1)) /
			                       PAGES_16M;
			Desc->NumberOfPages &= ~(PAGES_16M - 1);
			Desc->PhysicalStart += ADDR_16M - 1;
			Desc->PhysicalStart &= ~(ADDR_16M - 1);
			if (Desc->NumberOfPages < PAGES_16M)
				continue;

			Print(L"Available RAM [%16llx - %16llx]\n", Desc->PhysicalStart,
				  Desc->PhysicalStart + Desc->NumberOfPages * PAGE_SIZE - 1);
			/*
			 * This is safe: CopyMem handles overlapping memory regions, asserts
			 * above made sure that size of memory descriptor is not bigger than
			 * DescSize, and Mmap[MmapEntries] will always be pointing behind
			 * Desc (except possibly first iteration, when they are equal).
			 */
			CopyMem(&Mmap[MmapEntries], Desc, sizeof(EFI_MEMORY_DESCRIPTOR));
			MmapEntries++;
		}
	}

	UpdateTotalPages();
	Print(L"Found %lld pages of available RAM (%lld MB)\n",
		  TotalPages, TotalPages >> 8);
}

static VOID WriteOneEntry (UINTN I)
{
	for (UINTN P = 0; P < Mmap[I].NumberOfPages; P++) {
		UINT64 *Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		StirPattern((UINT64)Ptr);
		for (UINTN Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			*Ptr = Pattern();
			Ptr++;
		}

		PagesDone++;
		Print(L"\r... %3.3d%%", (PagesDone * 100)/TotalPages);
	}
}

static VOID ExcludeRange (UINTN I, UINT64 Base, UINT64 NumPages)
{
	Print(L"\nExcluding range @ %llx, %llx pages\n", Base, NumPages);
	/*
	 * There are 4 cases, sorted by increasing complexity:
	 * 1. Excluded range is at the end of an entry. Decrease entry's
	 *    NumberOfPages.
	 * 2. Excluded range is at the beginning of an entry. Decrease entry's
	 *    NumberOfPages and increase PhysicalStart.
	 * 3. Whole Mmap entry is removed. Decrease MmapEntries by one and shift
	 *    (copy) the remaining entries down by one place.
	 * 4. Excluded range is in the middle of an entry. Entry must be split into
	 *    two entries. 1st new entry has PhysicalStart same as the original and
	 *    the NumberOfPages modified to end just before Base. 2nd entry has
	 *    PhysicalStart equal to end of excluded range and NumberOfPages equal
	 *    to original NumberOfPages reduced by sum of NumPages and 1st entry's
	 *    NumberOfPages. MmapEntries is increased (up to MEMORY_DESC_MAX),
	 *    remaining entries are shifted (copied) up by one place, original entry
	 *    is overwritten by 1st new entry and 2nd entry is written immediately
	 *    after that.
	 *
	 * Case 3 is a subset of both cases 1 and 2, so it must be checked before
	 * them.
	 */
	EFI_MEMORY_DESCRIPTOR *OrigEntry = &Mmap[I];
	EFI_MEMORY_DESCRIPTOR NewEntries[2] = {0};

	Assert (OrigEntry->PhysicalStart <= Base);
	Assert (OrigEntry->NumberOfPages >= NumPages);
	Assert (OrigEntry->PhysicalStart + OrigEntry->NumberOfPages * PAGE_SIZE >=
	        Base + NumPages * PAGE_SIZE);

	if (Base == OrigEntry->PhysicalStart &&
	    NumPages == OrigEntry->NumberOfPages) {
		/* Case 3. */
		/*
		 * Test for strictly greater than 1, so we won't end up with
		 * MmapEntries == 0 after the operation.
		 */
		Assert (MmapEntries > 1);
		/* Safe, last entry would result in size equal to 0, no need to test. */
		CopyMem (OrigEntry, OrigEntry + 1,
		         (MmapEntries - I - 1) * sizeof(EFI_MEMORY_DESCRIPTOR));
		MmapEntries--;
	} else if (Base + NumPages * PAGE_SIZE ==
	           OrigEntry->PhysicalStart + OrigEntry->NumberOfPages * PAGE_SIZE) {
		/* Case 1. */
		OrigEntry->NumberOfPages -= NumPages;
	} else if (Base == OrigEntry->PhysicalStart &&
	           NumPages != OrigEntry->NumberOfPages) {
		/* Case 2. */
		OrigEntry->NumberOfPages -= NumPages;
		OrigEntry->PhysicalStart += NumPages * PAGE_SIZE;
	} else {
		/* Case 4. */
		Assert (MmapEntries < MEMORY_DESC_MAX);
		/* Create new entries with original one used as a template. */
		NewEntries[0] = *OrigEntry;
		NewEntries[0].NumberOfPages = (Base - OrigEntry->PhysicalStart) /
		                              PAGE_SIZE;
		NewEntries[1] = *OrigEntry;
		NewEntries[1].PhysicalStart = Base + NumPages * PAGE_SIZE;
		NewEntries[1].NumberOfPages = OrigEntry->NumberOfPages -
		                              NewEntries[0].NumberOfPages -
		                              NumPages;
		/*
		 * Move remaining entries to make room for new ones. Safe,
		 * I = MmapEntries - 1 would result in size equal to 0 so no copy,
		 * otherwise OrigEntry is at most pointing to Mmap[MmapEntries - 2],
		 * and Assert() above makes sure that
		 * OrigEntry + 2 < Mmap[MEMORY_DESC_MAX - 2 + 2], so
		 * OrigEntry + 2 < Mmap[MEMORY_DESC_MAX].
		 */
		CopyMem (OrigEntry + 2, OrigEntry + 1,
		         (MmapEntries - I - 1) * sizeof(EFI_MEMORY_DESCRIPTOR));
		/* Insert new entries and update number of entries. */
		CopyMem (OrigEntry, NewEntries, sizeof(NewEntries));
		MmapEntries++;
	}
}

static VOID ExcludeOneEntry (UINTN I)
{
	BOOLEAN WasSame = TRUE;
	UINT64 First = (UINT64)-1, Last = 0;
	UINT64 *Ptr;
	for (UINTN P = 0; P < Mmap[I].NumberOfPages; P++) {
		Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		StirPattern((UINT64)Ptr);
		for (UINTN Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			UINT64 Expected = Pattern();

			if (*Ptr != Expected) {
				if (WasSame == TRUE || (P == 0 && Q == 0)) {
					First = (UINT64)Ptr & ~(UINT64)(PAGE_SIZE - 1);
				}
				WasSame = FALSE;
			} else {
				if (WasSame == FALSE) {
					/*
					 * Last is actually the first address on new page that is
					 * the same as expected. This makes it easier to convert to
					 * number of pages.
					 */
					Last = (UINT64)Ptr + PAGE_SIZE - 1;
					Last &= ~(UINT64)(PAGE_SIZE - 1);

					ExcludeRange (I, First, (Last - First) / PAGE_SIZE);
					First = (UINT64)-1;
					Last = 0;
				}
				WasSame = TRUE;
			}
			Ptr++;
		}
		PagesDone++;
		Print(L"\r... %3.3d%%", (PagesDone * 100)/TotalPages);
	}
	if (First != (UINT64)-1) {
		ExcludeRange (I, First, ((UINT64)Ptr - First) / PAGE_SIZE);
	}
}

static UINT64 Differences = 0;
static UINT64 Compared = 0;
static UINT64 OneToZero[64];
static UINT64 ZeroToOne[64];

static VOID CompareOneEntry (UINTN I)
{
	for (UINTN P = 0; P < Mmap[I].NumberOfPages; P++) {
		UINT64 *Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		StirPattern((UINT64)Ptr);
		for (UINTN Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			UINT64 Expected = Pattern();
			if (*Ptr != Expected) {
				Expected ^= *Ptr;
				for (UINT64 I = 0; I < 64; I++) {
					UINT64 Tmp = 1ULL << I;
					if (Expected & Tmp) {
						if (*Ptr & Tmp) {
							ZeroToOne[I]++;
						} else {
							OneToZero[I]++;
						}
					}
				}
			}
			Ptr++;
		}
		PagesDone++;
		Print(L"\r... %3.3d%%", (PagesDone * 100)/TotalPages);
	}

	Compared += Mmap[I].NumberOfPages * PAGE_SIZE * 8;
}

static VOID GetFileName(CHAR16 *Name)
{
	EFI_TIME Time;

	uefi_call_wrapper(gRT->GetTime, 2, &Time, NULL);

	UnicodeSPrint(Name, 0, L"%04d_%02d_%02d_%02d_%02d.csv",
	              Time.Year, Time.Month, Time.Day,
	              Time.Hour, Time.Minute);
}

static VOID CreateResultFile(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **Csv)
{
	EFI_LOADED_IMAGE *Loaded = NULL;
	EFI_FILE_PROTOCOL *Root = NULL;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs = NULL;
	CHAR16 Name[25];
	EFI_STATUS Status;
	CHAR8 Header[] = "Bit, 0to1, 1to0\n";
	UINTN Len = sizeof(Header) - 1;

	Status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle,
	                           &LoadedImageProtocol, (VOID **)&Loaded);
	Assert(Status == EFI_SUCCESS);
	Assert(Loaded != NULL);

	Status = uefi_call_wrapper(gBS->HandleProtocol, 3, Loaded->DeviceHandle,
	                           &gEfiSimpleFileSystemProtocolGuid,
	                           (VOID **)&SimpleFs);
	Assert(SimpleFs != NULL);

	Status = uefi_call_wrapper(SimpleFs->OpenVolume, 2, SimpleFs, &Root);
	Assert(Root != NULL);

	GetFileName(Name);

	Status = uefi_call_wrapper(Root->Open, 5, Root, Csv, Name,
	                           EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE |
	                           EFI_FILE_MODE_READ, 0);
	Assert(*Csv != NULL);

	Status = uefi_call_wrapper((*Csv)->Write, 3, *Csv, &Len, Header);
	Assert(Status == EFI_SUCCESS);
}

static VOID AddResultLine(EFI_FILE_PROTOCOL *Csv, UINT64 Bit,
                          UINT64 ZerosToOnes, UINT64 OnesToZeros)
{
	/*
	 * UINT64 has up to 20 digits in decimal. Bit will always have up to 2
	 * digits, but better safe than sorry.
	 */
	CHAR8 Str[70];
	UINTN Len;
	EFI_STATUS Status;

	Len = AsciiSPrint(Str, 70, "%lld,%lld,%lld\n", Bit, ZerosToOnes,
	                  OnesToZeros);

	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Str);
	Assert(Status == EFI_SUCCESS);
}

static SMBIOS_STRUCTURE_POINTER GetNextSmbiosStruct (
  SMBIOS3_STRUCTURE_TABLE *Smbios,
  SMBIOS_STRUCTURE_POINTER Ptr
  )
{
	/* Skip formatted area */
	Ptr.Raw += Ptr.Hdr->Length;
	if ((UINT64)Ptr.Raw >= Smbios->TableAddress + Smbios->TableMaximumSize) {
		Ptr.Raw = NULL;
		return Ptr;
	}

	/* Find end of strings marker */
	while (*Ptr.Raw != '\0' || *(Ptr.Raw + 1) != '\0')
		Ptr.Raw++;

	/* Skip end of strings marker */
	Ptr.Raw += 2;

	if ((UINT64)Ptr.Raw >= Smbios->TableAddress + Smbios->TableMaximumSize) {
		Ptr.Raw = NULL;
		return Ptr;
	}

	return Ptr;
}

static CHAR8 *GetProductName(VOID)
{
	SMBIOS3_STRUCTURE_TABLE *SmbiosTable = NULL;
	SMBIOS_STRUCTURE_POINTER Ptr;

	LibGetSystemConfigurationTable (&SMBIOS3TableGuid, (VOID **)&SmbiosTable);
	Ptr.Raw = (UINT8 *)SmbiosTable->TableAddress;

	while (Ptr.Raw != NULL && Ptr.Hdr->Type != 1)
		Ptr = GetNextSmbiosStruct(SmbiosTable, Ptr);

	if (Ptr.Raw == NULL || Ptr.Type1->ProductName == 0)
		return "unknown";

	return LibGetSmbiosString (&Ptr, Ptr.Type1->ProductName);
}

static CHAR8 *SmbiosString(SMBIOS_STRUCTURE_POINTER *Ptr, UINT16 Num)
{
	CHAR8 *Ret;

	if (Ptr == NULL || Num == 0)
		return "unknown";

	Ret = LibGetSmbiosString(Ptr, Num);
	if (Ret != NULL)
		return Ret;

	return "unknown";
}

static VOID StoreDimmsInfo(EFI_FILE_PROTOCOL *Csv)
{
	CHAR8 Header[] = "\n\nDIMM info\nLocator, Bank Locator, Part Number\n";
	CHAR8 Str[200];
	UINTN Len = sizeof(Header) - 1;
	SMBIOS3_STRUCTURE_TABLE *SmbiosTable = NULL;
	SMBIOS_STRUCTURE_POINTER Ptr;
	SMBIOS_TYPE17 *T17;
	EFI_STATUS Status;

	LibGetSystemConfigurationTable (&SMBIOS3TableGuid, (VOID **)&SmbiosTable);
	Ptr.Raw = (UINT8 *)SmbiosTable->TableAddress;

	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Header);
	Assert(Status == EFI_SUCCESS);

	while (TRUE) {
		if (Ptr.Raw == NULL) break;
		if (Ptr.Hdr->Type != 17) {
			Ptr = GetNextSmbiosStruct(SmbiosTable, Ptr);
			continue;
		}

		/*
		 * According to SMBIOS specification, one such table should be created
		 * per DIMM slot, regardless of whether the slot is currently populated
		 * or not. What coreboot does is a different story. Code in 'lib' is
		 * ready to produce descriptions for empty slots, but 'soc/intel' skips
		 * unpopulated slots as if they didn't exist.
		 */
		T17 = (SMBIOS_TYPE17 *)Ptr.Raw;
		Len = AsciiSPrint (Str, sizeof(Str), "\"%a\",\"%a\",\"%a\"\n",
		                   SmbiosString(&Ptr, T17->DeviceLocator),
		                   SmbiosString(&Ptr, T17->BankLocator),
		                   SmbiosString(&Ptr, T17->PartNumber)
		                  );

		Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Str);
		Assert(Status == EFI_SUCCESS);

		Ptr = GetNextSmbiosStruct(SmbiosTable, Ptr);
	}

	/* Empty line */
	Len = 1;
	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Header);
	Assert(Status == EFI_SUCCESS);
}

static VOID FinalizeResults(EFI_FILE_PROTOCOL *Csv)
{
	CHAR8 Footer[] = "\n\nDifferent bits, Total compared bits\n";
	CHAR8 Str[100];
	CHAR16 LStr[100];
	CHAR16 InStr[10];
	UINTN Len = sizeof(Footer) - 1;
	EFI_STATUS Status;

	/* Footer */
	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Footer);
	Assert(Status == EFI_SUCCESS);

	/* Statistics */
	Len = AsciiSPrint(Str, 100, "%lld,%lld\n", Differences, Compared);

	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Str);
	Assert(Status == EFI_SUCCESS);

	/* Pad with few empty rows, reusing footer */
	Len = 2;
	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Footer);
	Assert(Status == EFI_SUCCESS);

	/* Platform product name */
	Len = AsciiSPrint(Str, 100, "ProductName,\"%a\"\n", GetProductName());

	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Str);
	Assert(Status == EFI_SUCCESS);

	/* Store information about populated memory */
	StoreDimmsInfo(Csv);

	/* Flush before allowing users to do something unexpected */
	Status = uefi_call_wrapper(Csv->Flush, 1, Csv);
	Assert(Status == EFI_SUCCESS);

	Print(L"%H");
	/* Prompt and save temperature */
	Input(L"Ambient temperature: ", InStr, 10);
	Print(L"\n");
	Len = AsciiSPrint(Str, 100, "Temperature,\"%s\"\n", InStr);

	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Str);
	Assert(Status == EFI_SUCCESS);

	/* Flush before allowing users to do something unexpected */
	Status = uefi_call_wrapper(Csv->Flush, 1, Csv);
	Assert(Status == EFI_SUCCESS);

	/* Prompt and save power-off time */
	Input(L"Time (in seconds) without power: ", InStr, 10);
	Print(L"\n");
	Len = AsciiSPrint(Str, 100, "Time,\"%s\"\n", InStr);

	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Str);
	Assert(Status == EFI_SUCCESS);

	/* Flush before allowing users to do something unexpected */
	Status = uefi_call_wrapper(Csv->Flush, 1, Csv);
	Assert(Status == EFI_SUCCESS);

	/* Prompt for other comments, 96 + 2 * '"' + '\n' + '\0' = 100 */
	Input(L"Comments (max 96 characters, leave empty to skip): ", LStr, 96);
	Print(L"\n");
	Len = AsciiSPrint(Str, 100, "\"%s\"\n", LStr);
	Print(L"%N");

	Status = uefi_call_wrapper(Csv->Write, 3, Csv, &Len, Str);
	Assert(Status == EFI_SUCCESS);

	/* Close the file, which flushes it to disk */
	Status = uefi_call_wrapper(Csv->Close, 1, Csv);
	Assert(Status == EFI_SUCCESS);
}

/* No EFIAPI here. Not sure why, but gnu-efi converts this to SysV */
EFI_STATUS
efi_main (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_STATUS Status = EFI_SUCCESS;
	EFI_INPUT_KEY Key;
	EFI_GUID VarGuid = { 0x865a4a83, 0x19e9, 0x4f5b, {0x84, 0x06, 0xbc, 0xa0, 0xdb, 0x86, 0x91, 0x5e} };
	CHAR16 VarName[] = L"TestedMemoryMap";
	UINTN VarSize;
	UINT32 NVAttr = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE;

	InitializeLib(ImageHandle, SystemTable);

	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

	/* Disable watchdog so it won't reboot the platform after 20 minutes. */
	Status = uefi_call_wrapper(gBS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
	if (Status != EFI_SUCCESS) {
		Print(L"Error disabling the watchdog: %r\n", Status);
		return Status;
	}

	Print(L"Application for testing RAM data decay\n");

	InitMemmap();

	Print(L"\n\nChoose the mode:\n");
	Print(L"%H1%N. Pattern write\n");
	Print(L"%H2%N. Exclude modified by firmware\n");
	Print(L"%H3%N. Pattern compare\n\n");

	WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
	uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);

	while (Key.UnicodeChar < L'1' || Key.UnicodeChar > L'3') {
		WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
		uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
	}

	if (Key.UnicodeChar == L'1') {
		Print(L"Pattern write was selected\n");
		for (UINTN I = 0; I < MmapEntries; I++) {
			WriteOneEntry(I);
		}
		Print(L"\nPattern write done\n");
	} else if (Key.UnicodeChar == L'2') {
		Print(L"Exclude modified by firmware was selected\n");
		for (UINTN I = 0; I < MmapEntries; I++) {
			ExcludeOneEntry(I);
		}

		VarSize = MmapEntries * sizeof(EFI_MEMORY_DESCRIPTOR);
		Status = uefi_call_wrapper(gRT->SetVariable, 5, VarName, &VarGuid,
		                           NVAttr, VarSize, Mmap);
		Assert (Status == EFI_SUCCESS);
		Print(L"\nExclude modified by firmware done\n");
	} else if (Key.UnicodeChar == L'3') {
		EFI_FILE_PROTOCOL *Csv = NULL;
		Print(L"Pattern compare was selected\n");
		VarSize = sizeof(Mmap);
		Status = uefi_call_wrapper(gRT->GetVariable, 5, VarName, &VarGuid,
		                           NULL, &VarSize, Mmap);
		Assert (Status == EFI_SUCCESS);
		Assert (VarSize % sizeof(EFI_MEMORY_DESCRIPTOR) == 0);
		MmapEntries = VarSize / sizeof(EFI_MEMORY_DESCRIPTOR);
		UpdateTotalPages();

		for (UINTN I = 0; I < MmapEntries; I++) {
			CompareOneEntry(I);
		}

		Status = uefi_call_wrapper(gRT->SetVariable, 5, VarName, &VarGuid,
		                           0, 0, NULL);
		Assert (Status == EFI_SUCCESS);
		Print(L"\nPattern comparison done\n");

		/*
		 * We no longer care about memory map or preservation of memory. Safe
		 * to use firmware services again at this point.
		 */
		CreateResultFile(ImageHandle, &Csv);

		Print(L"\nPer bit differences:\n");
		for (UINTN I = 0; I < 64; I++) {
			Differences += ZeroToOne[I] + OneToZero[I];
			Print(L"%2d: %16lld 0to1, %16lld 1to0, %16lld total\n", I,
			      ZeroToOne[I], OneToZero[I], ZeroToOne[I] + OneToZero[I]);
			AddResultLine(Csv, I, ZeroToOne[I], OneToZero[I]);
		}

		Print(L"\n%lld/%lld different bits (%E%2lld.%02.2lld%%%N)\n",
		      Differences, Compared, (Differences * 100) / Compared,
		      ((Differences * 10000) / Compared) % 100);
		FinalizeResults(Csv);
	}

	/* Make sure data is actually written to RAM. */
	asm volatile("wbinvd" ::: "memory");

	/* Parse memmap again to see if it has changed. */
	MmapEntries = 0;
	TotalPages = 0;
	InitMemmap();

	Print(L"\nPress %HR%N to reboot, %HS%N to shut down\n");
	WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
	uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);

	while (Key.UnicodeChar != L'r' && Key.UnicodeChar != L's') {
		WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
		uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
	}

	if (Key.UnicodeChar == L's')
		Status = uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS,
		                           0, NULL);

	Status = uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetWarm, EFI_SUCCESS,
	                           0, NULL);

	return Status;
}
