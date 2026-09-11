if pgrep applauncher >/dev/null; then
    pkill applauncher
else
    applauncher &
fi
