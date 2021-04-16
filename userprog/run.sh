
TEST_NAME="open-twice"
FILES=""
FILES="-p ../../tests/userprog/sample.txt:sample.txt"
ARGS=""
# ARGS="a b c d e f g h i j k l m n o p q r s t u v"
DEBUGGING=""
# DEBUGGING="--gdb"

make clean && make && cd build && pintos-mkdisk filesys.dsk 10
pintos ${DEBUGGING} --fs-disk filesys.dsk -p tests/userprog/${TEST_NAME}:${TEST_NAME} ${FILES} -- -q -f run "${TEST_NAME} ${ARGS}"
# pintos -v -k -T 300 -m 20   --fs-disk=10 -p tests/filesys/base/syn-read:syn-read -p tests/filesys/base/child-syn-read:child-syn-read -- -q   -f run syn-read
# pintos -v -k -T 60 -m 20   --fs-disk=10 -p tests/userprog/create-bad-ptr:create-bad-ptr -- -q   -f run create-bad-ptr
cd ..

# make clean && make && cd build && pintos-mkdisk filesys.dsk 10
# cd build
# pintos --fs-disk filesys.dsk -p tests/filesys/base/syn-read:syn-read -- -q -f run 'syn-read'
# pintos --fs-disk filesys.dsk -p tests/filesys/base/syn-write:syn-write -- -q -f run 'syn-write'
# pintos --fs-disk filesys.dsk -p tests/userprog/args-none:args-none -- -q -f run 'args-none'
# pintos --fs-disk filesys.dsk -p tests/userprog/rox-simple:rox-simple -- -q -f run 'rox-simple'
# pintos --fs-disk filesys.dsk -p tests/userprog/read-bad-ptr:read-bad-ptr -- -q -f run 'read-bad-ptr'
# pintos --fs-disk filesys.dsk -p tests/userprog/create-long:create-long -- -q -f run create-long
# pintos --fs-disk filesys.dsk -p tests/userprog/no-vm/multi-oom:multi-oom -- -q -f run multi-oom
# cd ..