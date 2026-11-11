# tests for issues 13, 14, 15, 16

[[ ":$PATH:" != *":$HOME/ods2v2:"* ]] && export PATH="$PATH:$HOME/ods2v2"

if command -v ods2 >/dev/null 2>&1; then
  echo "ods2 found"
else
  echo "ods2 not found"
fi

# if command line arg exists, use as target disk image
if [ $# -gt 0 ]; then
  diskimage="$1"
else
  diskimage="transfer.dsk"
fi

if [ ! -r ${diskimage} ]; then
  echo "Disk image ${diskimage} not found or not readable"
  exit 1
fi

mkdir -p t || exit 1
printf '$ WRITE SYS$OUTPUT "Hello from VAR/CR"\n$ EXIT\n' > t/hello.com
printf 'crlf line\r\n\r\nodd\nno final newline' > t/mixed.txt
: > t/empty.txt
awk 'BEGIN{for(i=1;i<=2000;i++){s="";for(j=0;j<i%300;j++)s=s"x";print i": "s}}' > t/lengths.txt
head -c 500000000 /dev/urandom > t/big.bin

ods2 ${diskimage} CREATE/DIRECTORY [TEST]
ods2 ${diskimage} COPY/CONVERT t/hello.com [TEST]HELLO.COM
ods2 ${diskimage} COPY/CONVERT t/mixed.txt [TEST]MIXED.TXT
ods2 ${diskimage} COPY/CONVERT t/empty.txt [TEST]EMPTY.TXT
ods2 ${diskimage} COPY/CONVERT t/lengths.txt [TEST]LENGTHS.TXT
# Stream_LF, to compare
ods2 ${diskimage} COPY t/lengths.txt [TEST]LENGTHS_STMLF.TXT
ods2 ${diskimage} COPY t/big.bin [TEST]BIG.BIN
cat t/big.bin | ods2 ${diskimage} COPY /dev/stdin [TEST]BIGPIPE.BIN
for i in $(seq 1 150)
do
  ods2 ${diskimage} COPY t/hello.com "[TEST]F$i.COM" >/dev/null
done

echo
echo "Remember you need to clean up directory \"t\" yourself"
