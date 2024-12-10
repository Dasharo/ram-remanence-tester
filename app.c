#include <efi.h>
#include <efilib.h>

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

static VOID StirPattern (UINTN Seed)
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
		StirPattern((UINTN)Ptr);
		for (UINTN Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			*Ptr = Pattern();
			Ptr++;
		}

		PagesDone++;
		Print(L"\r... %3.3d%%", (PagesDone * 100)/TotalPages);
	}
}

static VOID ExcludeRange (UINTN I, UINTN Base, UINTN NumPages)
{
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
	UINTN First = (UINTN)-1, Last = 0;
	UINT64 *Ptr;
	for (UINTN P = 0; P < Mmap[I].NumberOfPages; P++) {
		Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		StirPattern((UINTN)Ptr);
		for (UINTN Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			UINTN Expected = Pattern();

			if (*Ptr != Expected) {
				if (WasSame == TRUE || (P == 0 && Q == 0)) {
					First = (UINTN)Ptr;
				}
				WasSame = FALSE;
			} else {
				if (WasSame == FALSE) {
					/*
					 * Last is actually the first address on new page that is
					 * the same as expected. This makes it easier to convert to
					 * number of pages.
					 */
					Last = (UINTN)Ptr + PAGE_SIZE - 1;
					Last &= ~(UINTN)(PAGE_SIZE - 1);

					ExcludeRange (I, First, (Last - First) / PAGE_SIZE);
					First = (UINTN)-1;
					Last = 0;
				}
				WasSame = TRUE;
			}
			Ptr++;
		}
		PagesDone++;
		Print(L"\r... %3.3d%%", (PagesDone * 100)/TotalPages);
	}
	if (First != (UINTN)-1) {
		ExcludeRange (I, First, ((UINTN)Ptr - First) / PAGE_SIZE);
	}
}

static UINTN Differences = 0;
static UINTN Compared = 0;
static UINTN OneToZero[64];
static UINTN ZeroToOne[64];

static VOID CompareOneEntry (UINTN I)
{
	for (UINTN P = 0; P < Mmap[I].NumberOfPages; P++) {
		UINT64 *Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		StirPattern((UINTN)Ptr);
		for (UINTN Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			UINTN Expected = Pattern();

			Compared += 64;
			if (*Ptr != Expected) {
				Expected ^= *Ptr;
				for (UINTN I = 0; I < 64; I++) {
					if ((Expected >> I) & 1) {
						ZeroToOne[I] +=  ((*Ptr >> I) & 1);
						OneToZero[I] += !((*Ptr >> I) & 1);
						Differences++;
					}
				}
			}
			Ptr++;
		}
		PagesDone++;
		Print(L"\r... %3.3d%%", (PagesDone * 100)/TotalPages);
	}
}

EFI_STATUS
EFIAPI
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
	Print(L"1. Pattern write\n");
	Print(L"2. Exclude modified by firmware\n");
	Print(L"3. Pattern compare\n\n");

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
		Print(L"%lld/%lld different bits (%lld%%)\n", Differences, Compared,
		      (Differences * 100) / Compared);
		Print(L"\nPer bit:\n");
		for (UINTN I = 0; I < 64; I++) {
			Print(L"%2d: %16lld 0to1, %16lld 1to0, %16lld total\n", I,
			      ZeroToOne[I], OneToZero[I], ZeroToOne[I] + OneToZero[I]);
		}
	}

	/* Make sure data is actually written to RAM. */
	asm volatile("wbinvd" ::: "memory");

	/* Parse memmap again to see if it has changed. */
	MmapEntries = 0;
	TotalPages = 0;
	InitMemmap();

	Print(L"\nPress any key to reboot\n");
	WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
	Status = uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetWarm, EFI_SUCCESS,
	                           0, NULL);

	return Status;
}
