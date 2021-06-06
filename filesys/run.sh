DEBUGGING=""
# DEBUGGING="--gdb"

cd build
pintos ${DEBUGGING}  -v -k -m 20   --fs-disk=10 -p tests/userprog/args-none:args-none --swap-disk=4 -- -q   -f run args-none
# rm -f tmp.dsk
# pintos-mkdisk tmp.dsk 2
# pintos ${DEBUGGING} -v -k -m 20   --fs-disk=tmp.dsk -p tests/filesys/extended/dir-rm-cwd:dir-rm-cwd -p tests/filesys/extended/tar:tar --swap-disk=4 -- -q   -f run dir-rm-cwd 
cd ..
