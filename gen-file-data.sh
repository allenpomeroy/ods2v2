echo
while read file
do
  echo "---"
  echo "$file contents:"
  cat $file
  echo "---"
done < file-list

