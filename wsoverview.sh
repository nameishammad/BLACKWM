# if pgrep -x skippy-xd >/dev/null; then
#     exit 0
# fi
#
# skippy-xd

if pgrep wsoverview >/dev/null; then
    pkill wsoverview
else
    wsoverview &
fi
