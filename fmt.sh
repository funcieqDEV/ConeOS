#!/bin/bash

echo "Formatting all .c and .h files in src/ (4 spaces indent)..."

# Szuka wszystkich plików .c oraz .h w katalogu src i jego podkatalogach
# i odpala na nich clang-format.
# -i oznacza "in-place", czyli nadpisuje pliki sformatowaną wersją.
find src -name '*.c' -o -name '*.h' | xargs clang-format -i

echo "Formatting complete!"
