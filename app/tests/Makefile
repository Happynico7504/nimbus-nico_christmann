# Host tests for the portable parts of the network switcher.
#   make -C app/tests                # synthetic checks only
#   make -C app/tests DATA=/some/dir # also run against real 3dsx.2.1.1.zip / sdfiles.tar.gz / latest.json in that dir
DATA ?= .
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O1 -g

host_test: host_test.cpp ../source/net/Archive.cpp ../source/net/Release.cpp ../source/net/Installer.cpp ../source/net/Sources.cpp ../source/net/Manager.cpp ../source/net/Services.cpp ../source/net/Fs.cpp ../source/net/State.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ -lz

test: host_test
	./host_test $(DATA)

# Live test against real GitHub (needs libcurl headers and internet).
LIVE_SRC = live_test.cpp ../source/net/CurlHttp.cpp ../source/net/Archive.cpp ../source/net/Release.cpp ../source/net/Installer.cpp ../source/net/Sources.cpp ../source/net/Manager.cpp ../source/net/Services.cpp ../source/net/Fs.cpp ../source/net/AppUpdate.cpp
live_test: $(LIVE_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lz -lcurl

live: live_test
	./live_test

clean:
	rm -f host_test live_test
.PHONY: test clean
