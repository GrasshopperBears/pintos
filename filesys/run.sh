cd build && make clean && make && pintos -v -k -T 60 -m 20   --fs-disk=10  --swap-disk=4 -- -q  -threads-tests -f run alarm-multiple