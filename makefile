 
all:init_disk UFS
init_disk: init_disk.o
	gcc init_disk.o -o init_disk
UFS: UFS.o
	gcc UFS.o -o UFS -Wall -D_FILE_OFFSET_BITS=64 -g -pthread -lfuse3 -lrt -ldl
UFS.o: UFS.c
	gcc -Wall -D_FILE_OFFSET_BITS=64 -g -c -o UFS.o UFS.c
init_disk.o: init_disk.c
	gcc -Wall -D_FILE_OFFSET_BITS=64 -g -c -o init_disk.o init_disk.c
.PHONY : all
clean :
	rm -f UFS init_disk UFS.o init_disk.o
