sudo apt-get install --reinstall nlohmann-json3-dev

rm -r build
mkdir build
cd build
cmake ..
cmake --build .