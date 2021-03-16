cd build
cnt=0
for iteration in {1..50}
do
  output=$(pintos -- -q run alarm-multiple)
  case "$output" in
    *"(alarm-multiple) end"*) echo "PASSED" ;;
    *) cnt=($cnt + 1) ;;
  esac
done
echo "RESULT: $cnt test failed"