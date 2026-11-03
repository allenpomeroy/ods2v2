# tests for issues 13, 14, 15, 16

mkdir -p t
printf '$ WRITE SYS$OUTPUT "Hello from VAR/CR"\n$ EXIT\n' > t/hello.com
printf 'crlf line\r\n\r\nodd\nno final newline' > t/mixed.txt
: > t/empty.txt
awk 'BEGIN{for(i=1;i<=2000;i++){s="";for(j=0;j<i%300;j++)s=s"x";print i": "s}}' > t/lengths.txt
head -c 500000000 /dev/urandom > t/big.bin

./ods2 transfer.dsk CREATE/DIRECTORY [TEST]
./ods2 transfer.dsk COPY/CONVERT t/hello.com [TEST]HELLO.COM
./ods2 transfer.dsk COPY/CONVERT t/mixed.txt [TEST]MIXED.TXT
./ods2 transfer.dsk COPY/CONVERT t/empty.txt [TEST]EMPTY.TXT
./ods2 transfer.dsk COPY/CONVERT t/lengths.txt [TEST]LENGTHS.TXT
./ods2 transfer.dsk COPY t/lengths.txt [TEST]LENGTHS_STMLF.TXT   # Stream_LF, to compare
./ods2 transfer.dsk COPY t/big.bin [TEST]BIG.BIN
cat t/big.bin | ./ods2 transfer.dsk COPY /dev/stdin [TEST]BIGPIPE.BIN
for i in $(seq 1 150); do ./ods2 transfer.dsk COPY t/hello.com "[TEST]F$i.COM" >/dev/null; done

echo
echo "remember you need to clean up directory \"t\" yourself"
