# if pgrep -x skippy-xd >/dev/null; then
#     exit 0
# fi
#
# skippy-xd

if pgrep voverview >/dev/null; then
    pkill voverview
else
    voverview &
fi
