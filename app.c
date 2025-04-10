#include <efi.h>
#include <efilib.h>

#define MAX_FILE_SIZE (UINT64)0xa0000000 /* 2684354560 bytes */

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

static VOID ShowProgress (VOID)
{
	static INTN Prev = -1;
	INTN Current = (PagesDone * 100)/TotalPages;
	if (Current != Prev) {
		Print(L"\r... %3.3d%%", Current);
		Prev = Current;
	}
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

        for (Desc = Mmap; (UINT8 *)Desc < (UINT8 *)Mmap + MMSize;
             Desc = NextMemoryDescriptor(Desc, DescSize)) {
                if (Desc->Type == EfiConventionalMemory) {
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

static VOID GetFileName(CHAR16 *Name, UINT64 AddressStart)
{
	EFI_TIME Time;

	uefi_call_wrapper(gRT->GetTime, 2, &Time, NULL);

	UnicodeSPrint(Name, 0, L"%04d_%02d_%02d_%02d_%02d_0x%016llx.csv",
	              Time.Year, Time.Month, Time.Day,
	              Time.Hour, Time.Minute, AddressStart);
}

static VOID CreateResultFile(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **File, CHAR16 *Name)
{
	EFI_LOADED_IMAGE *Loaded = NULL;
	EFI_FILE_PROTOCOL *Root = NULL;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs = NULL;
	EFI_STATUS Status;

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

	Status = uefi_call_wrapper(Root->Open, 5, Root, File, Name,
	                           EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE |
	                           EFI_FILE_MODE_READ, 0);
	Assert(*File != NULL);
}

static VOID WriteByte(EFI_FILE_PROTOCOL *File, UINT8 Byte)
{
	UINTN Len = (UINTN)sizeof(UINT8);
	EFI_STATUS Status;

	Status = uefi_call_wrapper(File->Write, 3, File, &Len, (void*)(&Byte));
	Assert(Status == EFI_SUCCESS);
}

static VOID FinalizeResults(EFI_FILE_PROTOCOL *File)
{
	EFI_STATUS Status;

	/* Close the file, which flushes it to disk */
	Status = uefi_call_wrapper(File->Close, 1, File);
	Assert(Status == EFI_SUCCESS);
}

static VOID DumpOneEntry (EFI_HANDLE ImageHandle, UINTN I)
{
	CHAR16 FileName[50];
	EFI_FILE_PROTOCOL *File = NULL;
	UINT64 CurrentFileSize = 0;

	GetFileName(FileName, Mmap[I].PhysicalStart * PAGE_SIZE);
	CreateResultFile(ImageHandle, &File, FileName);

	for (UINTN P = 0; P < Mmap[I].NumberOfPages; P++) {
		UINT64 *Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		for (UINTN Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			if (CurrentFileSize >= MAX_FILE_SIZE){
				FinalizeResults(File);
				GetFileName(FileName, Mmap[I].PhysicalStart + P * PAGE_SIZE);
				CreateResultFile(ImageHandle, &File, FileName);
				CurrentFileSize = 0;
			}

			WriteByte(File, (UINT8)(*Ptr));
			Ptr++;
			CurrentFileSize++;
		}
		PagesDone++;
		ShowProgress();
	}

	FinalizeResults(File);
}


/* No EFIAPI here. Not sure why, but gnu-efi converts this to SysV */
EFI_STATUS
efi_main (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_STATUS Status = EFI_SUCCESS;
	EFI_INPUT_KEY Key;

	InitializeLib(ImageHandle, SystemTable);

	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

	/* Disable watchdog so it won't reboot the platform after 20 minutes. */
	Status = uefi_call_wrapper(gBS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
	if (Status != EFI_SUCCESS) {
		Print(L"Error disabling the watchdog: %r\n", Status);
		return Status;
	}

	Print(L"Application for dumping RAM\n");

	InitMemmap();

	Print(L"\n\nDumping memory...\n");
	UpdateTotalPages();

	for (UINTN I = 0; I < MmapEntries; I++) {
		DumpOneEntry(ImageHandle, I);
	}

	Assert (Status == EFI_SUCCESS);
	Print(L"\nMemory dump done\n");

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
