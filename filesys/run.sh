DEBUGGING=""
DEBUGGING="--gdb"

cd build
rm -f tmp.dsk
pintos-mkdisk tmp.dsk 2
pintos ${DEBUGGING} -v -k -m 20   --fs-disk=tmp.dsk -p tests/filesys/extended/dir-rm-cwd:dir-rm-cwd -p tests/filesys/extended/tar:tar --swap-disk=4 -- -q   -f run dir-rm-cwd 
cd ..
