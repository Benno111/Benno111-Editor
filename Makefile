editor:
	windres resource.rc -O coff -o resource.o
	cc -O2 -Wall -Wextra -std=c11 -mwindows editor.c resource.o -o editor.exe -lcomdlg32
