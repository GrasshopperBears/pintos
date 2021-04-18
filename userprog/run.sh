TEST_NAME="fork-once"
FILES=""
# FILES="-p ../../tests/userprog/sample.txt:sample.txt"
ARGS=""
# ARGS="a b c d e f g h i j k l m n o p q r s t u v"
DEBUGGING=""
# DEBUGGING="--gdb"

make clean && make && cd build && pintos-mkdisk filesys.dsk 10
# pintos -v -k -T 60 -m 20   --fs-disk=10 -p tests/userprog/args-none:args-none -- -q   -f run args-none
# pintos ${DEBUGGING} --fs-disk filesys.dsk -p tests/userprog/${TEST_NAME}:${TEST_NAME} ${FILES} -- -q -f run "${TEST_NAME} ${ARGS}"
# pintos -v -k -T 60 -m 20   --fs-disk=10 -p tests/userprog/fork-once:fork-once -- -q   -f run fork-once
# pintos ${DEBUGGING} -v -k -T 60 -m 20   --fs-disk=10 -p tests/userprog/fork-multiple:fork-multiple -- -q   -f run fork-multiple
# pintos ${DEBUGGING} -v -k -T 300 -m 20   --fs-disk=10 -p tests/filesys/base/syn-read:syn-read -p tests/filesys/base/child-syn-read:child-syn-read -- -q   -f run syn-read
pintos ${DEBUGGING} -v -k -T 600 -m 20 -m 20   --fs-disk=10 -p tests/userprog/no-vm/multi-oom:multi-oom -- -q   -f run multi-oom
# pintos ${DEBUGGING} -v -k -T 60 -m 20   --fs-disk=10  -- -q  -threads-tests -f run priority-donate-multiple2
# pintos ${DEBUGGING} -v -k -T 60 -m 20   --fs-disk=10 -p tests/userprog/dup2/dup2-simple:dup2-simple -p ../../tests/userprog/dup2/sample.txt:sample.txt -- -q   -f run dup2-simple
# pintos ${DEBUGGING} -v -k -T 60 -m 20   --fs-disk=10 -p tests/userprog/dup2/dup2-complex:dup2-complex -p ../../tests/userprog/dup2/sample.txt:sample.txt -- -q   -f run dup2-complex
cd ..
