for iteration in {1}
do
  make clean && make && cd build
  output=$(make check)
  case "$output" in
    *"All 27 tests passed"*) echo "PASSED" ;;
    *) cnt=$((cnt+1)) ;;
  esac
  cd ..
done
echo "RESULT: $cnt test failed"