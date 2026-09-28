for p in patch/*.patch; do patch -p1 < "$p"; done
