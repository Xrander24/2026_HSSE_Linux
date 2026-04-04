savedcmd_tg.ko := ld -r -m elf_x86_64 --fatal-warnings -z noexecstack --build-id=sha1  -T /home/xrander24/HSSE/2026_linux/linux-6.19/scripts/module.lds -o tg.ko tg.o tg.mod.o .module-common.o
