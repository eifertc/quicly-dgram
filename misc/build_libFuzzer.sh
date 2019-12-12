#!/bin/sh

[ -e libFuzzer.a ] && exit 0
<<<<<<< HEAD
[ -d Fuzzer ] || git clone https://chromium.googlesource.com/chromium/llvm-project/llvm/lib/Fuzzer
cd Fuzzer
git checkout 29d1659edabe4ba2396f9697915bb7a0880cbd2f
cd ..
Fuzzer/build.sh
=======
[ -d Fuzzer ] || git clone https://github.com/llvm-mirror/compiler-rt.git Fuzzer
#git checkout 29d1659edabe4ba2396f9697915bb7a0880cbd2f
Fuzzer/lib/fuzzer/build.sh
>>>>>>> quiclyoriginal/master
