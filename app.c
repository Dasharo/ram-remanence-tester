#include <efi.h>
#include <efilib.h>

/* https://github.com/ncroxon/gnu-efi/issues/63 */
static UINTN AsciiVSPrint_fixed (
    OUT CHAR8         *Str,
    IN UINTN          StrSize,
    IN CONST CHAR8    *fmt,
    va_list           args
)
{
	CHAR16 *UnicodeStr, *UnicodeFmt;
	UINTN i, Len;

	UnicodeStr = AllocatePool(StrSize * sizeof(CHAR16));
	if (!UnicodeStr)
		return 0;

	UnicodeFmt = PoolPrint(L"%a", fmt);
	if (!UnicodeFmt) {
		FreePool(UnicodeStr);
		return 0;
	}

	Len = UnicodeVSPrint(UnicodeStr, StrSize * sizeof(CHAR16), UnicodeFmt, args);
	FreePool(UnicodeFmt);

	// The strings are ASCII so just do a plain Unicode conversion
	for (i = 0; i < Len; i++)
		Str[i] = (CHAR8)UnicodeStr[i];
	Str[Len] = 0;
	FreePool(UnicodeStr);

	return Len;
}

/* This isn't part of gnu-efi, and AsciiVSPrint has some issues. */
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
	len = AsciiVSPrint_fixed(Str, StrSize, fmt, args);
	va_end (args);

	/*
	 * Every '\n' is changed to '\r\r\n' by AsciiVSPrint() - one '\r' is added
	 * when converting format to Unicode with PoolPrint(), the other when
	 * UnicodeVSPrint() is invoked. Code below fixes the trailing '\r\r\n', but
	 * internal or multiple newline characters aren't handled.
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

	UpdateTotalPages();
	Print(L"Found %lld pages of available RAM (%lld MB)\n",
		  TotalPages, TotalPages >> 8);
}

static UINT64 Differences = 0;
static UINT64 Compared = 0;
static UINT64 OneToZero[64];
static UINT64 ZeroToOne[64];

static VOID GetFileName(CHAR16 *Name, UINT64 AddressStart, UINT64 AddressEnd)
{
	EFI_TIME Time;

	uefi_call_wrapper(gRT->GetTime, 2, &Time, NULL);

	UnicodeSPrint(Name, 0, L"%04d_%02d_%02d_%02d_%02d_0x%16llx-0x%16llx.csv",
	              Time.Year, Time.Month, Time.Day,
	              Time.Hour, Time.Minute, AddressStart, AddressEnd);
}

static VOID CreateResultFile(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **File, CHAR16 *Name)
{
	EFI_LOADED_IMAGE *Loaded = NULL;
	EFI_FILE_PROTOCOL *Root = NULL;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs = NULL;
	EFI_STATUS Status;
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

	Status = uefi_call_wrapper(Root->Open, 5, Root, file, Name,
	                           EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE |
	                           EFI_FILE_MODE_READ, 0);
	Assert(*file != NULL);
}

static VOID AddResultLine(EFI_FILE_PROTOCOL *File, UINT8 Byte)
{
	UINTN Len = (UINTN)sizeof(UINT8);
	EFI_STATUS Status;

	Status = uefi_call_wrapper(Csv->Write, 3, File, &Len, Byte);
	Assert(Status == EFI_SUCCESS);
}

static VOID FinalizeResults(EFI_FILE_PROTOCOL *Csv)
{
	EFI_STATUS Status;

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
	CHAR16 FileName[50];
	EFI_FILE_PROTOCOL *File = NULL;

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
	VarSize = sizeof(Mmap);
	Status = uefi_call_wrapper(gRT->GetVariable, 5, VarName, &VarGuid,
		                           NULL, &VarSize, Mmap);
	Assert (Status == EFI_SUCCESS);
	Assert (VarSize % sizeof(EFI_MEMORY_DESCRIPTOR) == 0);
	MmapEntries = VarSize / sizeof(EFI_MEMORY_DESCRIPTOR);
	UpdateTotalPages();

	for (UINTN I = 0; I < MmapEntries; I++) {
		DumpOneEntry(I);
	}

	Status = uefi_call_wrapper(gRT->SetVariable, 5, VarName, &VarGuid,
		                           0, 0, NULL);
	Assert (Status == EFI_SUCCESS);
	Print(L"\nMemory dump done\n");

	/*
	 * We no longer care about memory map or preservation of memory. Safe
	 * to use firmware services again at this point.
	 */
	CreateResultFile(ImageHandle, &Csv);

	FinalizeResults(Csv);

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
