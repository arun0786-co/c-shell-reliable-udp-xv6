#!/usr/bin/env bash
# Tests for S.H.A.M. Runs the real server and client on 127.0.0.1 in a
# scratch directory. Usage: ./test.sh (after make)
here=$(cd "$(dirname "$0")" && pwd)
server=$here/server
client=$here/client
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap '' PIPE # a peer that died early must fail a test, not kill the script
cd "$work" || exit 1
port=$((20000 + RANDOM % 20000))
pass=0
fail=0

ok() { pass=$((pass + 1)); echo "ok   $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1"; shift; for l in "$@"; do echo "     $l"; done; }

md5_of() {
    if command -v md5sum > /dev/null; then md5sum < "$1" | cut -d' ' -f1; else md5 -q "$1"; fi
}

# runs a command with a time limit; the exit status is 137 if it was killed
limit() {
    local secs=$1; shift
    # without an explicit <&0, bash gives background jobs /dev/null as stdin
    "$@" <&0 & local pid=$!
    ( sleep "$secs"; kill -9 $pid 2> /dev/null ) & local dog=$!
    wait $pid; local st=$?
    kill $dog 2> /dev/null; wait $dog 2> /dev/null
    return $st
}

next_port() { port=$((port + 1)); }

# transfer <name> <size> <server loss> [time limit]
transfer() {
    local name=$1 size=$2 loss=$3 secs=${4:-30}
    next_port
    rm -rf t && mkdir t && cd t || return
    head -c "$size" /dev/urandom > input.bin 2> /dev/null # macOS head refuses -c 0
    limit "$secs" "$server" $port $loss > srv.out 2> srv.err & local sp=$!
    sleep 0.2
    limit "$secs" "$client" 127.0.0.1 $port input.bin copy.bin > cli.out 2> cli.err
    local cst=$?
    wait $sp; local sst=$?
    local want="MD5: $(md5_of input.bin)"
    if [ $sst -ne 0 ] || [ $cst -ne 0 ]; then
        bad "$name" "server exit $sst, client exit $cst" "$(cat srv.err cli.err)"
    elif [ "$(cat srv.out)" != "$want" ]; then
        bad "$name" "stdout: $(cat srv.out)" "wanted: $want"
    elif ! cmp -s input.bin copy.bin; then
        bad "$name" "copy.bin differs from input.bin"
    elif [ -e copy.bin.part ]; then
        bad "$name" "copy.bin.part was left behind"
    else
        ok "$name ($(cat cli.err))"
    fi
    cd ..
}

echo "== file transfer"
transfer "empty file" 0 0
transfer "1 byte" 1 0
transfer "exactly one packet" 1024 0
transfer "5000 bytes" 5000 0
transfer "1 MiB" 1048576 0
transfer "1 MiB, 10% loss" 1048576 0.1 60
transfer "200 KB, 30% loss" 200000 0.3 60
transfer "50 KB, 50% loss" 50000 0.5 60

echo "== logging"
next_port
rm -rf t && mkdir t && cd t
head -c 300000 /dev/urandom > input.bin
"$server" $port > /dev/null 2>&1 & sp=$!
sleep 0.2
"$client" 127.0.0.1 $port input.bin copy.bin > /dev/null 2>&1
wait $sp
if [ -e server_log.txt ] || [ -e client_log.txt ]; then
    bad "no log files without RUDP_LOG=1"
else
    ok "no log files without RUDP_LOG=1"
fi
next_port
RUDP_LOG=1 limit 60 "$server" $port 0.2 > /dev/null 2>&1 & sp=$!
sleep 0.2
RUDP_LOG=1 limit 60 "$client" 127.0.0.1 $port input.bin copy.bin > /dev/null 2>&1
wait $sp
stamp='^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\] \[LOG\] [A-Z]'
badlines=$(cat server_log.txt client_log.txt | grep -Ecv "$stamp")
if [ "$badlines" = 0 ]; then ok "every log line has the timestamp format"; else bad "log format" "$badlines bad lines"; fi
missing=
for ev in "SND SYN SEQ=" "SND ACK FOR SYN" "SND DATA SEQ=[0-9]+ LEN=" "RCV ACK=" \
          "TIMEOUT SEQ=" "RETX DATA SEQ=[0-9]+ LEN=" "FLOW WIN UPDATE=" "SND FIN SEQ=" \
          "RCV FIN SEQ=" "SND ACK FOR FIN"; do
    grep -Eq "\] \[LOG\] $ev" client_log.txt || missing="$missing client:'$ev'"
done
for ev in "RCV SYN SEQ=" "SND SYN-ACK SEQ=[0-9]+ ACK=" "RCV ACK FOR SYN" "RCV DATA SEQ=[0-9]+ LEN=" \
          "SND ACK=[0-9]+ WIN=" "DROP DATA SEQ=" "RCV FIN SEQ=" "SND ACK FOR FIN" "SND FIN SEQ=" "RCV ACK="; do
    grep -Eq "\] \[LOG\] $ev" server_log.txt || missing="$missing server:'$ev'"
