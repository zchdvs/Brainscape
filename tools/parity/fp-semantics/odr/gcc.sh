cd /w
g++ -O2 -std=c++17 -ffp-contract=off -c tu_engine.cpp -o eng.o
g++ -O2 -std=c++17 -mfma -c tu_plugin.cpp -o plg.o
g++ -O2 -c main.cpp -o main.o
g++ main.o eng.o plg.o -o a1 && echo "gcc link eng first: $(./a1)"
g++ main.o plg.o eng.o -o a2 && echo "gcc link plg first: $(./a2)"
g++ -O2 -std=c++17 -ffp-contract=off -flto -c tu_engine.cpp -o engl.o
g++ -O2 -std=c++17 -mfma -flto -c tu_plugin.cpp -o plgl.o
g++ -O2 -flto main.o plgl.o engl.o -o a3 && echo "gcc LTO plg first: $(./a3)"
g++ -O2 -flto main.o engl.o plgl.o -o a4 && echo "gcc LTO eng first: $(./a4)"
