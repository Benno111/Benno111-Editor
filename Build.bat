@echo off
windres resource.rc -O coff -o resource.o
gcc -O2 -Wall -Wextra -std=c11 -mwindows editor.c resource.o -o editor.exe -lcomdlg32 -ld2d1 -luuid -lole32
