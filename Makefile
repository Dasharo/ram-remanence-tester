ARCH	:= x86_64
OBJS	:= app.o
TARGET	:= BOOTx64.EFI

# Required packages: gnu-efi-devel, gnu-efi
EFIINC	:= /usr/include/efi
EFILIB	:= /usr/lib

EFIINCS	= -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol
EFI_CRT_OBJS	= $(EFILIB)/crt0-efi-$(ARCH).o
EFI_LDS	= $(EFILIB)/elf_$(ARCH)_efi.lds

CFLAGS		= -g -ffreestanding -fno-stack-protector -fno-stack-check \
                  -fpic -fshort-wchar -mno-red-zone -Wall $(EFIINCS) \
                  -DEFI_FUNCTION_WRAPPER -DGNU_EFI_USE_MS_ABI -O2
LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared \
          -Bsymbolic -L $(EFILIB) $(EFI_CRT_OBJS)

CC	= gcc

.PHONY: all clean

all: $(TARGET)

BOOTx64.so: $(OBJS)
	ld $(OBJS) $(LDFLAGS) -o $@ -lefi -lgnuefi
%.EFI: %.so
	objcopy -j .text -j .sdata -j .data -j .dynamic \
	-j .dynsym -j .dynstr -j .rel -j .rela -j .reloc -j .rodata \
	--target=efi-app-$(ARCH) $^ $@

clean:
	-rm -f *.o
	-rm -f *.so
	-rm -f *.EFI
