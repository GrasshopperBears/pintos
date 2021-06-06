DEBUGGING=""
# DEBUGGING="--gdb"

cd build
rm -f tmp.dsk
pintos-mkdisk tmp.dsk 2
pintos -v -k -m 20   --fs-disk=tmp.dsk -p tests/filesys/extended/dir-empty-name:dir-empty-name -p tests/filesys/extended/tar:tar --swap-disk=4 -- -q   -f run dir-empty-name 
pintos ${DEBUGGING} -v -k   --fs-disk=tmp.dsk -g fs.tar:tests/filesys/extended/dir-empty-name.tar --swap-disk=4 -- -q  run 'tar fs.tar /'
rm -f tmp.dsk
# rm -f tmp.dsk
# perl -I../.. ../../tests/filesys/extended/dir-empty-name-persistence.ck tests/filesys/extended/dir-empty-name-persistence tests/filesys/extended/dir-empty-name-persistence.result
cd ..