done
if [ -z "$missing" ]; then ok "all required events logged"; else bad "required events" "missing:$missing"; fi
# replay the client log: bytes in flight never exceed 10 packets or the
# window the server last advertised
over=$(sed -E 's/^\[[^]]*\] \[LOG\] //' client_log.txt | awk '
    /^RCV SYN-ACK/ { split($4, a, "="); acked = a[2]; win = 16384; next }
    /^RCV ACK=/ { split($2, a, "="); d = a[2] - acked; if (d < -2147483648) d += 4294967296; if (d > 0) acked = a[2]; next }
    /^FLOW WIN UPDATE=/ { split($3, a, "="); win = a[2]; next }
    /^SND DATA/ { split($3, s, "="); split($4, l, "="); f = s[2] + l[2] - acked;
                  if (f < 0) f += 4294967296; if (f > 10240 || (f > l[2] && f > win)) n++ }
    END { print n + 0 }')
if [ "$over" = 0 ]; then ok "in-flight data stayed within both windows"; else bad "window limits" "$over sends over the limit"; fi
cd ..

echo "== chat"
# chat <name> <who quits: client|server|eof> <loss>
chat() {
    local name=$1 quitter=$2 loss=$3
    next_port
    rm -rf t && mkdir t && cd t || return
    mkfifo sin cin
    limit 60 "$server" $port --chat $loss < sin > srv.out 2> srv.err & local sp=$!
    exec 3> sin
    sleep 0.2
    limit 60 "$client" 127.0.0.1 $port --chat $loss < cin > cli.out 2> cli.err & local cp=$!
    exec 4> cin
    sleep 0.3
    for i in 1 2 3 4 5 6 7 8 9 10; do
        echo "from client $i" >&4
        echo "from server $i" >&3
    done
    sleep 0.2
    case $quitter in
        client) echo /quit >&4 ;;
        server) echo /quit >&3 ;;
        eof) exec 4>&- ;;
    esac
    wait $cp; local cst=$?
    wait $sp; local sst=$?
    exec 3>&- 4>&-
    local want_s want_c
    want_s=$(for i in 1 2 3 4 5 6 7 8 9 10; do echo "[Client]: from client $i"; done)
    want_c=$(for i in 1 2 3 4 5 6 7 8 9 10; do echo "[Server]: from server $i"; done)
    if [ $sst -ne 0 ] || [ $cst -ne 0 ]; then
        bad "$name" "server exit $sst, client exit $cst" "$(cat srv.err cli.err)"
    elif [ "$(cat srv.out)" != "$want_s" ] || [ "$(cat cli.out)" != "$want_c" ]; then
        bad "$name" "server saw: $(tr '\n' '|' < srv.out)" "client saw: $(tr '\n' '|' < cli.out)"
    else
        ok "$name"
    fi
    cd ..
}
chat "client types /quit" client 0
chat "server types /quit" server 0
chat "end of input closes the chat" eof 0
chat "client /quit, 30% loss both ways" client 0.3

echo "== errors"
next_port
rm -rf t && mkdir t && cd t
echo hi > input.bin
start=$SECONDS
limit 20 "$client" 127.0.0.1 $port input.bin copy.bin > /dev/null 2> cli.err; st=$?
if [ $st -eq 1 ] && [ $((SECONDS - start)) -le 6 ]; then
    ok "no server: client gives up ($(cat cli.err))"
else
    bad "no server" "exit $st after $((SECONDS - start)) s: $(cat cli.err)"
fi
"$client" 127.0.0.1 $port missing.bin copy.bin > /dev/null 2> cli.err; st=$?
if [ $st -eq 1 ]; then ok "missing input file ($(cat cli.err))"; else bad "missing input file" "exit $st"; fi
"$client" 127.0.0.1 $port input.bin copy.bin 1.5 > /dev/null 2>&1; st=$?
"$server" $port abc > /dev/null 2>&1; st2=$?
if [ $st -eq 2 ] && [ $st2 -eq 2 ]; then ok "bad loss rate is refused"; else bad "bad loss rate" "client $st, server $st2"; fi
next_port
limit 20 "$server" $port > srv.out 2> srv.err & sp=$!
sleep 0.2
limit 20 "$client" 127.0.0.1 $port input.bin ../escaped.bin > /dev/null 2>&1
wait $sp
if [ -e ../escaped.bin ] || [ ! -e escaped.bin ]; then
    bad "file name from the network is reduced to its last component"
else
    ok "file name from the network is reduced to its last component"
fi
cd ..

echo "passed $pass, failed $fail"
[ $fail -eq 0 ]
