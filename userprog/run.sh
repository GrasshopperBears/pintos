make clean && make && cd build && pintos-mkdisk filesys.dsk 10
pintos --fs-disk filesys.dsk -p tests/userprog/args-single:args-single -- -q -f run 'args-single hi'
cd ..