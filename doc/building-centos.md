## Building Seastar on CentOS

### Building seastar on CentOS 7

Installing required packages:
```
sudo ./install-dependencies.sh
./cooking.sh -r dev -i 'c-ares;fmt'
```

To compile Seastar explicitly using gcc 5, use:
```
CXX=/opt/scylladb/bin/g++ ./cooking.sh -r dev -i 'c-ares;fmt'
ninja-build -C build
```
