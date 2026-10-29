#!/bin/sh
# Installa fosid come LaunchAgent (parte da solo al login).  Disinstallare: ./install.sh uninstall
set -e
cd "$(dirname "$0")"

LABEL=com.user.fosid
DIR="$HOME/Library/Application Support/fosid"
BIN="$DIR/fosid"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/fosid.log"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true

if [ "$1" = uninstall ]; then
    rm -f "$PLIST" && rm -rf "$DIR"
    echo "fosid rimosso (ricordati di toglierlo da Impostazioni > Privacy > Monitoraggio input)"
    exit 0
fi

mkdir -p "$DIR" "$HOME/Library/LaunchAgents"
clang -O2 -Wall -o "$BIN" fosid.c -framework IOKit -framework CoreFoundation -framework CoreAudio
codesign -s - -f -i "$LABEL" "$BIN"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>             <string>$LABEL</string>
    <key>ProgramArguments</key>  <array><string>$BIN</string></array>
    <key>RunAtLoad</key>         <true/>
    <key>KeepAlive</key>         <true/>
    <key>ThrottleInterval</key>  <integer>10</integer>
    <key>ProcessType</key>       <string>Interactive</string>
    <key>StandardErrorPath</key> <string>$LOG</string>
</dict>
</plist>
EOF

launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "fosid installato. Log: $LOG"
echo "Al primo avvio macOS chiede 'Monitoraggio input' per fosid: consentilo (Impostazioni > Privacy e sicurezza)."
