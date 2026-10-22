if pgrep macapp >/dev/null; then
    pkill macapp
else
    macapp &
fi
