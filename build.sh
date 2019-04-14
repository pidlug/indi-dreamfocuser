#!/bin/sh

# W tym katalogu umieścimy źródła indi-dreamfocuser'a
D=~/src/dreamfocuser
[ -d "$D" ] || mkdir -p "$D"
cd "$D" || { echo "Nie udało się stworzyć środowiska kompilacji dla DreamFocusera!"; exit 1; }

echo "Potrzebujemy git i zależności budowania dla INDI"
sudo apt install git
sudo apt build-dep libindi

git clone https://github.com/pidlug/indi-dreamfocuser.git
cd indi-dreamfocuser
git pull
cd ..

[ -d build ] && rm -R build
mkdir build

cd build && \
cmake ../indi-dreamfocuser && \
make && sudo make install
