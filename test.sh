#!/bin/sh
make
rm -rf output
mkdir output
./gwyexport --f jpg -m --gradient 'Wrapmono' --colormap 'adaptive' --filters 'pc;melc;sr;melc;pc' -o 'output' testfile/test*.flat
